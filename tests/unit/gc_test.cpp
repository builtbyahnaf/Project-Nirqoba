#include "test_harness.h"

#include <cstddef>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "nirqoba/runtime/gc.h"
#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/cache_line.h"

using namespace std;

namespace {

struct Node {
  Node* next;
  uint64_t value;
};

struct PairReferences {
  void* first;
  void* second;
};

struct TripleReferences {
  void* first;
  void* second;
  void* third;
};

void trace_node(void* object, size_t object_bytes, nrq_gc_visit_fn visit,
                void* context) {
  (void)object_bytes;
  auto* node = static_cast<Node*>(object);
  visit(node->next, context);
}

std::atomic<uint64_t> safepoint_mutator_progress{0};
std::atomic<uint64_t> safepoint_trace_progress[2]{};
std::atomic<int> safepoint_trace_count{0};

void trace_safepoint_probe(void* object, size_t object_bytes,
                           nrq_gc_visit_fn visit, void* context) {
  (void)object;
  (void)object_bytes;
  (void)visit;
  (void)context;
  const int index = safepoint_trace_count.fetch_add(1);
  if (index < 2)
    safepoint_trace_progress[index].store(
        safepoint_mutator_progress.load());
  if (index == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void recurse_with_roots(int depth, uint64_t& live_at_bottom) {
  auto* local = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  const uint64_t frame = nrq_gc_root_frame_begin();
  nrq_gc_root_register(reinterpret_cast<void**>(&local));
  if (depth > 1) {
    recurse_with_roots(depth - 1, live_at_bottom);
  } else {
    nrq_gc_collect();
    live_at_bottom = nrq_gc_get_live_objects();
  }
  nrq_gc_root_frame_end(frame);
  local = nullptr;
}

}  // namespace

TEST(GcTest, SharedCountersDoNotShareACacheLine) {
  // The allocation-path accounting is updated from every mutator thread. If the
  // padding ever regressed, unrelated shared state would share a line and every
  // update would invalidate another thread's copy even though the variables are
  // independent.
  ASSERT_EQ(nirq::nrq_cache_line_bytes, 64);
  ASSERT_EQ(alignof(nirq::NrqCacheLineAligned<std::atomic<uint64_t>>), 64);
  ASSERT_EQ(sizeof(nirq::NrqCacheLineAligned<std::atomic<uint64_t>>), 64);

  nirq::NrqCacheLineAligned<std::atomic<uint64_t>> first;
  nirq::NrqCacheLineAligned<std::atomic<uint64_t>> second;
  const auto first_address = reinterpret_cast<uintptr_t>(&first);
  const auto second_address = reinterpret_cast<uintptr_t>(&second);
  ASSERT_EQ(first_address % 64, 0);
  ASSERT_EQ(second_address % 64, 0);
  ASSERT_TRUE(second_address - first_address >= 64);
}

TEST(GcTest, ObjectHeaderStaysWithinItsMetadataBudget) {
  // Small managed objects pay this overhead on every allocation. The header
  // shares `next`/`tlab_next` and derives the payload address, so it must stay
  // at 96 bytes; the payload must remain max_align_t aligned.
  ASSERT_EQ(nrq_gc_object_header_bytes(), size_t{96});
  ASSERT_EQ(nrq_gc_object_header_bytes() % alignof(std::max_align_t), size_t{0});

  void* object = nrq_gc_allocate(8, nullptr);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(reinterpret_cast<uintptr_t>(object) % alignof(std::max_align_t),
            uintptr_t{0});
}

TEST(GcTest, PreservesReachableCyclesAndSweepsAfterRootRemoval) {
  nrq_gc_enable();
  nrq_gc_set_threshold(1024 * 1024);

  auto* first = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  auto* second = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  nrq_gc_root_store(reinterpret_cast<void**>(&first), first);
  nrq_gc_root_register(reinterpret_cast<void**>(&first));
  nrq_gc_root_store(reinterpret_cast<void**>(&first->next), second);
  nrq_gc_root_store(reinterpret_cast<void**>(&second->next), first);

  nrq_gc_collect();
  auto live_stats = nrq_gc_get_stats();
  ASSERT_EQ(live_stats.live_objects, static_cast<uint64_t>(2));

  nrq_gc_root_unregister(reinterpret_cast<void**>(&first));
  first = nullptr;
  nrq_gc_collect();
  auto swept_stats = nrq_gc_get_stats();
  ASSERT_EQ(swept_stats.live_objects, static_cast<uint64_t>(0));
  ASSERT_TRUE(swept_stats.collected_bytes >= sizeof(Node) * 2);
}

TEST(GcTest, MemoryProfilerIncludesCurrentGcStatistics) {
  nrq_gc_enable();
  nrq_gc_collect();
  void* object = nrq_gc_allocate(48, nullptr);
  ASSERT_NE(object, nullptr);
  const nrq_gc_stats gc = nrq_gc_get_stats();
  NrqMemoryProfile profile{};
  nrq_memory_profiler_capture_into(&profile);
  ASSERT_EQ(profile.gc_allocated_bytes, gc.allocated_bytes);
  ASSERT_EQ(profile.gc_collected_bytes, gc.collected_bytes);
  ASSERT_EQ(profile.gc_collections, gc.collections);
  ASSERT_EQ(profile.gc_live_objects, gc.live_objects);
  ASSERT_EQ(profile.gc_live_objects, uint64_t{1});
  nrq_gc_collect();
}

TEST(GcTest, ExplicitCollectionStillRunsWhenAutomaticCollectionIsDisabled) {
  nrq_gc_set_threshold(1);
  nrq_gc_disable();
  void* object = nrq_gc_allocate(32, nullptr);
  ASSERT_NE(object, nullptr);
  const auto before = nrq_gc_get_stats();
  ASSERT_EQ(before.live_objects, static_cast<uint64_t>(1));

  nrq_gc_collect();
  const auto after = nrq_gc_get_stats();
  ASSERT_EQ(after.live_objects, static_cast<uint64_t>(0));
  nrq_gc_enable();
  nrq_gc_set_threshold(1024 * 1024);
}

TEST(GcTest, ExplicitAllocatorOwnershipIsOutsideTheTracingHeap) {
  nrq_gc_disable();
  nrq_gc_collect();
  const nrq_gc_stats before = nrq_gc_get_stats();
  auto* explicit_bytes = static_cast<unsigned char*>(
      nrq_memory_allocate(64, alignof(uint64_t)));
  ASSERT_NE(explicit_bytes, nullptr);
  explicit_bytes[0] = 0x5A;

  nrq_gc_collect();
  ASSERT_EQ(explicit_bytes[0], static_cast<unsigned char>(0x5A));
  const nrq_gc_stats after = nrq_gc_get_stats();
  ASSERT_EQ(after.allocated_bytes, before.allocated_bytes);
  ASSERT_EQ(after.live_objects, before.live_objects);

  nrq_memory_deallocate_checked(explicit_bytes);
}

TEST(GcTest, RejectsAllocationSizeOverflowWithoutChangingHeapStatistics) {
  nrq_gc_disable();
  nrq_gc_collect();
  const nrq_gc_stats before = nrq_gc_get_stats();
  ASSERT_EQ(nrq_gc_allocate(static_cast<size_t>(-1), nullptr), nullptr);
  const nrq_gc_stats after = nrq_gc_get_stats();
  ASSERT_EQ(after.allocated_bytes, before.allocated_bytes);
  ASSERT_EQ(after.live_objects, before.live_objects);
  ASSERT_EQ(after.collections, before.collections);
}

TEST(GcTest, RejectsInvalidTypedLayoutsWithoutPublishingAnObject) {
  nrq_gc_disable();
  nrq_gc_collect();
  const nrq_gc_stats before = nrq_gc_get_stats();
  const size_t invalid_offset = sizeof(void*);
  ASSERT_EQ(nrq_gc_allocate_array_typed(1, sizeof(void*), &invalid_offset, 1),
            nullptr);
  const nrq_gc_stats after = nrq_gc_get_stats();
  ASSERT_EQ(after.allocated_bytes, before.allocated_bytes);
  ASSERT_EQ(after.live_objects, before.live_objects);
}

TEST(GcTest, TracesInlineAndOverflowReferenceLayouts) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();
  const size_t pair_offsets[] = {0, sizeof(void*)};
  const size_t triple_offsets[] = {0, sizeof(void*), 2 * sizeof(void*)};
  auto* pair = static_cast<PairReferences*>(nrq_gc_allocate_array_typed(
      1, sizeof(PairReferences), pair_offsets, 2));
  auto* triple = static_cast<TripleReferences*>(nrq_gc_allocate_array_typed(
      1, sizeof(TripleReferences), triple_offsets, 3));
  ASSERT_NE(pair, nullptr);
  ASSERT_NE(triple, nullptr);
  pair->first = nrq_gc_allocate(16, nullptr);
  pair->second = nrq_gc_allocate(16, nullptr);
  triple->first = nrq_gc_allocate(16, nullptr);
  triple->second = nrq_gc_allocate(16, nullptr);
  triple->third = nrq_gc_allocate(16, nullptr);
  ASSERT_NE(pair->first, nullptr);
  ASSERT_NE(pair->second, nullptr);
  ASSERT_NE(triple->first, nullptr);
  ASSERT_NE(triple->second, nullptr);
  ASSERT_NE(triple->third, nullptr);
  nrq_gc_root_register(reinterpret_cast<void**>(&pair));
  nrq_gc_root_register(reinterpret_cast<void**>(&triple));

  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline + 7);

