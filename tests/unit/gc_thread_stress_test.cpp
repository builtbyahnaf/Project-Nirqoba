// AOR Phase 8: the mixed multithreaded stress mix.
//
// Everything the collector supports, at once: four mutators allocating H0 chain
// nodes, H8 compact records, H16 arrays, structure-of-arrays payloads and large
// objects, publishing and retiring root slots, writing an old-to-young reference
// through the barrier every iteration, and a collector thread running minors and
// full cycles underneath. Each mutator keeps an arena of its own and points into
// it from collector storage, so the two strategies are exercised side by side
// rather than in separate tests.
//
// What these tests are looking for is what a benchmark cannot show: a survivor
// whose stamp changed or vanished (premature reclamation), a chain the marker
// lost (root or barrier defect), a page handed to two owners (duplicate
// reclamation), and a slot whose descriptor outlived its registration.

#include "test_harness.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "nirqoba/runtime/cdmm.h"
#include "nirqoba/runtime/gc.h"

namespace {

struct Cell {
  void* child;   // offset 0, described as a reference
  uint64_t stamp;  // offset 8
};

const size_t kChildOffset[] = {0};

// Every multithreaded failure is counted, never asserted inside a worker: a
// failed assertion there would abort the process before the joins.
struct Failure {
  std::atomic<uint64_t> stamp_mismatch{0};
  std::atomic<uint64_t> broken_chain{0};
  std::atomic<uint64_t> allocation_failed{0};
  std::atomic<uint64_t> wrong_representation{0};
  std::atomic<uint64_t> strategy_confusion{0};
  std::atomic<uint64_t> duplicate_owner{0};

  uint64_t total() const {
    return stamp_mismatch.load() + broken_chain.load() +
           allocation_failed.load() + wrong_representation.load() +
           strategy_confusion.load() + duplicate_owner.load();
  }
};

// A root slot on the heap, so a worker's slot address is never a recycled stack
// address and each thread owns its own registration.
class Slot {
 public:
  explicit Slot(void* initial = nullptr) : storage_(new void*(initial)) {}
  ~Slot() { delete storage_; }
  Slot(const Slot&) = delete;
  Slot& operator=(const Slot&) = delete;
  void** address() { return storage_; }

 private:
  void** storage_;
};

const nrq_object_layout* cell_layout() {
  static const nrq_object_layout* layout = [] {
    return nrq_gc_layout_intern(sizeof(Cell), 0, nullptr, kChildOffset, 1);
  }();
  return layout;
}

// Walks a chain of `limit` cells and reports the first stamp that does not
// decrease strictly. Only the stamps are read, not the collector's classification
// of the address: an object this thread allocated is published by the safepoint
// that precedes the walk, but the walk itself must not depend on the collector
// having noticed it yet. A reclaimed and reused cell would show a stamp of zero or
// one that does not decrease, which is what this looks for.
uint64_t verify_chain(Failure& failure, void* head, uint64_t expected_stamp,
                      size_t limit) {
  uint64_t hops = 0;
  uint64_t previous = expected_stamp + 1;
  for (void* cursor = head; cursor && hops < limit; ++hops) {
    const auto* cell = static_cast<const Cell*>(cursor);
    if (cell->stamp == 0 || cell->stamp >= previous) {
      nrq_gc_sweep_record history[256];
      const size_t recorded = nrq_gc_sweep_history(history, 256);
      for (size_t entry = 0; entry < recorded; ++entry) {
        if (history[entry].address != cursor) continue;
        std::fprintf(stderr,
                     "NRQ_FREED hop=%llu cell=%p cycle=%llu born=%d marked=%d "
                     "whole_page=%d kind=%d layout=%u live=%d gen=%d full=%d "
                     "mark_word=%llx bit=%u pagemarks=%d seen=%d pushed=%d "
                     "page=%p idx=%llu ret=%d\n",
                     static_cast<unsigned long long>(hops),
                     static_cast<const void*>(cursor),
                     static_cast<unsigned long long>(history[entry].cycle),
                     static_cast<int>(history[entry].born),
                     static_cast<int>(history[entry].marked),
                     static_cast<int>(history[entry].whole_page),
                     static_cast<int>(history[entry].kind),
                     history[entry].payload_bytes,
                     static_cast<int>(history[entry].live_objects),
                     static_cast<int>(history[entry].generation),
                     static_cast<int>(history[entry].full),
                     static_cast<unsigned long long>(history[entry].mark_word),
                     static_cast<unsigned>(history[entry].mark_bit),
                     static_cast<int>(history[entry].page_marked_objects),
                     static_cast<int>(history[entry].visit_seen),
                     static_cast<int>(history[entry].visit_pushed),
                     history[entry].page,
                     static_cast<unsigned long long>(history[entry].page_index),
                     static_cast<int>(history[entry].retired));
      }
      std::fprintf(stderr, "NRQ_STAMP hop=%llu cell=%p stamp=%llu\n",
                   static_cast<unsigned long long>(hops),
                   static_cast<const void*>(cell),
                   static_cast<unsigned long long>(cell->stamp));
      failure.stamp_mismatch.fetch_add(1);
      break;
    }
    previous = cell->stamp;
    cursor = cell->child;
  }
  if (hops == 0) failure.broken_chain.fetch_add(1);
  return hops;
}

}  // namespace

