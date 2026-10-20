// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/nc_ledger.h"

#include <string.h>

#include "iree/base/internal/atomics.h"
#include "iree/base/threading/mutex.h"

#define IREE_HAL_STREAMING_NC_LEDGER_GRANULE_SHIFT 21
#define IREE_HAL_STREAMING_NC_LEDGER_FILTER_BITS_LOG2 16
#define IREE_HAL_STREAMING_NC_LEDGER_FILTER_WORDS \
  ((1u << IREE_HAL_STREAMING_NC_LEDGER_FILTER_BITS_LOG2) / 64)

typedef struct iree_hal_streaming_nc_range_t {
  uint64_t base;
  uint64_t end;
  // Largest |end| of this and every lower-based range; bounds the backward
  // walk when ranges overlap.
  uint64_t max_end;
  iree_hal_streaming_nc_range_kind_t kind;
} iree_hal_streaming_nc_range_t;

struct iree_hal_streaming_nc_ledger_t {
  iree_allocator_t host_allocator;
  iree_slim_mutex_t mutex;
  // Sorted by base. Guarded by |mutex|.
  iree_hal_streaming_nc_range_t* ranges;
  iree_host_size_t capacity;
  iree_host_size_t count;
  // Mirrors |count| for the lock-free empty check.
  iree_atomic_int64_t published_count;
  // Bit per hashed 2 MiB granule covered by some range. Bits may be stale-set
  // after a removal is racing a lookup, never stale-clear for a live range.
  iree_atomic_int64_t filter[IREE_HAL_STREAMING_NC_LEDGER_FILTER_WORDS];
};

const char* iree_hal_streaming_nc_range_kind_name(
    iree_hal_streaming_nc_range_kind_t kind) {
  switch (kind) {
    case IREE_HAL_STREAMING_NC_RANGE_KIND_IPC_IMPORT:
      return "ipc";
    case IREE_HAL_STREAMING_NC_RANGE_KIND_HOST:
      return "host";
    case IREE_HAL_STREAMING_NC_RANGE_KIND_INDIRECT:
      return "indirect";
    default:
      return "unknown";
  }
}

static inline uint32_t iree_hal_streaming_nc_ledger_filter_bit(
    uint64_t granule) {
  return (uint32_t)((granule * 0x9E3779B97F4A7C15ull) >>
                    (64 - IREE_HAL_STREAMING_NC_LEDGER_FILTER_BITS_LOG2));
}

static inline bool iree_hal_streaming_nc_ledger_filter_test(
    iree_hal_streaming_nc_ledger_t* ledger, uint64_t address) {
  const uint32_t bit = iree_hal_streaming_nc_ledger_filter_bit(
      address >> IREE_HAL_STREAMING_NC_LEDGER_GRANULE_SHIFT);
  const int64_t word =
      iree_atomic_load(&ledger->filter[bit / 64], iree_memory_order_relaxed);
  return ((uint64_t)word >> (bit % 64)) & 1;
}

// Sets the filter bits of [base, end) in |words|.
static void iree_hal_streaming_nc_ledger_filter_fill(uint64_t base,
                                                     uint64_t end,
                                                     uint64_t* words) {
  const uint64_t first = base >> IREE_HAL_STREAMING_NC_LEDGER_GRANULE_SHIFT;
  const uint64_t last = (end - 1) >> IREE_HAL_STREAMING_NC_LEDGER_GRANULE_SHIFT;
  if (last - first >= (1u << IREE_HAL_STREAMING_NC_LEDGER_FILTER_BITS_LOG2)) {
    memset(words, 0xFF,
           IREE_HAL_STREAMING_NC_LEDGER_FILTER_WORDS * sizeof(uint64_t));
    return;
  }
  for (uint64_t granule = first; granule <= last; ++granule) {
    const uint32_t bit = iree_hal_streaming_nc_ledger_filter_bit(granule);
    words[bit / 64] |= 1ull << (bit % 64);
  }
}