  pair->first = nullptr;
  pair->second = nullptr;
  triple->first = nullptr;
  triple->second = nullptr;
  triple->third = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline + 2);

  nrq_gc_root_unregister(reinterpret_cast<void**>(&pair));
  nrq_gc_root_unregister(reinterpret_cast<void**>(&triple));
  pair = nullptr;
  triple = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
}

TEST(GcTest, RootFramesUnregisterOnlyTheirOwnNestedRoots) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();
  auto* outer = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  auto* inner = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  ASSERT_NE(outer, nullptr);
  ASSERT_NE(inner, nullptr);

  const uint64_t outer_frame = nrq_gc_root_frame_begin();
  nrq_gc_root_register(reinterpret_cast<void**>(&outer));
  const uint64_t inner_frame = nrq_gc_root_frame_begin();
  nrq_gc_root_register(reinterpret_cast<void**>(&inner));

  nrq_gc_root_frame_end(inner_frame);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline + 1);

  nrq_gc_root_frame_end(outer_frame);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);

  // Frame markers must identify frames even when no root has been registered
  // between nested begin calls.
  const uint64_t empty_outer = nrq_gc_root_frame_begin();
  const uint64_t empty_inner = nrq_gc_root_frame_begin();
  ASSERT_NE(empty_outer, empty_inner);
  nrq_gc_root_frame_end(empty_inner);
  nrq_gc_root_frame_end(empty_outer);
}

