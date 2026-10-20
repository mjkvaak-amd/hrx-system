// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LIBHRX_SRC_BINDING_COMMON_NC_LEDGER_H_
#define LIBHRX_SRC_BINDING_COMMON_NC_LEDGER_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

//===----------------------------------------------------------------------===//
// iree_hal_streaming_nc_ledger_t
//===----------------------------------------------------------------------===//

// Address ranges that this process maps MTYPE_NC on gfx94x/gfx950 and that an
// agent other than the reading GPU may write while a submission is in flight.
// A dispatch that can reach one of them needs an acquire that invalidates NC
// L2 lines; every other interior agent acquire is redundant.
//
// Lookups take no lock unless a 2 MiB-granule hash filter reports a possible
// hit, so scanning a few dozen kernarg words costs a few nanoseconds in the
// common case. False positives of the filter only cost a locked lookup.
typedef struct iree_hal_streaming_nc_ledger_t iree_hal_streaming_nc_ledger_t;

typedef enum iree_hal_streaming_nc_range_kind_e {
  // Another GPU's memory imported through IPC.
  IREE_HAL_STREAMING_NC_RANGE_KIND_IPC_IMPORT = 0,
  // Host memory (pinned, registered or managed). gfx950 maps it NC and the
  // CPU writes it.
  IREE_HAL_STREAMING_NC_RANGE_KIND_HOST = 1,
  // Device memory that a host-to-device copy filled with pointers into other
  // ledger ranges, so a kernel given this buffer can reach NC memory.
  IREE_HAL_STREAMING_NC_RANGE_KIND_INDIRECT = 2,
} iree_hal_streaming_nc_range_kind_t;

// Returns a short name for |kind| for diagnostics.
const char* iree_hal_streaming_nc_range_kind_name(
    iree_hal_streaming_nc_range_kind_t kind);

iree_status_t iree_hal_streaming_nc_ledger_create(
    iree_allocator_t host_allocator,
    iree_hal_streaming_nc_ledger_t** out_ledger);

void iree_hal_streaming_nc_ledger_destroy(
    iree_hal_streaming_nc_ledger_t* ledger);

// Adds [|base|, |base| + |size|). A range with the same base is replaced.
iree_status_t iree_hal_streaming_nc_ledger_insert(
    iree_hal_streaming_nc_ledger_t* ledger, uint64_t base, uint64_t size,
    iree_hal_streaming_nc_range_kind_t kind);

// Removes the range starting at |base|, if any.
void iree_hal_streaming_nc_ledger_remove(iree_hal_streaming_nc_ledger_t* ledger,
                                         uint64_t base);

// Returns the number of ranges.
iree_host_size_t iree_hal_streaming_nc_ledger_count(
    iree_hal_streaming_nc_ledger_t* ledger);

// Returns true if |address| lies in a range and stores its kind.
bool iree_hal_streaming_nc_ledger_lookup(
    iree_hal_streaming_nc_ledger_t* ledger, uint64_t address,
    iree_hal_streaming_nc_range_kind_t* out_kind);

// Returns true if any naturally aligned 64-bit word of |data| points into a
// range and stores the kind of the first hit.
bool iree_hal_streaming_nc_ledger_scan(
    iree_hal_streaming_nc_ledger_t* ledger, iree_const_byte_span_t data,
    iree_hal_streaming_nc_range_kind_t* out_kind);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // LIBHRX_SRC_BINDING_COMMON_NC_LEDGER_H_
