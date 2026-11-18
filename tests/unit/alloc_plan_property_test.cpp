// Phase 6: property checks across the whole plan/representation surface.
//
// The plan is a prediction and the allocator is the fact. These tests take many
// shapes, ask the planner what it would do, allocate through the planned entry
// point, and require the two answers to match. They also pin the memo behaviour
// the direct-mapped table exists for: a program alternating eight shapes must not
// re-intern them on every round.

#include "test_harness.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "nirqoba/runtime/alloc_hint.h"
#include "nirqoba/runtime/gc.h"

namespace {

constexpr uint32_t kReprBits(NrqAllocPlanRepresentation representation) {
  return static_cast<uint32_t>(representation) << 8;
}

int32_t runtime_representation(NrqAllocPlanRepresentation representation) {
  switch (representation) {
    case NRQ_ALLOC_REPR_HEADERLESS: return NRQ_GC_REPR_HEADERLESS;
    case NRQ_ALLOC_REPR_COMPACT: return NRQ_GC_REPR_COMPACT;
    case NRQ_ALLOC_REPR_EXTENDED: return NRQ_GC_REPR_EXTENDED;
    default: return NRQ_GC_REPR_AUTO;
  }
}

uint32_t plan_flags_for(const NrqAllocPlan& plan) {
  return nrq_alloc_plan_flags(&plan);
}

}  // namespace

TEST(AllocPlanPropertyTest, ThePlanPredictsTheRepresentationForRecords) {
  nrq_gc_disable();
  nrq_gc_collect();
  const size_t sizes[] = {8, 16, 24, 32, 48, 64, 120, 200, 255, 256, 257, 512,
                          1024, 4096};
  for (const size_t payload : sizes) {
    void* root = nullptr;
    nrq_gc_root_register(&root);
    NrqAllocHint hint{};
    hint.payload_bytes = payload;
    hint.element_count = 1;
    hint.alignment = 8;
    hint.flags = NRQ_ALLOC_HINT_MANAGED;
    const NrqAllocPlan plan = nrq_alloc_plan(&hint);
    ASSERT_EQ(plan.flags & NRQ_ALLOC_PLAN_REJECTED, 0u);
    void* site_cache = nullptr;
    void* object = nrq_gc_allocate_planned(&root, payload, nullptr, 0,
                                           &site_cache, plan_flags_for(plan));
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(nrq_gc_object_representation(object),
              runtime_representation(static_cast<NrqAllocPlanRepresentation>(
                  plan.representation)));
    ASSERT_EQ(nrq_gc_object_header_size(object),
              static_cast<size_t>(plan.header_bytes));
    ASSERT_NE(site_cache, nullptr);
    nrq_gc_root_unregister(&root);
    nrq_gc_collect();
    nrq_gc_collect();
  }
  nrq_gc_enable();
}

TEST(AllocPlanPropertyTest, ThePlanPredictsTheRepresentationForArrays) {
  nrq_gc_disable();
  nrq_gc_collect();
  for (const size_t element : {size_t{8}, size_t{16}, size_t{64}, size_t{257}}) {
    void* root = nullptr;
    nrq_gc_root_register(&root);
    NrqAllocHint hint{};
    hint.payload_bytes = element;
    hint.element_count = 3;
    hint.alignment = 8;
    hint.flags = NRQ_ALLOC_HINT_MANAGED | NRQ_ALLOC_HINT_ARRAY;
    const NrqAllocPlan plan = nrq_alloc_plan(&hint);
    ASSERT_EQ(plan.representation, NRQ_ALLOC_REPR_EXTENDED);
    void* site_cache = nullptr;
    const size_t offsets[1] = {0};
    void* object = nrq_gc_allocate_array_planned(
        &root, 3, element, offsets, 1, &site_cache, plan_flags_for(plan));
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(nrq_gc_object_representation(object), NRQ_GC_REPR_EXTENDED);
    ASSERT_EQ(nrq_gc_object_header_size(object), size_t{16});
    nrq_gc_root_unregister(&root);
    nrq_gc_collect();
    nrq_gc_collect();
  }
  nrq_gc_enable();
}