TEST(GcTest, BumpRegionsAlignMixedObjectSizesAndReclaimEmptyPages) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();
  constexpr size_t kObjectCount = 384;
  for (size_t index = 0; index < kObjectCount; ++index) {
    const size_t size = (index % 7 == 0) ? 4097 : 1 + (index * 13) % 257;
    void* object = nrq_gc_allocate(size, nullptr);
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(object) % alignof(std::max_align_t),
              uintptr_t{0});
  }
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline + kObjectCount);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
  nrq_gc_enable();
}

TEST(GcTest, AllocationPageCacheIsInvalidatedAfterCollection) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();
  ASSERT_NE(nrq_gc_allocate(32, nullptr), nullptr);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
  ASSERT_NE(nrq_gc_allocate(32, nullptr), nullptr);
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline + 1);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
}

TEST(GcTest, AllocationThresholdTriggersAutomaticCollection) {
  nrq_gc_enable();
  nrq_gc_set_threshold(1);
  nrq_gc_collect();
  const auto before = nrq_gc_get_stats();
  void* first = nrq_gc_allocate(32, nullptr);
  ASSERT_NE(first, nullptr);
  void* second = nrq_gc_allocate(32, nullptr);
  ASSERT_NE(second, nullptr);
  const auto after = nrq_gc_get_stats();
  ASSERT_TRUE(after.collections > before.collections);
  ASSERT_EQ(after.live_objects, static_cast<uint64_t>(1));
  nrq_gc_collect();
  nrq_gc_set_threshold(1024 * 1024);
}

TEST(GcTest, PinnedObjectSurvivesUntilItIsUnpinned) {
  nrq_gc_enable();
  nrq_gc_collect();
  void* object = nrq_gc_allocate(24, nullptr);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_pin(object), object);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{1});
  nrq_gc_unpin(object);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(GcTest, TransferPinsSupportBalancedManagedReturnHandoffs) {
  nrq_gc_disable();
  nrq_gc_collect();
  void* object = nrq_gc_allocate(24, nullptr);
  ASSERT_NE(object, nullptr);
  constexpr int kHandoffs = 32;
  for (int index = 0; index < kHandoffs; ++index)
    ASSERT_TRUE(nrq_gc_pin_transfer(object) == object);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{1});
  for (int index = 0; index < kHandoffs; ++index) nrq_gc_unpin(object);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(GcTest, ReferenceDiagnosticsCountHeapAndRootReferencesWhenEnabled) {
  nrq_gc_enable();
  nrq_gc_collect();
  auto* first = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  auto* second = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  first->next = second;
  second->next = nullptr;
  nrq_gc_root_register(reinterpret_cast<void**>(&first));
  nrq_gc_ref_count_stop();
  ASSERT_EQ(nrq_gc_get_ref_count(first), int64_t{-1});
  nrq_gc_ref_count_start();
  ASSERT_EQ(nrq_gc_get_ref_count(first), int64_t{1});
  ASSERT_EQ(nrq_gc_get_ref_count(second), int64_t{1});
  nrq_gc_ref_count_stop();
  nrq_gc_root_unregister(reinterpret_cast<void**>(&first));
  first = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(GcTest, ReferenceDiagnosticsCountTypedLayoutReferences) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();
  auto* child = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  size_t pointer_offset = offsetof(PairReferences, first);
  auto* parent = static_cast<PairReferences*>(nrq_gc_allocate_array_typed(
      1, sizeof(PairReferences), &pointer_offset, 1));
  ASSERT_NE(child, nullptr);
  ASSERT_NE(parent, nullptr);
  parent->first = child;
  nrq_gc_root_register(reinterpret_cast<void**>(&parent));
  nrq_gc_ref_count_start();
  ASSERT_EQ(nrq_gc_get_ref_count(parent), int64_t{1});
  ASSERT_EQ(nrq_gc_get_ref_count(child), int64_t{1});
  nrq_gc_ref_count_stop();
  nrq_gc_root_unregister(reinterpret_cast<void**>(&parent));
  parent = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
}

