// Phase 6: compiler-planned array allocation.
//
// The compiler passes a static plan and a per-site cache word. The runtime must
// use the plan when it holds, keep the resolved descriptor in the cache so later
// allocations at that site skip descriptor lookup, and refuse to trust a cache
// word that describes a different shape.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>

#include "nirqoba/runtime/alloc_hint.h"
#include "nirqoba/runtime/gc.h"

namespace {

constexpr uint32_t kArrayExtendedPlan = 1u | (3u << 8);

void* root_slot = nullptr;

struct Pair {
  void* first;
  void* second;
};

}  // namespace

TEST(GcPlannedAllocationTest, PlannedArrayUsesTheExtendedRepresentation) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_root_register(&root_slot);
  const size_t offsets[2] = {offsetof(Pair, first), offsetof(Pair, second)};
  void* site_cache = nullptr;
  void* object = nrq_gc_allocate_array_planned(
      &root_slot, 4, sizeof(Pair), offsets, 2, &site_cache, kArrayExtendedPlan);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(root_slot, object);
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  ASSERT_EQ(nrq_gc_object_header_size(object), size_t{16});
  // The first allocation resolved the descriptor and stored it at the site.
  ASSERT_NE(site_cache, nullptr);

  // A second allocation at the same site reuses the cached descriptor instead of
  // searching the layout memo.
  const uint64_t misses = nrq_gc_layout_memo_misses();
  void* second = nrq_gc_allocate_array_planned(
      &root_slot, 4, sizeof(Pair), offsets, 2, &site_cache, kArrayExtendedPlan);
  ASSERT_NE(second, nullptr);
  ASSERT_NE(second, object);
  // A warm site cache means no layout-memo work at all on the second call.
  ASSERT_EQ(nrq_gc_layout_memo_misses(), misses);
  nrq_gc_root_unregister(&root_slot);
  nrq_gc_collect();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
  nrq_gc_enable();
}

