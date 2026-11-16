// Phase 7 runtime-isolation tests.
//
// These tests link the development runtime, so they observe the same collector
// implementation a --debug-enabled program uses. Ordinary owned, arena, pool,
// owned-array, secure and plain-shared operations must not allocate collector
// objects, register roots, create layouts or run a collection merely because the
// collector is available.
#include "test_harness.h"

#include <cstdint>

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/gc.h"

namespace {

struct CollectorSnapshot {
  uint64_t allocated_bytes;
  uint64_t collected_bytes;
  uint64_t collections;
  uint64_t live_objects;
  size_t layouts;
  uint64_t memo_hits;
  uint64_t memo_misses;
};

CollectorSnapshot snapshot() {
  const nrq_gc_stats stats = nrq_gc_get_stats();
  return CollectorSnapshot{
      stats.allocated_bytes, stats.collected_bytes, stats.collections,
      stats.live_objects, nrq_gc_layout_count(), nrq_gc_layout_memo_hits(),
      nrq_gc_layout_memo_misses()};
}

void assert_unchanged(const CollectorSnapshot& before,
                      const CollectorSnapshot& after) {
  ASSERT_EQ(after.allocated_bytes, before.allocated_bytes);
  ASSERT_EQ(after.collected_bytes, before.collected_bytes);
  ASSERT_EQ(after.collections, before.collections);
  ASSERT_EQ(after.live_objects, before.live_objects);
  ASSERT_EQ(after.layouts, before.layouts);
  ASSERT_EQ(after.memo_hits, before.memo_hits);
  ASSERT_EQ(after.memo_misses, before.memo_misses);
}

}  // namespace

TEST(GcIsolationTest, OrdinaryAllocatorsDoNotEnterTheCollector) {
  nrq_gc_enable();
  const CollectorSnapshot before = snapshot();

  void* owned = nrq_memory_allocate(64, 16);
  ASSERT_NE(owned, nullptr);
  nrq_memory_deallocate_checked(owned);

  void* array = nrq_memory_allocate_array(8, 8, 8);
  ASSERT_NE(array, nullptr);
  nrq_memory_deallocate_checked(array);

  void* secure = nrq_memory_secure_allocate(32, 16);
  ASSERT_NE(secure, nullptr);
  nrq_memory_secure_release(secure);

  void* arena = nrq_allocator_create(NRQ_ALLOCATOR_ARENA, 4096);
  ASSERT_NE(arena, nullptr);
  void* arena_value = nrq_arena_allocate(arena, 64, 16);
  ASSERT_NE(arena_value, nullptr);
  ASSERT_EQ(nrq_arena_reset(arena), int32_t{1});
  ASSERT_EQ(nrq_arena_destroy(arena), int32_t{1});

  void* pool = nrq_allocator_create(NRQ_ALLOCATOR_POOL, 64);
  ASSERT_NE(pool, nullptr);
  void* pool_value = nrq_memory_allocate_from(pool, 32, 16);
  ASSERT_NE(pool_value, nullptr);
  nrq_memory_deallocate_checked(pool_value);
  ASSERT_EQ(nrq_allocator_destroy(pool), int32_t{1});

  void* shared = nrq_memory_shared_allocate(64, 16);
  ASSERT_NE(shared, nullptr);
  ASSERT_EQ(nrq_memory_shared_count(shared), uint64_t{1});
  ASSERT_EQ(nrq_memory_shared_release(shared), int32_t{1});

  assert_unchanged(before, snapshot());
}

TEST(GcIsolationTest, PlainSharedRootsAreNotCollectorRoots) {
  // The shared runtime has its own strong count and control shards. A shared
  // payload with no managed fields must not create a GC layout or root.
  const CollectorSnapshot before = snapshot();
  void* shared = nrq_memory_shared_allocate(48, 16);
  ASSERT_NE(shared, nullptr);
  ASSERT_EQ(nrq_memory_shared_release(shared), int32_t{1});
  assert_unchanged(before, snapshot());
}