TEST(GcTest, ReferenceDiagnosticsCountPinnedRoots) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_ref_count_start();
  void* object = nrq_gc_allocate(32, nullptr);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_get_ref_count(object), int64_t{0});
  ASSERT_EQ(nrq_gc_pin(object), object);
  ASSERT_EQ(nrq_gc_get_ref_count(object), int64_t{1});
  nrq_gc_unpin(object);
  ASSERT_EQ(nrq_gc_get_ref_count(object), int64_t{0});
  nrq_gc_ref_count_stop();
  nrq_gc_collect();
}

TEST(GcTest, ConcurrentAllocationIsSerializedAndReclaimed) {
  nrq_gc_enable();
  nrq_gc_set_threshold(1024 * 1024 * 1024);
  nrq_gc_collect();
  std::atomic<bool> succeeded = true;
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&succeeded] {
      nrq_gc_mutator_enter();
      for (int index = 0; index < 128; ++index) {
        if (!nrq_gc_allocate(48, nullptr)) succeeded = false;
      }
      nrq_gc_mutator_leave();
    });
  }
  for (std::thread& worker : workers) worker.join();
  ASSERT_TRUE(succeeded.load());
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{512});
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
  nrq_gc_set_threshold(1024 * 1024);
}

TEST(GcTest, UnpublishedTlabAllocationsRemainVisibleInStats) {
  nrq_gc_disable();
  nrq_gc_collect();
  const nrq_gc_stats before = nrq_gc_get_stats();

  nrq_gc_mutator_enter();
  void* first = nrq_gc_allocate(24, nullptr);
  void* second = nrq_gc_allocate(32, nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  // Both objects are still in this thread's unpublished TLAB batch. Statistics
  // must aggregate the per-mutator counters rather than require a global update
  // on the allocation fast path.
  const nrq_gc_stats during = nrq_gc_get_stats();
  ASSERT_EQ(during.live_objects, before.live_objects + 2);
  ASSERT_EQ(during.allocated_bytes, before.allocated_bytes + 56);

  nrq_gc_mutator_leave();
  const nrq_gc_stats after = nrq_gc_get_stats();
  ASSERT_EQ(after.live_objects, before.live_objects + 2);
  ASSERT_EQ(after.allocated_bytes, before.allocated_bytes + 56);
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(GcTest, RootStoreSynchronizesConcurrentMutationAndCollection) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();
  auto* root = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  auto* first = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  auto* second = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  ASSERT_NE(root, nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  nrq_gc_root_register(reinterpret_cast<void**>(&root));
  nrq_gc_root_register(reinterpret_cast<void**>(&first));
  nrq_gc_root_register(reinterpret_cast<void**>(&second));
  nrq_gc_root_store(reinterpret_cast<void**>(&root->next), first);

  std::thread mutator([&] {
    for (int index = 0; index < 2000; ++index) {
      Node* target = index % 2 == 0 ? first : second;
      nrq_gc_root_store(reinterpret_cast<void**>(&root->next), target);
    }
  });
  std::thread collector([] {
    for (int index = 0; index < 200; ++index) nrq_gc_collect();
  });
  mutator.join();
  collector.join();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline + 3);

  nrq_gc_root_unregister(reinterpret_cast<void**>(&second));
  nrq_gc_root_unregister(reinterpret_cast<void**>(&first));
  nrq_gc_root_unregister(reinterpret_cast<void**>(&root));
  root = nullptr;
  first = nullptr;
  second = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
}

TEST(GcTest, RootedAllocationPublishesBeforeConcurrentCollection) {
  nrq_gc_disable();
  nrq_gc_collect();
  void* root = nullptr;
  nrq_gc_root_register(&root);
  std::atomic<bool> done = false;
  std::atomic<bool> succeeded = true;
  std::thread collector([&] {
    while (!done.load(std::memory_order_acquire)) nrq_gc_collect();
  });

  for (uint64_t value = 1; value <= 5000; ++value) {
    auto* object = static_cast<uint64_t*>(nrq_gc_allocate_array_typed_rooted(
        &root, 1, sizeof(uint64_t), nullptr, 0));
    if (!object) {
      succeeded = false;
      break;
    }
    *object = value;
    if (*object != value) succeeded = false;
  }

  done.store(true, std::memory_order_release);
  collector.join();
  // Allocations racing with a collection may be younger than that cycle's
  // snapshot, so perform one final quiescent collection before asserting the
  // exact retained set.
  nrq_gc_collect();
  ASSERT_TRUE(succeeded.load());
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(*static_cast<uint64_t*>(root), uint64_t{5000});
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{1});
  nrq_gc_root_unregister(&root);
  root = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(GcTest, ConcurrentCollectionRequestsFromMutatorsSerializeWithoutDeadlock) {
  nrq_gc_disable();
  nrq_gc_collect();
  void* root = nrq_gc_allocate(32, nullptr);
  ASSERT_NE(root, nullptr);
  nrq_gc_root_register(&root);

  auto request_collections = [] {
    nrq_gc_mutator_enter();
    for (int index = 0; index < 24; ++index) nrq_gc_collect();
    nrq_gc_mutator_leave();
  };
  std::thread first(request_collections);
  std::thread second(request_collections);
  first.join();
  second.join();

  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{1});
  nrq_gc_root_unregister(&root);
  root = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(GcTest, CollectionMarksIncrementallyWhileMutatorsRun) {
  nrq_gc_disable();
  nrq_gc_collect();
  safepoint_mutator_progress = 0;
  safepoint_trace_progress[0] = 0;
  safepoint_trace_progress[1] = 0;
  safepoint_trace_count = 0;
  void* first = nrq_gc_allocate(sizeof(Node), trace_safepoint_probe);
  void* second = nrq_gc_allocate(sizeof(Node), trace_safepoint_probe);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  nrq_gc_root_register(&first);
  nrq_gc_root_register(&second);

  std::atomic<bool> mutator_ready = false;
  std::atomic<bool> stop_mutator = false;
  std::thread mutator([&] {
    nrq_gc_mutator_enter();
    mutator_ready.store(true, std::memory_order_release);
    while (!stop_mutator.load(std::memory_order_acquire)) {
      safepoint_mutator_progress.fetch_add(1);
      nrq_gc_safepoint();
    }
    nrq_gc_mutator_leave();
  });
  while (!mutator_ready.load(std::memory_order_acquire) ||
         safepoint_mutator_progress.load() < 100)
    std::this_thread::yield();

  nrq_gc_collect();
  const int trace_count = safepoint_trace_count.load();
  const uint64_t progress_during_first_trace = safepoint_trace_progress[0].load();
  const uint64_t progress_during_second_trace = safepoint_trace_progress[1].load();
  stop_mutator.store(true, std::memory_order_release);
  mutator.join();
  ASSERT_EQ(trace_count, 2);
  // Marking is sliced and mutators are resumed between slices. The probe's
  // second trace therefore observes progress made since the first trace rather
  // than the frozen progress of a stop-the-world collector.
  ASSERT_TRUE(progress_during_second_trace > progress_during_first_trace);

  nrq_gc_root_unregister(&second);
  nrq_gc_root_unregister(&first);
  first = nullptr;
  second = nullptr;
  nrq_gc_collect();
}

TEST(GcTest, ThreadLocalBumpAllocationsPublishAtCollectionSafepoints) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();
  Node* survivor = nullptr;
  nrq_gc_root_register(reinterpret_cast<void**>(&survivor));
  nrq_gc_mutator_enter();
  constexpr size_t kAllocationCount = 128;
  std::vector<Node*> objects;
  objects.reserve(kAllocationCount);
  bool allocations_succeeded = true;
  for (size_t index = 0; index < kAllocationCount; ++index) {
    auto* object = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
    if (!object) allocations_succeeded = false;
    objects.push_back(object);
  }
  survivor = objects.back();
  nrq_gc_root_store(reinterpret_cast<void**>(&survivor), survivor);
  const uint64_t live_before_collection = nrq_gc_get_live_objects();

  nrq_gc_collect();
  const uint64_t live_after_collection = nrq_gc_get_live_objects();
  nrq_gc_mutator_leave();
  ASSERT_TRUE(allocations_succeeded);
  ASSERT_EQ(live_before_collection, baseline + kAllocationCount);
  ASSERT_EQ(live_after_collection, baseline + 1);

  nrq_gc_root_unregister(reinterpret_cast<void**>(&survivor));
  survivor = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
}