static void iree_hal_streaming_nc_ledger_update_max_end(
    iree_hal_streaming_nc_ledger_t* ledger, iree_host_size_t first) {
  uint64_t max_end = first ? ledger->ranges[first - 1].max_end : 0;
  for (iree_host_size_t i = first; i < ledger->count; ++i) {
    max_end = iree_max(max_end, ledger->ranges[i].end);
    ledger->ranges[i].max_end = max_end;
  }
}

// Returns the number of ranges whose base is <= |address|.
static iree_host_size_t iree_hal_streaming_nc_ledger_upper_bound(
    const iree_hal_streaming_nc_ledger_t* ledger, uint64_t address) {
  iree_host_size_t low = 0;
  iree_host_size_t high = ledger->count;
  while (low < high) {
    const iree_host_size_t mid = low + (high - low) / 2;
    if (ledger->ranges[mid].base <= address) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return low;
}

iree_status_t iree_hal_streaming_nc_ledger_create(
    iree_allocator_t host_allocator,
    iree_hal_streaming_nc_ledger_t** out_ledger) {
  IREE_ASSERT_ARGUMENT(out_ledger);
  *out_ledger = NULL;
  iree_hal_streaming_nc_ledger_t* ledger = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, sizeof(*ledger), (void**)&ledger));
  memset(ledger, 0, sizeof(*ledger));
  ledger->host_allocator = host_allocator;
  iree_slim_mutex_initialize(&ledger->mutex);
  *out_ledger = ledger;
  return iree_ok_status();
}

void iree_hal_streaming_nc_ledger_destroy(
    iree_hal_streaming_nc_ledger_t* ledger) {
  if (!ledger) {
    return;
  }
  iree_allocator_free(ledger->host_allocator, ledger->ranges);
  iree_slim_mutex_deinitialize(&ledger->mutex);
  iree_allocator_free(ledger->host_allocator, ledger);
}

iree_status_t iree_hal_streaming_nc_ledger_insert(
    iree_hal_streaming_nc_ledger_t* ledger, uint64_t base, uint64_t size,
    iree_hal_streaming_nc_range_kind_t kind) {
  IREE_ASSERT_ARGUMENT(ledger);
  if (size == 0) {
    return iree_ok_status();
  }
  const uint64_t end = base + size < base ? UINT64_MAX : base + size;

  iree_slim_mutex_lock(&ledger->mutex);
  iree_host_size_t index =
      iree_hal_streaming_nc_ledger_upper_bound(ledger, base);
  const bool replace = index > 0 && ledger->ranges[index - 1].base == base;
  if (replace) {
    --index;
  } else {
    if (ledger->count == ledger->capacity) {
      const iree_host_size_t new_capacity =
          ledger->capacity ? ledger->capacity * 2 : 64;
      iree_status_t status = iree_allocator_realloc(
          ledger->host_allocator, new_capacity * sizeof(ledger->ranges[0]),
          (void**)&ledger->ranges);
      if (!iree_status_is_ok(status)) {
        iree_slim_mutex_unlock(&ledger->mutex);
        return status;
      }
      ledger->capacity = new_capacity;
    }
    memmove(&ledger->ranges[index + 1], &ledger->ranges[index],
            (ledger->count - index) * sizeof(ledger->ranges[0]));
    ++ledger->count;
  }
  ledger->ranges[index] = (iree_hal_streaming_nc_range_t){
      .base = base,
      .end = end,
      .kind = kind,
  };
  iree_hal_streaming_nc_ledger_update_max_end(ledger, index);

  uint64_t words[IREE_HAL_STREAMING_NC_LEDGER_FILTER_WORDS] = {0};
  iree_hal_streaming_nc_ledger_filter_fill(base, end, words);
  for (iree_host_size_t i = 0; i < IREE_HAL_STREAMING_NC_LEDGER_FILTER_WORDS;
       ++i) {
    if (words[i]) {
      iree_atomic_fetch_or(&ledger->filter[i], (int64_t)words[i],
                           iree_memory_order_relaxed);
    }
  }
  iree_atomic_store(&ledger->published_count, (int64_t)ledger->count,
                    iree_memory_order_release);
  iree_slim_mutex_unlock(&ledger->mutex);
  return iree_ok_status();
}

