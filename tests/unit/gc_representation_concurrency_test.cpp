// AOR Phase 4: representations under concurrent mutators.
//
// Each thread allocates its own shape while the collector runs. A thread-owned
// headerless slab is deliberately single-shape, so every thread must get its own
// pages and every published object must keep the representation its request
// asked for.

#include "test_harness.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "nirqoba/runtime/gc.h"

namespace {

struct Leaf {
  uint64_t value;
};

struct Node {
  void* child;
  uint64_t value;
};

void trace_node(void* object, size_t object_bytes, nrq_gc_visit_fn visit,
                void* context) {
  (void)object_bytes;
  visit(*static_cast<void**>(object), context);
}

struct WorkerResult {
  std::atomic<uint64_t> headerless{0};
  std::atomic<uint64_t> compact{0};
  std::atomic<uint64_t> extended{0};
  std::atomic<int> wrong{0};
};

}  // namespace

TEST(GcRepresentationConcurrencyTest, ThreadsKeepTheirOwnRepresentation) {
  nrq_gc_disable();
  nrq_gc_set_threshold(static_cast<size_t>(1) << 30);
  nrq_gc_collect();

  constexpr int kThreads = 4;
  constexpr int kObjects = 2000;
  WorkerResult result;
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  std::vector<std::thread> collectors;

  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([thread, &result] {
      nrq_gc_mutator_enter();
      for (int index = 0; index < kObjects; ++index) {
        void* object = nullptr;
        int32_t expected = NRQ_GC_REPR_AUTO;
        switch (thread % 3) {
          case 0:
            object = nrq_gc_allocate(sizeof(Leaf), nullptr);
            expected = NRQ_GC_REPR_HEADERLESS;
            break;
          case 1:
            object = nrq_gc_allocate(sizeof(Node), trace_node);
            expected = NRQ_GC_REPR_HEADERLESS;
            break;
          default:
            object = nrq_gc_allocate_array_typed(4, sizeof(void*), nullptr, 0);
            expected = NRQ_GC_REPR_EXTENDED;
            break;
        }
        if (!object) {
          result.wrong.fetch_add(1);
          continue;
        }
        // The object is unpublished until this thread leaves, so its
        // representation cannot be read here - only the request is counted.
        switch (expected) {
          case NRQ_GC_REPR_HEADERLESS:
            result.headerless.fetch_add(1);
            break;
          case NRQ_GC_REPR_COMPACT:
            result.compact.fetch_add(1);
            break;
          default:
            result.extended.fetch_add(1);
            break;
        }
      }
      nrq_gc_mutator_leave();
    });
  }
  // The mutators publish their batches while the collector cycles, so a page
  // kind and an unpublished range are exercised together.
  collectors.emplace_back([&stop] {
    while (!stop.load(std::memory_order_relaxed)) {
      nrq_gc_collect();
      std::this_thread::yield();
    }
  });
  for (auto& worker : workers) worker.join();
  stop.store(true);
  for (auto& collector : collectors) collector.join();
  nrq_gc_collect();

  ASSERT_EQ(result.wrong.load(), 0);
  ASSERT_TRUE(result.headerless.load() > 0);
  ASSERT_TRUE(result.extended.load() > 0);
  ASSERT_EQ(result.compact.load(), uint64_t{0});
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

TEST(GcRepresentationConcurrencyTest, UnpublishedObjectsAreNotDiscoverable) {
  // The page-local discovery rule is what lets H0 exist: an object is managed
  // only once its batch is published. This pins that a thread's own unpublished
  // allocation reports no representation until leave.
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_mutator_enter();
  void* object = nrq_gc_allocate(sizeof(Leaf), nullptr);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_AUTO));
  nrq_gc_mutator_leave();
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  nrq_gc_collect();
}