TEST(GcTest, RepeatedRootRegionKeepsContiguousArrayReferencesAlive) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();
  void* references[3] = {
      nrq_gc_allocate(24, nullptr), nrq_gc_allocate(24, nullptr),
      nrq_gc_allocate(24, nullptr)};
  ASSERT_NE(references[0], nullptr);
  ASSERT_NE(references[1], nullptr);
  ASSERT_NE(references[2], nullptr);
  const size_t pointer_offset = 0;
  nrq_gc_root_register_array(references, 3, sizeof(void*), &pointer_offset, 1);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline + 3);
  nrq_gc_root_unregister(reinterpret_cast<void**>(references));
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
}

TEST(GcTest, ArrayRootInitializationClearsOnlyReferenceSlots) {
  PairReferences elements[2]{};
  std::memset(elements, 0xff, sizeof(elements));
  const size_t offsets[] = {offsetof(PairReferences, first),
                            offsetof(PairReferences, second)};
  nrq_gc_root_clear_array_references(elements, 2, sizeof(PairReferences),
                                     offsets, 2);
  EXPECT_EQ(elements[0].first, nullptr);
  EXPECT_EQ(elements[0].second, nullptr);
  EXPECT_EQ(elements[1].first, nullptr);
  EXPECT_EQ(elements[1].second, nullptr);
}
TEST(GcTest, PinDiagnosticsReportCountAndRejectExcessivePins) {
  nrq_gc_enable();
  nrq_gc_collect();
  const uint64_t baseline_pins = nrq_gc_pin_count();
  const uint64_t baseline_violations = nrq_gc_pin_violations();
  ASSERT_EQ(baseline_pins, uint64_t{0});

  auto* node = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(nrq_gc_pin(node) == node);
  ASSERT_EQ(nrq_gc_pin_count(), baseline_pins + 1);

  // Repeating a pin without a matching unpin would keep the object alive
  // forever, so the runtime caps the per-object pin depth and records every
  // rejection instead of failing the process.
  int accepted = 0;
  for (int attempt = 0; attempt < 64; ++attempt) {
    if (nrq_gc_pin(node)) ++accepted;
  }
  ASSERT_TRUE(accepted > 0);
  ASSERT_TRUE(accepted < 64);
  ASSERT_EQ(nrq_gc_pin_violations() - baseline_violations,
            static_cast<uint64_t>(64 - accepted));
  ASSERT_EQ(nrq_gc_pin_count(), baseline_pins + 1);

  for (int pin = 0; pin <= accepted; ++pin) nrq_gc_unpin(node);
  ASSERT_EQ(nrq_gc_pin_count(), baseline_pins);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(GcTest, MinorCollectionPromotesSurvivorsAndRetainsOldGarbage) {
  nrq_gc_disable();
  nrq_gc_set_generational(1);
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();

  auto* survivor = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  ASSERT_NE(survivor, nullptr);
  nrq_gc_root_register(reinterpret_cast<void**>(&survivor));

  // Two young collections promote the rooted object into the old generation.
  nrq_gc_collect_minor();
  nrq_gc_collect_minor();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline + 1);

  nrq_gc_root_unregister(reinterpret_cast<void**>(&survivor));
  survivor = nullptr;
  nrq_gc_collect_minor();
  // The now-unreachable object is old; a young collection must retain it.
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline + 1);

  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
  ASSERT_TRUE(nrq_gc_get_minor_collection_count() >= 3);
  ASSERT_TRUE(nrq_gc_get_major_collection_count() >= 2);
}