TEST(AllocPlanPropertyTest, EightAlternatingShapesStopThrashingTheMemo) {
  nrq_gc_disable();
  nrq_gc_collect();
  // The memo used to hold four layouts, so eight round-robined shapes evicted one
  // another and every allocation re-entered interning.
  const size_t sizes[] = {16, 24, 32, 40, 48, 56, 64, 72};
  std::vector<void*> roots(8, nullptr);
  for (void*& root : roots) nrq_gc_root_register(&root);
  for (size_t round = 0; round < 2; ++round) {
    for (size_t index = 0; index < 8; ++index) {
      void* object = nrq_gc_allocate_array_typed_rooted(
          &roots[index], 2, sizes[index], nullptr, 0);
      ASSERT_NE(object, nullptr);
    }
  }
  const uint64_t misses_after_warmup = nrq_gc_layout_memo_misses();
  for (size_t round = 0; round < 4; ++round) {
    for (size_t index = 0; index < 8; ++index) {
      void* object = nrq_gc_allocate_array_typed_rooted(
          &roots[index], 2, sizes[index], nullptr, 0);
      ASSERT_NE(object, nullptr);
    }
  }
  // Warm shapes must not pay interning again. A miss here means the table still
  // lets one shape evict another.
  ASSERT_EQ(nrq_gc_layout_memo_misses(), misses_after_warmup);
  for (void*& root : roots) nrq_gc_root_unregister(&root);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(AllocPlanPropertyTest, ADifferentShapeInTheSameBucketIsRejected) {
  nrq_gc_disable();
  nrq_gc_collect();
  void* root = nullptr;
  nrq_gc_root_register(&root);
  // Same payload, different reference table: the cached descriptor must not be
  // reused just because the size agrees.
  void* site_cache = nullptr;
  const size_t first[2] = {0, sizeof(void*)};
  void* object = nrq_gc_allocate_planned(&root, 16, first, 2, &site_cache, 0);
  ASSERT_NE(object, nullptr);
  void* stale = site_cache;
  const size_t second[1] = {sizeof(void*)};
  void* other = nrq_gc_allocate_planned(&root, 16, second, 1, &site_cache, 0);
  ASSERT_NE(other, nullptr);
  ASSERT_NE(site_cache, stale);
  nrq_gc_root_unregister(&root);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(AllocPlanPropertyTest, PlannedAllocationIsSafeFromSeveralThreads) {
  nrq_gc_disable();
  nrq_gc_collect();
  // Every thread keeps its own root and site cache, which is how a compiled
  // program uses them: the cache word belongs to the site, not the allocator.
  constexpr size_t kThreads = 4;
  constexpr size_t kPerThread = 200;
  std::atomic<size_t> failures{0};
  std::vector<std::thread> workers;
  for (size_t thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&failures, thread] {
      void* root = nullptr;
      nrq_gc_root_register(&root);
      void* site_cache = nullptr;
      const size_t offsets[1] = {0};
      const size_t element = 16 + thread * 8;
      const uint32_t plan = 1u | kReprBits(NRQ_ALLOC_REPR_EXTENDED);
      for (size_t index = 0; index < kPerThread; ++index) {
        void* object = nrq_gc_allocate_array_planned(
            &root, 2, element, offsets, 1, &site_cache, plan);
        if (!object || nrq_gc_object_representation(object) != NRQ_GC_REPR_EXTENDED)
          ++failures;
      }
      nrq_gc_root_unregister(&root);
    });
  }
  for (std::thread& worker : workers) worker.join();
  ASSERT_EQ(failures.load(), size_t{0});
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(AllocPlanPropertyTest, ARejectedPlanStillAllocatesWithTheDefaultChoice) {
  nrq_gc_disable();
  nrq_gc_collect();
  void* root = nullptr;
  nrq_gc_root_register(&root);
  void* site_cache = nullptr;
  void* object = nrq_gc_allocate_planned(
      &root, 32, nullptr, 0, &site_cache,
      NRQ_ALLOC_PLAN_REJECTED | kReprBits(NRQ_ALLOC_REPR_HEADERLESS));
  ASSERT_NE(object, nullptr);
  // 32 bytes is headerless either way, so the fallback and the rejected plan agree
  // here; what matters is that the call succeeded and the cache was rewritten.
  ASSERT_EQ(nrq_gc_object_representation(object), NRQ_GC_REPR_HEADERLESS);
  ASSERT_NE(site_cache, nullptr);
  nrq_gc_root_unregister(&root);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}