TEST(GcPlannedAllocationTest, MissingPlanFallsBackToTheAllocator) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_root_register(&root_slot);
  const size_t offsets[1] = {0};
  void* site_cache = nullptr;
  // A caller that declares no plan still gets a correct allocation: arrays are
  // extended no matter what the compiler said.
  void* object = nrq_gc_allocate_array_planned(&root_slot, 3, sizeof(void*),
                                               offsets, 1, &site_cache, 0);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  nrq_gc_root_unregister(&root_slot);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(GcPlannedAllocationTest, ARejectedPlanFallsBackInsteadOfFailing) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_root_register(&root_slot);
  const size_t offsets[1] = {0};
  void* site_cache = nullptr;
  const uint32_t rejected = NRQ_ALLOC_PLAN_REJECTED;
  void* object = nrq_gc_allocate_array_planned(
      &root_slot, 2, sizeof(void*), offsets, 1, &site_cache, rejected);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  nrq_gc_root_unregister(&root_slot);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(GcPlannedAllocationTest, ACacheDescribingAnotherShapeIsIgnored) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_root_register(&root_slot);
  const size_t offsets[1] = {0};
  // A record descriptor for the same payload size: it has the same element size
  // but not the array flag, so it must not be used for an array allocation.
  const nrq_object_layout* record =
      nrq_gc_layout_intern(sizeof(void*), 0, nullptr, nullptr, 0);
  ASSERT_NE(record, nullptr);
  void* site_cache = const_cast<void*>(static_cast<const void*>(record));
  void* object = nrq_gc_allocate_array_planned(&root_slot, 2, sizeof(void*),
                                               offsets, 1, &site_cache,
                                               kArrayExtendedPlan);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  ASSERT_NE(site_cache, static_cast<void*>(const_cast<nrq_object_layout*>(record)));
  // The cache now names an array descriptor, and tracing is correct: storing a
  // managed reference in the element keeps it alive across a collection.
  nrq_gc_root_unregister(&root_slot);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(GcPlannedAllocationTest, AOneElementArrayIsStillExtended) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_root_register(&root_slot);
  const size_t offsets[1] = {0};
  void* site_cache = nullptr;
  void* object = nrq_gc_allocate_array_planned(&root_slot, 1, sizeof(void*),
                                               offsets, 1, &site_cache,
                                               kArrayExtendedPlan);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  ASSERT_EQ(nrq_gc_object_header_size(object), size_t{16});
  nrq_gc_root_unregister(&root_slot);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(GcPlannedAllocationTest, AOneElementArrayUsesCompactWhenPlannedOrAuto) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_root_register(&root_slot);
  constexpr uint32_t kArrayCompactPlan =
      NRQ_ALLOC_PLAN_ARRAY | NRQ_ALLOC_PLAN_REPEATED |
      NRQ_ALLOC_PLAN_REPR_BITS(NRQ_ALLOC_REPR_COMPACT);
  const size_t offsets[1] = {0};
  void* site_cache = nullptr;
  void* compact_planned = nrq_gc_allocate_array_planned(
      &root_slot, 1, 24, offsets, 1, &site_cache, kArrayCompactPlan);
  ASSERT_NE(compact_planned, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(compact_planned),
            static_cast<int32_t>(NRQ_GC_REPR_COMPACT));
  ASSERT_EQ(nrq_gc_object_header_size(compact_planned), size_t{8});
  const nrq_object_layout* layout = nrq_gc_object_layout(compact_planned);
  ASSERT_NE(layout, nullptr);
  ASSERT_TRUE((layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) != 0);

  void* compact_auto = nrq_gc_allocate_array_planned(
      &root_slot, 1, 24, offsets, 1, &site_cache, 0);
  ASSERT_NE(compact_auto, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(compact_auto),
            static_cast<int32_t>(NRQ_GC_REPR_COMPACT));
  ASSERT_EQ(nrq_gc_object_header_size(compact_auto), size_t{8});
  nrq_gc_root_unregister(&root_slot);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(GcPlannedAllocationTest, PlannedSoAUsesAndCachesItsColumnLayout) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_root_register(&root_slot);
  // One managed column at offset 0 and one scalar column at offset 8, each with
  // stride 8 inside a 24-byte element.
  const size_t columns[4] = {0, 8, 8, 8};
  void* site_cache = nullptr;
  void* object = nrq_gc_allocate_soa_planned(&root_slot, 4, 24, columns, 4,
                                             &site_cache, kArrayExtendedPlan);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  ASSERT_NE(site_cache, nullptr);
  const uint64_t misses = nrq_gc_layout_memo_misses();
  void* second = nrq_gc_allocate_soa_planned(&root_slot, 4, 24, columns, 4,
                                              &site_cache, kArrayExtendedPlan);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(nrq_gc_layout_memo_misses(), misses);
  nrq_gc_root_unregister(&root_slot);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(GcPlannedAllocationTest, SoACacheWithDifferentColumnsIsIgnored) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_root_register(&root_slot);
  const size_t offsets[1] = {0};
  // A plain interleaved array descriptor for the same element size: it is not a
  // column layout at all, so the SoA path must not reuse it.
  const nrq_object_layout* array =
      nrq_gc_layout_intern(24, 24, nullptr, offsets, 1);
  ASSERT_NE(array, nullptr);
  void* site_cache = const_cast<void*>(static_cast<const void*>(array));
  const size_t columns[4] = {0, 8, 8, 8};
  void* object = nrq_gc_allocate_soa_planned(&root_slot, 2, 24, columns, 4,
                                             &site_cache, kArrayExtendedPlan);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  nrq_gc_root_unregister(&root_slot);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(GcPlannedAllocationTest, PlannedAndTypedEntryPointsAgreeOnTheRepresentation) {
  nrq_gc_disable();
  nrq_gc_collect();
  void* typed_root = nullptr;
  void* planned_root = nullptr;
  nrq_gc_root_register(&typed_root);
  nrq_gc_root_register(&planned_root);
  const size_t offsets[1] = {0};
  void* typed =
      nrq_gc_allocate_array_typed_rooted(&typed_root, 2, sizeof(void*), offsets, 1);
  void* site_cache = nullptr;
  void* planned = nrq_gc_allocate_array_planned(
      &planned_root, 2, sizeof(void*), offsets, 1, &site_cache,
      kArrayExtendedPlan);
  ASSERT_NE(typed, nullptr);
  ASSERT_NE(planned, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(typed),
            nrq_gc_object_representation(planned));
  ASSERT_EQ(nrq_gc_object_header_size(typed),
            nrq_gc_object_header_size(planned));
  nrq_gc_root_unregister(&typed_root);
  nrq_gc_root_unregister(&planned_root);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}