TEST(GcTest, OldToYoungStoreBarrierKeepsYoungObjectAliveDuringMinorCollection) {
  nrq_gc_disable();
  nrq_gc_set_generational(1);
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();

  auto* old_parent = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  ASSERT_NE(old_parent, nullptr);
  nrq_gc_root_register(reinterpret_cast<void**>(&old_parent));
  nrq_gc_collect_minor();
  nrq_gc_collect_minor();  // Promote the parent into the old generation.

  auto* young_child = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
  ASSERT_NE(young_child, nullptr);
  // The store barrier records this old-to-young edge for the remembered set.
  nrq_gc_root_store(reinterpret_cast<void**>(&old_parent->next), young_child);

  nrq_gc_collect_minor();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline + 2);

  // Clear the only edge and force a full collection; the remembered set is
  // conservative between minors, so the floating young object is reclaimed by
  // the full collection rather than by a transitively stale remembered entry.
  nrq_gc_root_store(reinterpret_cast<void**>(&old_parent->next), nullptr);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline + 1);

  nrq_gc_root_unregister(reinterpret_cast<void**>(&old_parent));
  old_parent = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
}

TEST(GcTest, AutomaticCollectionStateIsObservable) {
  nrq_gc_disable();
  ASSERT_EQ(nrq_gc_is_enabled(), int32_t{0});
  nrq_gc_enable();
  ASSERT_EQ(nrq_gc_is_enabled(), int32_t{1});
  nrq_gc_disable();
  ASSERT_EQ(nrq_gc_is_enabled(), int32_t{0});
  // Explicit collection stays available while automatic collection is off.
  nrq_gc_collect();
  nrq_gc_enable();
  ASSERT_EQ(nrq_gc_is_enabled(), int32_t{1});
}

