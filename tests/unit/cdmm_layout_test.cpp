// Phase 6: deterministic regions configured from compiler layout descriptors.
//
// The point of these entry points is that the slot geometry the compiler computed
// is the geometry the pool and arena reserve. What matters most is the refusal:
// a deterministic region cannot trace, so a layout that owns managed references
// must not be accepted.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "nirqoba/runtime/cdmm.h"
#include "nirqoba/runtime/gc.h"

namespace {

constexpr size_t kRecordBytes = 64;

struct BigRecord {
  char bytes[kRecordBytes];
};

}  // namespace

TEST(CdmmLayoutTest, APoolTakesItsSlotSizeFromTheLayout) {
  const nrq_object_layout* layout =
      nrq_gc_layout_intern(kRecordBytes, 0, nullptr, nullptr, 0);
  ASSERT_NE(layout, nullptr);
  void* pool = nrq_cdmm_pool_create_layout(
      layout, 0, NRQ_CDMM_POOL_GENERATIONS, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  void* first = nrq_cdmm_pool_allocate(pool);
  void* second = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_NE(first, second);
  // The slot is at least the payload the compiler described, and every byte of
  // that payload is writable.
  std::memset(first, 0x5A, kRecordBytes);
  std::memset(second, 0xA5, kRecordBytes);
  ASSERT_EQ(nrq_cdmm_pool_release(pool, first), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_release(pool, first), int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_release(pool, second), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmLayoutTest, ALayoutWithManagedReferencesIsRefused) {
  const size_t offsets[1] = {0};
  const nrq_object_layout* with_reference =
      nrq_gc_layout_intern(32, 0, nullptr, offsets, 1);
  ASSERT_NE(with_reference, nullptr);
  ASSERT_EQ(nrq_cdmm_pool_create_layout(with_reference, 0, 0, nullptr, nullptr),
            nullptr);
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  ASSERT_EQ(nrq_cdmm_arena_allocate_layout(arena, with_reference), nullptr);
  ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmLayoutTest, AnArrayLayoutIsRefusedByADeterministicRegion) {
  const size_t offsets[1] = {0};
  const nrq_object_layout* array =
      nrq_gc_layout_intern(sizeof(void*), sizeof(void*), nullptr, offsets, 1);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(nrq_cdmm_pool_create_layout(array, 0, 0, nullptr, nullptr), nullptr);
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  ASSERT_EQ(nrq_cdmm_arena_allocate_layout(arena, array), nullptr);
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmLayoutTest, AArenaAllocatesFromALayout) {
  const nrq_object_layout* layout =
      nrq_gc_layout_intern(kRecordBytes, 0, nullptr, nullptr, 0);
  ASSERT_NE(layout, nullptr);
  void* arena = nrq_cdmm_arena_create(64 * 1024, 0);
  ASSERT_NE(arena, nullptr);
  void* object = nrq_cdmm_arena_allocate_layout(arena, layout);
  ASSERT_NE(object, nullptr);
  ASSERT_TRUE(nrq_cdmm_arena_contains(arena, object) != 0);
  const uint64_t alignment = reinterpret_cast<uintptr_t>(object) % 16;
  ASSERT_EQ(alignment, uint64_t{0});
  std::memset(object, 0x33, kRecordBytes);
  ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{kRecordBytes});
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmLayoutTest, NullLayoutsAreRefusedEverywhere) {
  ASSERT_EQ(nrq_cdmm_pool_create_layout(nullptr, 0, 0, nullptr, nullptr),
            nullptr);
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  ASSERT_EQ(nrq_cdmm_arena_allocate_layout(arena, nullptr), nullptr);
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}
