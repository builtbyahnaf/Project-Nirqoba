// AOR Phase 7: the boundary between ownership strategies.
//
// Nirqoba has three storage strategies that must not merge: deterministic
// ownership (the default), explicit regions (arena and pool), and tracing
// collector storage. An AOR representation is a header choice inside collector
// storage; it is not an ownership class, and an allocation plan may choose a
// strategy but may not change one.
//
// These tests pin the boundary from both sides: the collector cannot claim
// explicit storage, an explicit plan cannot reach the collector, and a reference
// between the two is visible without transferring either lifetime.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <memory>

#include "nirqoba/runtime/alloc_hint.h"
#include "nirqoba/runtime/cdmm.h"
#include "nirqoba/runtime/gc.h"

namespace {

// A registered root slot with its own storage, released by its owner.
class RootSlot {
 public:
  RootSlot() : storage_(new void*(nullptr)) {
    nrq_gc_root_register(storage_.get());
  }
  ~RootSlot() { nrq_gc_root_unregister(storage_.get()); }
  RootSlot(const RootSlot&) = delete;
  RootSlot& operator=(const RootSlot&) = delete;

  void** slot() { return storage_.get(); }
  void* get() const { return *storage_; }
  void clear() { nrq_gc_root_store(storage_.get(), nullptr); }

 private:
  std::unique_ptr<void*> storage_;
};

void clear_heap() {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();
}

// The plan a compiler emits for an arena or pool site: no representation, no
// header, deterministic lifetime.
NrqAllocPlan deterministic_plan(uint64_t payload_bytes, uint32_t reference_count) {
  NrqAllocHint hint{};
  hint.payload_bytes = payload_bytes;
  hint.element_count = 1;
  hint.alignment = 16;
  hint.reference_count = reference_count;
  hint.flags = NRQ_ALLOC_HINT_DETERMINISTIC;
  return nrq_alloc_plan(&hint);
}

}  // namespace

TEST(StrategyBoundaryTest, EveryCollectorEntryPointRefusesADeterministicPlan) {
  clear_heap();
  const uint64_t base = nrq_gc_get_live_objects();
  RootSlot root;

  // The flag wins over the representation: a plan that says "deterministic
  // storage" must not be honoured by an entry point that only allocates
  // collector storage, even if it also names a header.
  const uint32_t deterministic =
      NRQ_ALLOC_PLAN_DETERMINISTIC |
      NRQ_ALLOC_PLAN_REPR_BITS(NRQ_ALLOC_REPR_HEADERLESS);
  const size_t offsets[] = {0};
  const size_t columns[] = {0, 8};
  void* cache = nullptr;
  ASSERT_EQ(nrq_gc_allocate_planned(root.slot(), 16, offsets, 1, &cache,
                                    deterministic),
            nullptr);
  ASSERT_EQ(nrq_gc_allocate_array_planned(root.slot(), 4, 16, offsets, 1, &cache,
                                          deterministic),
            nullptr);
  ASSERT_EQ(nrq_gc_allocate_soa_planned(root.slot(), 4, 12, columns, 2, &cache,
                                        deterministic),
            nullptr);
  // A refused call allocates nothing and caches nothing.
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
  ASSERT_EQ(cache, nullptr);
  nrq_gc_enable();
}

TEST(StrategyBoundaryTest, ADeterministicPlanIsHonouredByARegionInstead) {
  // The same plan shape the collector refuses is exactly what a region accepts:
  // choosing the strategy is the point of the plan, changing it is not.
  const NrqAllocPlan plan = deterministic_plan(64, 0);
  ASSERT_TRUE((plan.flags & NRQ_ALLOC_PLAN_DETERMINISTIC) != 0);
  ASSERT_EQ(plan.representation, static_cast<uint32_t>(NRQ_ALLOC_REPR_NONE));
  ASSERT_EQ(plan.header_bytes, uint64_t{0});

  void* pool = nrq_cdmm_pool_create_planned(&plan, 0, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  void* slot = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(slot, nullptr);
  ASSERT_EQ(nrq_cdmm_strategy_of(slot), NRQ_CDMM_STRATEGY_POOL);
  ASSERT_EQ(nrq_gc_pointer_is_managed(slot), int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});

  // A plan that carries managed references cannot describe a region, because a
  // region does not trace and would silently drop them.
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  const NrqAllocPlan traced = deterministic_plan(64, 1);
  ASSERT_TRUE((traced.flags & NRQ_ALLOC_PLAN_HAS_REFERENCES) != 0);
  ASSERT_EQ(nrq_cdmm_arena_allocate_planned(arena, &traced), nullptr);
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(StrategyBoundaryTest, RepresentationDoesNotDecideOwnership) {
  clear_heap();
  const uint64_t base = nrq_gc_get_live_objects();

  // Same payload size, same 0-byte header, different ownership: the header
  // scheme follows the storage strategy, it does not choose it.
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  void* region_object = nrq_cdmm_arena_allocate(arena, 16, 16);
  ASSERT_NE(region_object, nullptr);

  RootSlot root;
  *root.slot() = nrq_gc_allocate(16, nullptr);
  ASSERT_NE(root.get(), nullptr);

  // The collector-managed 16-byte record is headerless and managed; the
  // 16-byte region object has no representation at all and is not managed.
  ASSERT_EQ(nrq_gc_object_representation(root.get()),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  ASSERT_EQ(nrq_gc_object_header_size(root.get()), size_t{0});
  ASSERT_EQ(nrq_gc_pointer_is_managed(root.get()), int32_t{1});

  ASSERT_EQ(nrq_gc_object_representation(region_object),
            static_cast<int32_t>(NRQ_GC_REPR_AUTO));
  ASSERT_EQ(nrq_gc_object_header_size(region_object), size_t{0});
  ASSERT_EQ(nrq_gc_pointer_is_managed(region_object), int32_t{0});
  ASSERT_EQ(nrq_cdmm_strategy_of(region_object), NRQ_CDMM_STRATEGY_ARENA);

  // Destroying the region does not disturb the collector object.
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{1});

  root.clear();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
  nrq_gc_enable();
}

TEST(StrategyBoundaryTest, ExplicitStorageCanRootACollectorObjectWithoutOwningIt) {
  clear_heap();
  const uint64_t base = nrq_gc_get_live_objects();
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  auto** slot = static_cast<void**>(
      nrq_cdmm_arena_allocate(arena, sizeof(void*), alignof(void*)));
  ASSERT_NE(slot, nullptr);

  *slot = nrq_gc_allocate(64, nullptr);
  void* object = *slot;
  ASSERT_NE(object, nullptr);

  // The region is a root region: its content keeps the collector object alive
  // while the program says so. Neither side takes over the other's lifetime.
  const size_t offsets[] = {0};
  nrq_gc_root_register_array(slot, 1, sizeof(void*), offsets, 1);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_pointer_is_managed(object), int32_t{1});
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{1});

  // The collector does not own the region, and the region does not own the
  // object. Releasing the root leaves the object to the collector alone.
  nrq_gc_root_unregister(reinterpret_cast<void**>(slot));
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{1});
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_pointer_is_managed(object), int32_t{0});
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
  nrq_gc_enable();
}
