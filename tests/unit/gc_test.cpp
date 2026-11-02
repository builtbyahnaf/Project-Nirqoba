#include "test_harness.h"

#include <cstddef>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "nirqoba/runtime/gc.h"
#include "nirqoba/runtime/allocator.h"

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

}  // namespace

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
      for (int index = 0; index < 128; ++index) {
        if (!nrq_gc_allocate(48, nullptr)) succeeded = false;
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  ASSERT_TRUE(succeeded.load());
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{512});
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
  nrq_gc_set_threshold(1024 * 1024);
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
  ASSERT_TRUE(succeeded.load());
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(*static_cast<uint64_t*>(root), uint64_t{5000});
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{1});
  nrq_gc_root_unregister(&root);
  root = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
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
