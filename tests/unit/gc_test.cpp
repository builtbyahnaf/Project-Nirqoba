#include "test_harness.h"

#include <cstddef>
#include <atomic>
#include <thread>
#include <vector>

#include "nirqoba/runtime/gc.h"
#include "nirqoba/runtime/allocator.h"

namespace {

struct Node {
  Node* next;
  uint64_t value;
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