TEST(GcTest, AllocationPressureIsCollectedWithoutLosingLiveObjects) {
  nrq_gc_enable();
  nrq_gc_set_threshold(1024 * 1024 * 1024);
  nrq_gc_collect();
  constexpr int kObjects = 20000;
  std::vector<void*> objects;
  objects.reserve(kObjects);

  for (int index = 0; index < kObjects; ++index) {
    void* object = nrq_gc_allocate(64, nullptr);
    ASSERT_NE(object, nullptr);
    objects.push_back(object);
  }
  objects.clear();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
  nrq_gc_set_threshold(1024 * 1024);
}

TEST(GcTest, NestedRootFramesWithConcurrentCollection) {
  // Verifies that root frame begin/end correctly maintains root visibility
  // during concurrent GC collection cycles.
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();

  std::atomic<bool> stop_collector{false};
  std::thread collector_thread([&] {
    while (!stop_collector.load(std::memory_order_acquire))
      nrq_gc_collect();
  });

  // Run many nested frame cycles while the collector runs concurrently.
  for (int iteration = 0; iteration < 100; ++iteration) {
    auto* outer_obj = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
    ASSERT_NE(outer_obj, nullptr);
    const uint64_t outer_frame = nrq_gc_root_frame_begin();
    nrq_gc_root_register(reinterpret_cast<void**>(&outer_obj));

    {
      auto* inner_obj = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
      ASSERT_NE(inner_obj, nullptr);
      const uint64_t inner_frame = nrq_gc_root_frame_begin();
      nrq_gc_root_register(reinterpret_cast<void**>(&inner_obj));
      // inner_obj must remain live during collection while its frame is active.
      nrq_gc_root_frame_end(inner_frame);
      // After inner frame ends, inner_obj has no root -- may be collected.
    }

    nrq_gc_root_frame_end(outer_frame);
    // outer_obj has no root now; both objects may be collected.
  }

  stop_collector.store(true, std::memory_order_release);
  collector_thread.join();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
}

TEST(GcTest, RapidRootFrameCreationWithoutCollection) {
  // Exercises the fast path for root_frame_begin/end when the collector is idle.
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();

  for (int iteration = 0; iteration < 1000; ++iteration) {
    const uint64_t f1 = nrq_gc_root_frame_begin();
    const uint64_t f2 = nrq_gc_root_frame_begin();
    const uint64_t f3 = nrq_gc_root_frame_begin();
    ASSERT_NE(f1, f2);
    ASSERT_NE(f2, f3);
    nrq_gc_root_frame_end(f3);
    nrq_gc_root_frame_end(f2);
    nrq_gc_root_frame_end(f1);
  }

  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
}

TEST(GcTest, RootedAllocationValidationIsThreadLocal) {
  // Verifies that rooted allocation correctly validates root slot registration
  // and that unregistered slots are rejected.
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_mutator_enter();

  void* valid_root = nullptr;
  const uint64_t frame = nrq_gc_root_frame_begin();
  nrq_gc_root_register(&valid_root);

  // Allocate into the registered slot repeatedly to exercise the O(1) fast path.
  for (int index = 0; index < 10; ++index) {
    auto* object = static_cast<uint64_t*>(nrq_gc_allocate_array_typed_rooted(
        &valid_root, 1, sizeof(uint64_t), nullptr, 0));
    ASSERT_NE(object, nullptr);
    *object = static_cast<uint64_t>(index);
    ASSERT_EQ(*static_cast<uint64_t*>(valid_root), static_cast<uint64_t>(index));
  }

  nrq_gc_root_frame_end(frame);
  nrq_gc_mutator_leave();
  nrq_gc_collect();
}

