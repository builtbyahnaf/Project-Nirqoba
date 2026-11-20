// Chi-5b-i-b-1: dynamic array backing storage.
//
// The compiler slice will own length and capacity; the runtime owns only the
// bytes. These tests pin the contract that slice depends on: an empty array holds
// no buffer, freeing an empty array is a no-op rather than a special case at every
// destructor, and every successful create is matched by exactly one free.
//
// They also pin isolation. Array buffers come from their own allocator instance,
// so allocating and freeing them leaves the default allocator's small-allocation
// cache and its profile exactly as they were; allocator_test.cpp asserts on both.

#include "test_harness.h"

#include <cstdint>

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/array.h"

TEST(ArrayAllocatorTest, AZeroCapacityArrayHoldsNoBuffer) {
  const int32_t before = nrq_array_live_count();
  ASSERT_EQ(nrq_array_create(0, 8, 8), nullptr);
  ASSERT_EQ(nrq_array_live_count(), before);
}

TEST(ArrayAllocatorTest, FreeingTheEmptyRepresentationIsANoOp) {
  const int32_t before = nrq_array_live_count();
  nrq_array_free(nullptr);
  ASSERT_EQ(nrq_array_live_count(), before);
}

TEST(ArrayAllocatorTest, CreateReturnsAnAlignedBufferAndCountsOnce) {
  const int32_t before = nrq_array_live_count();
  void* buffer = nrq_array_create(10, 8, 8);
  ASSERT_NE(buffer, nullptr);
  ASSERT_EQ(reinterpret_cast<uintptr_t>(buffer) % 8, static_cast<uintptr_t>(0));
  ASSERT_EQ(nrq_array_live_count(), before + 1);

  nrq_array_free(buffer);
  ASSERT_EQ(nrq_array_live_count(), before);
}

TEST(ArrayAllocatorTest, ARepeatedCreateFreeCycleDoesNotGrowTheLiveCount) {
  const int32_t before = nrq_array_live_count();
  for (int i = 0; i < 64; ++i) {
    void* buffer = nrq_array_create(static_cast<size_t>(i + 1), 4, 4);
    ASSERT_NE(buffer, nullptr);
    nrq_array_free(buffer);
  }
  ASSERT_EQ(nrq_array_live_count(), before);
}

TEST(ArrayAllocatorTest, AnUnaddressableCapacityIsRefused) {
  const int32_t before = nrq_array_live_count();
  const size_t huge = static_cast<size_t>(-1) / 4 + 1;
  ASSERT_EQ(nrq_array_create(huge, 8, 8), nullptr);
  ASSERT_EQ(nrq_array_live_count(), before);
}

// Chi-5b-i-c: growth. The first growth is the first allocation, later ones keep
// the elements already stored, and every one of them stays in the array domain.
TEST(ArrayAllocatorTest, TheFirstGrowthAllocatesInTheArrayDomain) {
  const int32_t before = nrq_array_live_count();
  void* buffer = nrq_array_reallocate(nullptr, 0, 8, 8, 8);
  ASSERT_NE(buffer, nullptr);
  ASSERT_EQ(nrq_array_live_count(), before + 1);
  nrq_array_free(buffer);
  ASSERT_EQ(nrq_array_live_count(), before);
}

TEST(ArrayAllocatorTest, GrowthPreservesTheElementsAlreadyStored) {
  const int32_t before = nrq_array_live_count();
  void* buffer = nrq_array_create(4, sizeof(uint32_t), sizeof(uint32_t));
  ASSERT_NE(buffer, nullptr);
  auto* slots = static_cast<uint32_t*>(buffer);
  for (uint32_t i = 0; i < 4; ++i) slots[i] = i * 7 + 1;

  void* grown =
      nrq_array_reallocate(buffer, 4, 16, sizeof(uint32_t), sizeof(uint32_t));
  ASSERT_NE(grown, nullptr);
  auto* grown_slots = static_cast<uint32_t*>(grown);
  for (uint32_t i = 0; i < 4; ++i) ASSERT_EQ(grown_slots[i], i * 7 + 1);
  // Growth replaces the storage, not the ownership: one array is one live
  // buffer before and after, and one free releases it.
  ASSERT_EQ(nrq_array_live_count(), before + 1);

  nrq_array_free(grown);
  ASSERT_EQ(nrq_array_live_count(), before);
}

TEST(ArrayAllocatorTest, ARequestThatDoesNotExtendTheCapacityKeepsTheBuffer) {
  const int32_t before = nrq_array_live_count();
  void* buffer = nrq_array_create(4, 4, 4);
  ASSERT_NE(buffer, nullptr);
  ASSERT_EQ(nrq_array_reallocate(buffer, 4, 4, 4, 4), buffer);
  ASSERT_EQ(nrq_array_reallocate(buffer, 4, 2, 4, 4), buffer);
  ASSERT_EQ(nrq_array_live_count(), before + 1);
  nrq_array_free(buffer);
  ASSERT_EQ(nrq_array_live_count(), before);
}

TEST(ArrayAllocatorTest, GrowthStaysOutOfTheDefaultAllocatorProfile) {
  // 48 bytes is a size the default allocator caches: a growth that fell back to
  // the default domain would show up as live bytes here.
  const uint64_t bytes_before = nrq_memory_profiler_get_current_bytes();
  void* buffer = nrq_array_reallocate(nullptr, 0, 6, 8, 8);
  ASSERT_NE(buffer, nullptr);
  void* grown = nrq_array_reallocate(buffer, 6, 12, 8, 8);
  ASSERT_NE(grown, nullptr);
  ASSERT_EQ(nrq_memory_profiler_get_current_bytes(), bytes_before);
  nrq_array_free(grown);
  ASSERT_EQ(nrq_memory_profiler_get_current_bytes(), bytes_before);
}

TEST(ArrayAllocatorTest, ArrayBuffersLeaveTheDefaultAllocatorProfileAlone) {
  // Size 48 is deliberately one the default allocator caches: if the buffer came
  // from the default domain, this and allocator_test.cpp's reuse assertions would
  // both see the difference.
  const uint64_t bytes_before = nrq_memory_profiler_get_current_bytes();
  void* buffer = nrq_array_create(12, 4, 4);
  ASSERT_NE(buffer, nullptr);
  nrq_array_free(buffer);
  ASSERT_EQ(nrq_memory_profiler_get_current_bytes(), bytes_before);
}
