// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/nc_ledger.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::hal::streaming {
namespace {

class NcLedgerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IREE_ASSERT_OK(
        iree_hal_streaming_nc_ledger_create(iree_allocator_system(), &ledger_));
  }
  void TearDown() override { iree_hal_streaming_nc_ledger_destroy(ledger_); }

  bool Lookup(uint64_t address,
              iree_hal_streaming_nc_range_kind_t* kind = nullptr) {
    return iree_hal_streaming_nc_ledger_lookup(ledger_, address, kind);
  }

  iree_hal_streaming_nc_ledger_t* ledger_ = nullptr;
};

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kGiB = 1ull << 30;

TEST_F(NcLedgerTest, EmptyLedgerMatchesNothing) {
  EXPECT_EQ(iree_hal_streaming_nc_ledger_count(ledger_), 0u);
  EXPECT_FALSE(Lookup(0x7f0000000000ull));
  const uint64_t words[2] = {0x7f0000000000ull, 0};
  EXPECT_FALSE(iree_hal_streaming_nc_ledger_scan(
      ledger_, iree_make_const_byte_span(words, sizeof(words)), nullptr));
}

TEST_F(NcLedgerTest, LookupIsHalfOpen) {
  const uint64_t base = 0x7f0000001000ull;
  IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
      ledger_, base, 4096, IREE_HAL_STREAMING_NC_RANGE_KIND_IPC_IMPORT));
  iree_hal_streaming_nc_range_kind_t kind;
  EXPECT_TRUE(Lookup(base, &kind));
  EXPECT_EQ(kind, IREE_HAL_STREAMING_NC_RANGE_KIND_IPC_IMPORT);
  EXPECT_TRUE(Lookup(base + 4095));
  EXPECT_FALSE(Lookup(base + 4096));
  EXPECT_FALSE(Lookup(base - 1));
}

TEST_F(NcLedgerTest, ZeroSizeIsIgnored) {
  IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
      ledger_, 0x1000, 0, IREE_HAL_STREAMING_NC_RANGE_KIND_HOST));
  EXPECT_EQ(iree_hal_streaming_nc_ledger_count(ledger_), 0u);
}

TEST_F(NcLedgerTest, SameBaseReplaces) {
  const uint64_t base = 0x7f0000000000ull;
  IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
      ledger_, base, 4096, IREE_HAL_STREAMING_NC_RANGE_KIND_HOST));
  IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
      ledger_, base, 3 * kMiB, IREE_HAL_STREAMING_NC_RANGE_KIND_INDIRECT));
  EXPECT_EQ(iree_hal_streaming_nc_ledger_count(ledger_), 1u);
  iree_hal_streaming_nc_range_kind_t kind;
  EXPECT_TRUE(Lookup(base + 2 * kMiB, &kind));
  EXPECT_EQ(kind, IREE_HAL_STREAMING_NC_RANGE_KIND_INDIRECT);
}

TEST_F(NcLedgerTest, OverlappingRangesAreFound) {
  // A large range followed by a small one starting inside it: an address past
  // the small range but inside the large one must still hit.
  const uint64_t big = 0x7f0000000000ull;
  IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
      ledger_, big, 64 * kMiB, IREE_HAL_STREAMING_NC_RANGE_KIND_IPC_IMPORT));
  IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
      ledger_, big + kMiB, 4096, IREE_HAL_STREAMING_NC_RANGE_KIND_INDIRECT));
  iree_hal_streaming_nc_range_kind_t kind;
  EXPECT_TRUE(Lookup(big + 32 * kMiB, &kind));
  EXPECT_EQ(kind, IREE_HAL_STREAMING_NC_RANGE_KIND_IPC_IMPORT);
  EXPECT_TRUE(Lookup(big + kMiB, &kind));
  EXPECT_EQ(kind, IREE_HAL_STREAMING_NC_RANGE_KIND_INDIRECT);
  EXPECT_FALSE(Lookup(big + 64 * kMiB));
}