TEST(GcTest, DeeplyNestedRootFramesRemoveOnlyOwnedRoots) {
  nrq_gc_disable();
  nrq_gc_collect();
  constexpr int kDepth = 128;
  std::vector<Node*> nodes(kDepth, nullptr);
  std::vector<uint64_t> frames;
  frames.reserve(kDepth);

  for (int index = 0; index < kDepth; ++index) {
    nodes[index] = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
    ASSERT_NE(nodes[index], nullptr);
    frames.push_back(nrq_gc_root_frame_begin());
    nrq_gc_root_register(reinterpret_cast<void**>(&nodes[index]));
  }

  // Ending an inner frame must leave every outer registration intact.
  for (int index = kDepth - 1; index > 0; --index)
    nrq_gc_root_frame_end(frames[index]);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{1});

  nrq_gc_root_frame_end(frames[0]);
  nodes[0] = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(GcTest, RecursiveManagedTraversalKeepsAncestorRootsVisible) {
  nrq_gc_disable();
  nrq_gc_collect();
  constexpr int kDepth = 64;
  uint64_t live_at_bottom = 0;
  recurse_with_roots(kDepth, live_at_bottom);
  ASSERT_EQ(live_at_bottom, static_cast<uint64_t>(kDepth));
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(GcTest, DuplicateRootSlotRegistrationsPreserveOuterOwners) {
  nrq_gc_disable();
  nrq_gc_collect();
  void* slot = nullptr;
  const uint64_t outer = nrq_gc_root_frame_begin();
  nrq_gc_root_register(&slot);

  const uint64_t inner = nrq_gc_root_frame_begin();
  nrq_gc_root_register(&slot);  // duplicate registration of the same slot
  nrq_gc_root_frame_end(inner);

  // The outer frame still owns the slot, so rooted allocation must accept it.
  auto* replacement =
      nrq_gc_allocate_array_typed_rooted(&slot, 1, sizeof(uint64_t), nullptr, 0);
  ASSERT_NE(replacement, nullptr);

  // Two entries inside one frame must both disappear when that frame ends.
  const uint64_t repeated = nrq_gc_root_frame_begin();
  nrq_gc_root_register(&slot);
  nrq_gc_root_register(&slot);
  nrq_gc_root_frame_end(repeated);
  ASSERT_NE(nrq_gc_allocate_array_typed_rooted(&slot, 1, sizeof(uint64_t),
                                               nullptr, 0),
            nullptr);

  nrq_gc_root_frame_end(outer);
  slot = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(GcTest, EarlyReturnUnwindsRootFrameAndReleasesRoots) {
  nrq_gc_disable();
  nrq_gc_collect();
  struct FrameGuard {
    uint64_t marker;
    ~FrameGuard() { nrq_gc_root_frame_end(marker); }
  };
  void* root = nullptr;
  bool returned_early = false;
  try {
    const uint64_t frame = nrq_gc_root_frame_begin();
    nrq_gc_root_register(&root);
    root = nrq_gc_allocate(sizeof(Node), trace_node);
    ASSERT_NE(root, nullptr);
    FrameGuard guard{frame};
    if (root != nullptr) throw 7;  // simulate early-return unwinding
    returned_early = true;
  } catch (int value) {
    ASSERT_EQ(value, 7);
  }
  ASSERT_TRUE(!returned_early);
  root = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(GcTest, ThreadTerminationWithActiveRootFramesRemovesDanglingRoots) {
  nrq_gc_disable();
  nrq_gc_collect();
  std::thread worker([] {
    nrq_gc_mutator_enter();
    void* local = nrq_gc_allocate(sizeof(Node), trace_node);
    const uint64_t frame = nrq_gc_root_frame_begin();
    nrq_gc_root_register(&local);
    (void)frame;  // exit without ending the frame or leaving the mutator
  });
  worker.join();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(GcTest, DuplicateRootRegistrationsInConcurrentCollectionStayConsistent) {
  nrq_gc_disable();
  nrq_gc_collect();
  std::atomic<bool> stop_collector{false};
  std::thread collector([&] {
    while (!stop_collector.load(std::memory_order_acquire)) nrq_gc_collect();
  });

  for (int iteration = 0; iteration < 200; ++iteration) {
    void* slot = nullptr;
    const uint64_t outer = nrq_gc_root_frame_begin();
    nrq_gc_root_register(&slot);
    const uint64_t inner = nrq_gc_root_frame_begin();
    nrq_gc_root_register(&slot);
    slot = nrq_gc_allocate(sizeof(Node), trace_node);
    nrq_gc_root_frame_end(inner);
    nrq_gc_root_frame_end(outer);
    slot = nullptr;
  }

  stop_collector.store(true, std::memory_order_release);
  collector.join();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}
