// CDMM Phase 5: the three strategies share a process.
//
// Deterministic arenas and pools must not confuse the collector, and a
// collection must not disturb them. These tests prove the separation in both
// directions: collector pages are never claimed by the explicit-strategy range
// table, and explicit storage survives a collection, a reset, and a release.

#include "test_harness.h"

#include <cstdint>
#include <cstring>

#include "nirqoba/runtime/cdmm.h"
#include "nirqoba/runtime/gc.h"

namespace {

// A root slot outside any test frame, so the collector can see it.
void* retained_object = nullptr;

void sweep_collector() {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

}  // namespace

TEST(CdmmCoexistenceTest, ExplicitStorageIsNotCollectorManaged) {
  sweep_collector();
  void* arena = nrq_cdmm_arena_create(4096, 0);
  void* pool = nrq_cdmm_pool_create(32, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(arena, nullptr);
  ASSERT_NE(pool, nullptr);
  void* arena_object = nrq_cdmm_arena_allocate(arena, 32, 8);
  void* pool_slot = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(arena_object, nullptr);
  ASSERT_NE(pool_slot, nullptr);
  ASSERT_EQ(nrq_gc_pointer_is_managed(arena_object), int32_t{0});
  ASSERT_EQ(nrq_gc_pointer_is_managed(pool_slot), int32_t{0});
  ASSERT_EQ(nrq_cdmm_strategy_of(arena_object), NRQ_CDMM_STRATEGY_ARENA);
  ASSERT_EQ(nrq_cdmm_strategy_of(pool_slot), NRQ_CDMM_STRATEGY_POOL);

  void* managed = nrq_gc_allocate(64, nullptr);
  ASSERT_NE(managed, nullptr);
  ASSERT_EQ(nrq_gc_pointer_is_managed(managed), int32_t{1});
  // The collector's storage never appears in the explicit-strategy table.
  ASSERT_EQ(nrq_cdmm_strategy_of(managed), NRQ_CDMM_STRATEGY_NONE);
  NrqCdmmFacts facts{};
  ASSERT_EQ(nrq_cdmm_object_facts(managed, &facts), int32_t{0});

  nrq_gc_collect();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_pointer_is_managed(managed), int32_t{0});
  // Collecting did not reclaim either explicit allocation.
  ASSERT_TRUE(nrq_cdmm_arena_contains(arena, arena_object));
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{1});
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmCoexistenceTest, CollectionLeavesExplicitStorageIntact) {
  sweep_collector();
  void* arena = nrq_cdmm_arena_create(8192, 0);
  void* pool = nrq_cdmm_pool_create(32, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(arena, nullptr);
  ASSERT_NE(pool, nullptr);
  std::memset(nrq_cdmm_arena_allocate(arena, 128, 8), 0x3c, 128);
  std::memset(nrq_cdmm_pool_allocate(pool), 0x4d, 32);
  const uint64_t arena_used = nrq_cdmm_arena_used(arena);
  const uint64_t pool_committed = nrq_cdmm_pool_committed(pool);

  void* garbage = nrq_gc_allocate(512, nullptr);
  ASSERT_NE(garbage, nullptr);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_pointer_is_managed(garbage), int32_t{0});

  ASSERT_EQ(nrq_cdmm_arena_used(arena), arena_used);
  ASSERT_EQ(nrq_cdmm_pool_committed(pool), pool_committed);
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{1});
  // Explicit allocation still works after a collection.
  void* after = nrq_cdmm_arena_allocate(arena, 64, 8);
  ASSERT_NE(after, nullptr);
  ASSERT_TRUE(nrq_cdmm_arena_contains(arena, after));
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmCoexistenceTest, ArenaResetLeavesCollectorObjectsReachable) {
  sweep_collector();
  nrq_gc_root_register(&retained_object);
  retained_object = nrq_gc_allocate(64, nullptr);
  ASSERT_NE(retained_object, nullptr);
  std::memset(retained_object, 0x2b, 64);
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  ASSERT_NE(nrq_cdmm_arena_allocate(arena, 32, 8), nullptr);

  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_pointer_is_managed(retained_object), int32_t{1});
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{1});
  ASSERT_EQ(nrq_gc_pointer_is_managed(retained_object), int32_t{1});
  ASSERT_EQ(static_cast<unsigned char*>(retained_object)[0],
            static_cast<unsigned char>(0x2b));

  nrq_gc_root_unregister(&retained_object);
  retained_object = nullptr;
  nrq_gc_collect();
  nrq_gc_collect();
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmCoexistenceTest, PoolReleaseDoesNotDisturbOtherStrategies) {
  sweep_collector();
  nrq_gc_root_register(&retained_object);
  retained_object = nrq_gc_allocate(48, nullptr);
  ASSERT_NE(retained_object, nullptr);
  void* arena = nrq_cdmm_arena_create(4096, 0);
  void* pool = nrq_cdmm_pool_create(32, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(arena, nullptr);
  ASSERT_NE(pool, nullptr);
  void* arena_object = nrq_cdmm_arena_allocate(arena, 48, 8);
  std::memset(arena_object, 0x61, 48);
  void* slot = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(slot, nullptr);

  ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{48});
  ASSERT_EQ(static_cast<unsigned char*>(arena_object)[47],
            static_cast<unsigned char>(0x61));
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_pointer_is_managed(retained_object), int32_t{1});

  nrq_gc_root_unregister(&retained_object);
  retained_object = nullptr;
  nrq_gc_collect();
  nrq_gc_collect();
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmCoexistenceTest, ExplicitDestructorsRunWhileTheCollectorIsLive) {
  sweep_collector();
  int calls = 0;
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  void* object = nrq_cdmm_arena_allocate_destructible(
      arena, 16, 8,
      [](void* pointer, void* context) {
        ++*static_cast<int*>(context);
        (void)pointer;
      },
      &calls);
  ASSERT_NE(object, nullptr);
  nrq_gc_collect();
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{1});
  ASSERT_EQ(calls, 1);
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}