void iree_hal_streaming_nc_ledger_remove(iree_hal_streaming_nc_ledger_t* ledger,
                                         uint64_t base) {
  IREE_ASSERT_ARGUMENT(ledger);
  iree_slim_mutex_lock(&ledger->mutex);
  const iree_host_size_t upper =
      iree_hal_streaming_nc_ledger_upper_bound(ledger, base);
  if (upper == 0 || ledger->ranges[upper - 1].base != base) {
    iree_slim_mutex_unlock(&ledger->mutex);
    return;
  }
  const iree_host_size_t index = upper - 1;
  memmove(&ledger->ranges[index], &ledger->ranges[index + 1],
          (ledger->count - index - 1) * sizeof(ledger->ranges[0]));
  --ledger->count;
  iree_hal_streaming_nc_ledger_update_max_end(ledger, index);

  // Rebuild, then store word by word: a live range's bit is set in both the
  // old and the new word, so a concurrent lookup never sees it clear.
  uint64_t words[IREE_HAL_STREAMING_NC_LEDGER_FILTER_WORDS] = {0};
  for (iree_host_size_t i = 0; i < ledger->count; ++i) {
    iree_hal_streaming_nc_ledger_filter_fill(ledger->ranges[i].base,
                                             ledger->ranges[i].end, words);
  }
  for (iree_host_size_t i = 0; i < IREE_HAL_STREAMING_NC_LEDGER_FILTER_WORDS;
       ++i) {
    iree_atomic_store(&ledger->filter[i], (int64_t)words[i],
                      iree_memory_order_relaxed);
  }
  iree_atomic_store(&ledger->published_count, (int64_t)ledger->count,
                    iree_memory_order_release);
  iree_slim_mutex_unlock(&ledger->mutex);
}

iree_host_size_t iree_hal_streaming_nc_ledger_count(
    iree_hal_streaming_nc_ledger_t* ledger) {
  return (iree_host_size_t)iree_atomic_load(&ledger->published_count,
                                            iree_memory_order_acquire);
}

static bool iree_hal_streaming_nc_ledger_lookup_locked(
    const iree_hal_streaming_nc_ledger_t* ledger, uint64_t address,
    iree_hal_streaming_nc_range_kind_t* out_kind) {
  iree_host_size_t i =
      iree_hal_streaming_nc_ledger_upper_bound(ledger, address);
  while (i > 0 && ledger->ranges[i - 1].max_end > address) {
    --i;
    if (address < ledger->ranges[i].end) {
      if (out_kind) {
        *out_kind = ledger->ranges[i].kind;
      }
      return true;
    }
  }
  return false;
}

bool iree_hal_streaming_nc_ledger_lookup(
    iree_hal_streaming_nc_ledger_t* ledger, uint64_t address,
    iree_hal_streaming_nc_range_kind_t* out_kind) {
  if (!ledger || iree_hal_streaming_nc_ledger_count(ledger) == 0) {
    return false;
  }
  if (!iree_hal_streaming_nc_ledger_filter_test(ledger, address)) {
    return false;
  }
  iree_slim_mutex_lock(&ledger->mutex);
  const bool hit =
      iree_hal_streaming_nc_ledger_lookup_locked(ledger, address, out_kind);
  iree_slim_mutex_unlock(&ledger->mutex);
  return hit;
}

bool iree_hal_streaming_nc_ledger_scan(
    iree_hal_streaming_nc_ledger_t* ledger, iree_const_byte_span_t data,
    iree_hal_streaming_nc_range_kind_t* out_kind) {
  if (!ledger || iree_hal_streaming_nc_ledger_count(ledger) == 0) {
    return false;
  }
  const iree_host_size_t word_count = data.data_length / sizeof(uint64_t);
  for (iree_host_size_t i = 0; i < word_count; ++i) {
    uint64_t value = 0;
    memcpy(&value, data.data + i * sizeof(uint64_t), sizeof(value));
    if (value == 0) {
      continue;
    }
    if (iree_hal_streaming_nc_ledger_lookup(ledger, value, out_kind)) {
      return true;
    }
  }
  return false;
}