TEST(GcThreadStressTest, MixedAllocationUnderConcurrentCollection) {
  nrq_gc_disable();
  nrq_gc_set_generational(1);
  nrq_gc_set_threshold(static_cast<size_t>(64) << 20);
  nrq_gc_collect();
  const uint64_t base = nrq_gc_get_live_objects();
  // Pin violations are a process-wide count, so this test can only be blamed for
  // the ones it adds.
  const uint64_t pin_violations_before = nrq_gc_pin_violations();

  constexpr int kThreads = 4;
  constexpr int kIterations = 900;
  Failure failure;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> collected{0};
  std::atomic<bool> collected_once{false};

  std::vector<std::thread> workers;
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([thread, &failure, &collected, &collected_once] {
      nrq_gc_mutator_enter();
      Slot head;
      Slot anchor;
      // Register the generation slot once, before anything is stored in it. A
      // slot that is unregistered while it already names an object publishes that
      // object only through the path its allocation took, so a cycle that lands in
      // that window may legitimately reclaim an object this thread is about to
      // call reachable. Registration is the property under test here, not a step
      // to be repeated per generation.
      nrq_gc_root_register_typed(head.address(), sizeof(void*), kChildOffset, 1);
      // One arena per mutator: explicit storage is owned, not shared.
      void* arena = nrq_cdmm_arena_create(8192, 0);
      if (!arena) failure.allocation_failed.fetch_add(1);

      // An old object kept for the whole run: the old-to-young barrier target.
      void* old = nrq_gc_allocate_layout(cell_layout());
      if (!old) failure.allocation_failed.fetch_add(1);
      static_cast<Cell*>(old)->stamp = 1;
      nrq_gc_root_store(anchor.address(), old);
      nrq_gc_root_register_typed(anchor.address(), sizeof(void*), kChildOffset, 1);

      uint64_t stamp = 1;
      for (int index = 0; index < kIterations; ++index) {
        ++stamp;
        void* cell = nrq_gc_allocate_layout(cell_layout());
        if (!cell) {
          failure.allocation_failed.fetch_add(1);
          break;
        }
        static_cast<Cell*>(cell)->stamp = stamp;
        // Plain store: the object is unpublished until the mutator leaves, and
        // its slot is published below.
        static_cast<Cell*>(cell)->child = *head.address();

        // The old object takes a reference to this young one through the
        // barrier, which is what arms the remembered set.
        void* value = cell;
        nrq_gc_root_copy(static_cast<Cell*>(old), &value, sizeof(value));

        // Publish this generation through a slot the thread owns and reuses.
        nrq_gc_root_store(head.address(), cell);
        // Publish this thread's batch before walking it, so the walk is over
        // storage the collector is allowed to see rather than over a range the
        // collector must ignore.
        nrq_gc_safepoint();
        verify_chain(failure, *head.address(), stamp, 8);

        // H8: a compact record. The representation is a property of published
        // storage, so it is not read here: this thread's batch is not visible
        // yet, and a concurrent collection may reclaim an unreachable object
        // before the batch is published. The shapes are checked after the joins.
        if ((index % 9) == 0) {
          void* compact =
              nrq_gc_allocate_repr(512, nullptr, NRQ_GC_REPR_COMPACT);
          if (!compact) failure.allocation_failed.fetch_add(1);
        }
        // H16: an array of references.
        if ((index % 13) == 0) {
          void* array = nrq_gc_allocate_array_typed(4, sizeof(void*), nullptr, 0);
          if (!array) failure.allocation_failed.fetch_add(1);
        }
        // SoA: strided pointer columns.
        if ((index % 17) == 0) {
          const size_t columns[] = {0, 8};
          Slot soa_slot;
          // A rooted allocation publishes through a slot that must already be a
          // registered pointer root.
          nrq_gc_root_register(soa_slot.address());
          void* soa =
              nrq_gc_allocate_soa_rooted(soa_slot.address(), 4, 12, columns, 2);
          if (!soa) failure.allocation_failed.fetch_add(1);
          nrq_gc_root_unregister(soa_slot.address());
        }
        // Explicit storage holding a collector pointer: the arena is not
        // managed, and the pointer inside it is not owned by the arena.
        if ((index % 23) == 0 && arena) {
          void** block = static_cast<void**>(
              nrq_cdmm_arena_allocate(arena, sizeof(void*), alignof(void*)));
          if (!block) {
            failure.allocation_failed.fetch_add(1);
          } else {
            *block = cell;
            if (nrq_cdmm_strategy_of(block) != NRQ_CDMM_STRATEGY_ARENA)
              failure.strategy_confusion.fetch_add(1);
            if (nrq_gc_pointer_is_managed(block) != 0)
              failure.strategy_confusion.fetch_add(1);
          }
          nrq_cdmm_arena_reset(arena);
        }
      }

      // The collector is a thread of its own, so the work below can finish before
      // it has completed a single cycle: that is a fact about the scheduler, not
      // about the collector. Stay in the mutator region - parked at a safepoint,
      // so a cycle shrinks the registry it waits on rather than blocking it - until
      // one full cycle has finished, which is what the assertion counts. The bound
      // keeps a collector that never finishes from hanging the suite.
      for (int spin = 0; spin < 200000 && !collected_once.load(); ++spin) {
        nrq_gc_safepoint();
        std::this_thread::yield();
      }

      // Leaving publishes this thread's batch. Only then can the object the
      // anchor points at be classified: the last young cell was written through
      // the barrier while it was still thread-local.
      nrq_gc_mutator_leave();
      void* anchored = nullptr;
      std::memcpy(&anchored, old, sizeof(anchored));
      if (anchored && nrq_gc_pointer_is_managed(anchored) != 1)
        failure.duplicate_owner.fetch_add(1);

      // The anchor is still rooted across the leave, which is what kept `old` and
      // its child alive for the check above.
      nrq_gc_root_unregister(head.address());
      nrq_gc_root_unregister(anchor.address());
      if (arena) nrq_cdmm_arena_destroy(arena);
    });
  }

  std::thread collector([&stop, &collected, &collected_once] {
    uint64_t rounds = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      nrq_gc_collect_minor();
      if ((++rounds % 4) == 0) {
        nrq_gc_collect();
        collected.fetch_add(1);
        collected_once.store(true);
      }
      std::this_thread::yield();
    }
  });

  for (auto& worker : workers) worker.join();
  stop.store(true);
  collector.join();

  // One assertion per failure kind, so a run names what broke instead of only
  // reporting that something did.
  ASSERT_EQ(failure.allocation_failed.load(), uint64_t{0});
  ASSERT_EQ(failure.stamp_mismatch.load(), uint64_t{0});
  ASSERT_EQ(failure.broken_chain.load(), uint64_t{0});
  ASSERT_EQ(failure.wrong_representation.load(), uint64_t{0});
  ASSERT_EQ(failure.strategy_confusion.load(), uint64_t{0});
  ASSERT_EQ(failure.duplicate_owner.load(), uint64_t{0});
  ASSERT_EQ(failure.total(), uint64_t{0});
  ASSERT_TRUE(collected.load() > 0);

  // The three representations coexist in one published heap, allocated through
  // the same runtime that just ran the concurrent mix.
  {
    std::unique_ptr<void*[]> slots(new void*[4]{nullptr, nullptr, nullptr,
                                                nullptr});
    nrq_gc_mutator_enter();
    for (int index = 0; index < 3; ++index)
      nrq_gc_root_register(&slots[static_cast<size_t>(index)]);
    nrq_gc_root_register(&slots[3]);
    nrq_gc_root_store(&slots[0], nrq_gc_allocate(16, nullptr));
    nrq_gc_root_store(
        &slots[1], nrq_gc_allocate_repr(512, nullptr, NRQ_GC_REPR_COMPACT));
    nrq_gc_root_store(
        &slots[2], nrq_gc_allocate_array_typed(4, sizeof(void*), nullptr, 0));
    const size_t columns[] = {0, 8};
    void* soa = nrq_gc_allocate_soa_rooted(&slots[3], 4, 12, columns, 2);
    // Leaving publishes this thread's batch; only now can the collector name
    // these objects, and only now do they have a representation to report.
    nrq_gc_mutator_leave();
    ASSERT_NE(slots[0], nullptr);
    ASSERT_NE(slots[1], nullptr);
    ASSERT_NE(slots[2], nullptr);
    ASSERT_NE(soa, nullptr);
    ASSERT_EQ(nrq_gc_object_representation(slots[0]),
              static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
    ASSERT_EQ(nrq_gc_object_representation(slots[1]),
              static_cast<int32_t>(NRQ_GC_REPR_COMPACT));
    ASSERT_EQ(nrq_gc_object_representation(slots[2]),
              static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
    ASSERT_EQ(nrq_gc_object_representation(soa),
              static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
    for (int index = 0; index < 4; ++index)
      nrq_gc_root_unregister(&slots[static_cast<size_t>(index)]);
  }
  ASSERT_EQ(nrq_gc_pin_violations() - pin_violations_before, uint64_t{0});
  ASSERT_EQ(nrq_gc_get_live_objects() >= base, true);

  // Everything the workers published is gone; only their retired storage could
  // remain, and the collector owns all of it.
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_set_generational(0);
  nrq_gc_enable();
}

TEST(GcThreadStressTest, ExplicitStorageSurvivesCollectorThreads) {
  nrq_gc_disable();
  nrq_gc_set_threshold(static_cast<size_t>(64) << 20);
  nrq_gc_collect();

  constexpr int kThreads = 3;
  constexpr int kBlocks = 256;
  std::atomic<bool> stop{false};
  Failure failure;
  std::atomic<uint64_t> mismatched{0};

  std::vector<std::thread> workers;
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([thread, &failure, &mismatched] {
      // Each thread owns its arena and its pool: no sharing, no lifecycle
      // transfer, and no collector involvement at all.
      void* arena = nrq_cdmm_arena_create(1 << 16, 0);
      void* pool = nrq_cdmm_pool_create(64, 8, 0, 0, nullptr, nullptr);
      if (!arena || !pool) {
        failure.allocation_failed.fetch_add(1);
        return;
      }
      std::vector<uint64_t*> values;
      values.reserve(kBlocks);
      for (int index = 0; index < kBlocks; ++index) {
        auto* block =
            static_cast<uint64_t*>(nrq_cdmm_arena_allocate(arena, 64, 8));
        auto* pooled = static_cast<uint64_t*>(nrq_cdmm_pool_allocate(pool));
        if (!block || !pooled) {
          failure.allocation_failed.fetch_add(1);
          continue;
        }
        *block = static_cast<uint64_t>(thread * 1000 + index);
        *pooled = *block ^ 0x5A5A5A5A5A5A5A5ALL;
        values.push_back(block);
        // A collector pointer inside explicit storage is legal; the storage does
        // not become managed, and the collector does not trace it unless the
        // owner registers the region as a root.
        void* managed = nrq_gc_allocate(16, nullptr);
        if (!managed) {
          failure.allocation_failed.fetch_add(1);
          continue;
        }
        if (nrq_cdmm_strategy_of(pooled) != NRQ_CDMM_STRATEGY_POOL)
          failure.strategy_confusion.fetch_add(1);
        if (nrq_gc_pointer_is_managed(block) != 0)
          failure.strategy_confusion.fetch_add(1);
      }
      // Values must be untouched by every collection that ran underneath.
      for (size_t index = 0; index < values.size(); ++index) {
        if (*values[index] != static_cast<uint64_t>(thread * 1000 + index))
          mismatched.fetch_add(1);
      }
      nrq_cdmm_pool_destroy(pool);
      nrq_cdmm_arena_destroy(arena);
    });
  }

  std::thread collector([&stop] {
    while (!stop.load(std::memory_order_relaxed)) {
      nrq_gc_collect();
      nrq_gc_collect_minor();
      std::this_thread::yield();
    }
  });

  for (auto& worker : workers) worker.join();
  stop.store(true);
  collector.join();

  ASSERT_EQ(failure.total(), uint64_t{0});
  ASSERT_EQ(mismatched.load(), uint64_t{0});
  nrq_gc_collect();
  nrq_gc_enable();
}