TEST_F(NcLedgerTest, RemoveForgetsOnlyThatRange) {
  const uint64_t a = 0x7f0000000000ull;
  const uint64_t b = 0x7f1000000000ull;
  IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
      ledger_, a, kMiB, IREE_HAL_STREAMING_NC_RANGE_KIND_HOST));
  IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
      ledger_, b, kMiB, IREE_HAL_STREAMING_NC_RANGE_KIND_HOST));
  iree_hal_streaming_nc_ledger_remove(ledger_, a);
  iree_hal_streaming_nc_ledger_remove(ledger_, a + 1);  // Not a base: no-op.
  EXPECT_EQ(iree_hal_streaming_nc_ledger_count(ledger_), 1u);
  EXPECT_FALSE(Lookup(a));
  EXPECT_TRUE(Lookup(b + 100));
}

TEST_F(NcLedgerTest, HugeRangeAndOverflowAreCovered) {
  const uint64_t base = 0x700000000000ull;
  IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
      ledger_, base, 512 * kGiB, IREE_HAL_STREAMING_NC_RANGE_KIND_IPC_IMPORT));
  EXPECT_TRUE(Lookup(base + 300 * kGiB));
  IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
      ledger_, UINT64_MAX - 4095, 1ull << 20,
      IREE_HAL_STREAMING_NC_RANGE_KIND_HOST));
  EXPECT_TRUE(Lookup(UINT64_MAX - 1));
}

TEST_F(NcLedgerTest, ScanFindsAlignedPointerWords) {
  const uint64_t base = 0x7f0000000000ull;
  IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
      ledger_, base, kMiB, IREE_HAL_STREAMING_NC_RANGE_KIND_IPC_IMPORT));
  // A by-value struct of pointers, like RankSignals, after two scalars.
  struct {
    int32_t rank;
    int32_t world_size;
    uint64_t pointers[4];
  } args = {1, 2, {0x7e0000000000ull, 0x7e0000100000ull, base + 256, 0}};
  iree_hal_streaming_nc_range_kind_t kind;
  EXPECT_TRUE(iree_hal_streaming_nc_ledger_scan(
      ledger_, iree_make_const_byte_span(&args, sizeof(args)), &kind));
  EXPECT_EQ(kind, IREE_HAL_STREAMING_NC_RANGE_KIND_IPC_IMPORT);
  args.pointers[2] = 0x7e0000200000ull;
  EXPECT_FALSE(iree_hal_streaming_nc_ledger_scan(
      ledger_, iree_make_const_byte_span(&args, sizeof(args)), nullptr));
}

TEST_F(NcLedgerTest, ManyRangesAgreeWithBruteForce) {
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  for (uint64_t i = 0; i < 2000; ++i) {
    const uint64_t base = 0x7f0000000000ull + i * 7 * kMiB + (i % 13) * 4096;
    const uint64_t size = (1 + i % 5) * kMiB;
    ranges.push_back({base, base + size});
    IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
        ledger_, base, size, IREE_HAL_STREAMING_NC_RANGE_KIND_HOST));
  }
  for (uint64_t i = 0; i < 2000; i += 3) {
    iree_hal_streaming_nc_ledger_remove(ledger_, ranges[i].first);
    ranges[i] = {0, 0};
  }
  for (uint64_t probe = 0x7f0000000000ull - kMiB;
       probe < 0x7f0000000000ull + 2000 * 7 * kMiB; probe += 333331) {
    bool expected = false;
    for (const auto& range : ranges) {
      expected |= probe >= range.first && probe < range.second;
    }
    ASSERT_EQ(Lookup(probe), expected) << std::hex << probe;
  }
}

TEST_F(NcLedgerTest, LiveRangeStaysVisibleDuringConcurrentChurn) {
  const uint64_t live = 0x7f0000000000ull;
  IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
      ledger_, live, kMiB, IREE_HAL_STREAMING_NC_RANGE_KIND_IPC_IMPORT));
  std::atomic<bool> stop{false};
  std::atomic<int> misses{0};
  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      if (!Lookup(live + 4096)) {
        misses.fetch_add(1);
      }
    }
  });
  for (uint64_t i = 0; i < 20000; ++i) {
    const uint64_t base = 0x7e0000000000ull + (i % 512) * 4 * kMiB;
    IREE_ASSERT_OK(iree_hal_streaming_nc_ledger_insert(
        ledger_, base, kMiB, IREE_HAL_STREAMING_NC_RANGE_KIND_HOST));
    iree_hal_streaming_nc_ledger_remove(ledger_, base);
  }
  stop = true;
  reader.join();
  EXPECT_EQ(misses.load(), 0);
}

}  // namespace
}  // namespace iree::hal::streaming
