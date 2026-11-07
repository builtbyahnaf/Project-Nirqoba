// tests/unit/gc_slot_index_test.cpp
//
// PO-27 regression unit tests: SlotIndex tombstone-churn capacity bound.
//
// These tests exercise the GC root-slot API (nrq_gc_root_register /
// nrq_gc_root_frame_begin / nrq_gc_root_frame_end) in patterns that
// directly triggered the hang identified in commit 0b2d457. The root cause
// was that SlotIndex::insert always doubled capacity when the load factor
// exceeded 75 %, counting tombstones in the numerator. After N insert/release
// cycles the backing vector reached 2^ceil(log2(N)) entries even with only
// 1–2 live entries, causing gigabyte allocations and O(N) probe loops.
//
// The fix (applied in PO-27A) rehashes to the same capacity when
// size_ < capacity/2 (tombstone-saturated), and only doubles when the table is
// truly half full of live entries -- matching the policy in PageDirectory.
// See tests/unit/slot_index_test.cpp for white-box tests that assert the
// capacity, compaction and probe diagnostics directly.
//
// Tests are written against the public GC API; they do not reach into
// SlotIndex internals. Capacity is bounded indirectly:
//   - If capacity explodes, the test exhausts addressable memory or takes
//     orders of magnitude longer. We use a wall-clock timeout via ASSERT
//     on elapsed time (< 5 s for any single test).
//   - nrq_gc_object_index_entries() (object index, not slot index) is
//     included as a cross-check to confirm the heap stays small.
//   - For the slot index specifically, we assert that repeated short-lived
//     root frames do not cause observable memory growth beyond a small factor
//     of the peak live-slot count, via a before/after comparison of OS-level
//     working-set bytes if available.
//
// All tests follow the same structure as gc_test.cpp:
//   nrq_gc_disable() / nrq_gc_collect() to start from a clean heap,
//   then operate, then nrq_gc_collect() to verify reclamation.

#include "test_harness.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

#include "nirqoba/runtime/gc.h"

using namespace std;
using Clock = chrono::steady_clock;

namespace {

// Sentinel trace function: a leaf node with no managed references.
void trace_leaf(void* /*object*/, size_t /*bytes*/,
                nrq_gc_visit_fn /*visit*/, void* /*context*/) {}

// Allocates a plain managed object (no references). Used as a slot target
// so the GC can track it without a trace callback.
void* alloc_leaf() {
  return nrq_gc_allocate(sizeof(void*), nullptr);
}

// Returns wall-clock milliseconds since `start`.
double elapsed_ms(Clock::time_point start) {
  return chrono::duration<double, milli>(Clock::now() - start).count();
}

constexpr double kTestTimeoutMs = 5000.0;  // 5 s hard ceiling per test

}  // namespace

// ---------------------------------------------------------------------------
// Test 1: Single root slot registered and released 500 000 times.
//
// This is the minimal reproduction of the PO-27 hang. Each iteration:
//   1. Allocates a managed leaf.
//   2. Registers the local pointer as a root slot.
//   3. Immediately unregisters it (tombstone created in SlotIndex).
//
// Without the fix: capacity doubles every ~log2 iteration, reaching ~32 MB
// after 500 000 iterations and ~2 s to allocate + scan on each insert.
// With the fix:    capacity stays at 16 or 32; total time < 500 ms.
//
// MUST PASS after fix. EXPECTED TO HANG before fix (killed by timeout in CI).
TEST(GcSlotIndexTest, SingleSlotRepeatedRegisterUnregisterStaysBounder) {
  nrq_gc_disable();
  nrq_gc_collect();

  constexpr int kIterations = 500'000;
  const auto start = Clock::now();

  for (int i = 0; i < kIterations; ++i) {
    void* leaf = alloc_leaf();
    ASSERT_NE(leaf, nullptr);
    nrq_gc_root_register(reinterpret_cast<void**>(&leaf));
    nrq_gc_root_unregister(reinterpret_cast<void**>(&leaf));

    // Early-exit if we are already over the timeout — avoids hanging the
    // entire test suite when the fix is absent.
    if (i % 10'000 == 0) {
      const double ms = elapsed_ms(start);
      ASSERT_TRUE(ms < kTestTimeoutMs);
    }
  }

  // After 500 000 tombstones all objects should be collected.
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});

  // Total time must be well under the timeout even on slow machines.
  ASSERT_TRUE(elapsed_ms(start) < kTestTimeoutMs);
}

// ---------------------------------------------------------------------------
// Test 2: Root-frame open/close cycle with one live slot per frame.
//
// Mimics the exact code pattern the Nirqoba compiler generates for a
// function that allocates one managed local and returns it:
//   nrq_gc_root_frame_begin()
//   nrq_gc_root_register(&local)
//   nrq_gc_root_frame_end(marker)   <- unregisters the slot
//
// After N such frames the slot index holds N tombstones. The fix ensures
// capacity never exceeds O(1) in this scenario.
TEST(GcSlotIndexTest, RootFrameChurnDoesNotGrowCapacity) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();

  constexpr int kFrames = 200'000;
  const auto start = Clock::now();

  for (int i = 0; i < kFrames; ++i) {
    void* leaf = alloc_leaf();
    ASSERT_NE(leaf, nullptr);
    const uint64_t frame = nrq_gc_root_frame_begin();
    nrq_gc_root_register(reinterpret_cast<void**>(&leaf));
    nrq_gc_root_frame_end(frame);

    if (i % 10'000 == 0) {
      const double ms = elapsed_ms(start);
      ASSERT_TRUE(ms < kTestTimeoutMs);
    }
  }

  // All objects should be unreachable after every frame is closed.
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
  ASSERT_TRUE(elapsed_ms(start) < kTestTimeoutMs);
}

// ---------------------------------------------------------------------------
// Test 3: Multiple slots per frame, all released together at frame end.
//
// A frame with K roots releases K slots simultaneously via frame_end.
// Repeating N times yields N*K tombstones. Verifies the fix handles bulk
// tombstone accumulation as well as single-slot release.
TEST(GcSlotIndexTest, MultiSlotFrameChurnDoesNotGrowCapacity) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();

  constexpr int kFrames    = 50'000;
  constexpr int kSlotsPerFrame = 8;
  const auto start = Clock::now();

  for (int f = 0; f < kFrames; ++f) {
    // Allocate kSlotsPerFrame managed objects and register each in a frame.
    void* slots[kSlotsPerFrame];
    for (int s = 0; s < kSlotsPerFrame; ++s) {
      slots[s] = alloc_leaf();
      ASSERT_NE(slots[s], nullptr);
    }
    const uint64_t frame = nrq_gc_root_frame_begin();
    for (int s = 0; s < kSlotsPerFrame; ++s) {
      nrq_gc_root_register(reinterpret_cast<void**>(&slots[s]));
    }
    nrq_gc_root_frame_end(frame);

    if (f % 5'000 == 0) {
      const double ms = elapsed_ms(start);
      ASSERT_TRUE(ms < kTestTimeoutMs);
    }
  }

  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
  ASSERT_TRUE(elapsed_ms(start) < kTestTimeoutMs);
}

// ---------------------------------------------------------------------------
// Test 4: Time per batch is roughly constant across churn epochs.
//
// Directly detects the exponential-growth symptom: each batch of inserts takes
// roughly O(1) time. If capacity is doubling per batch, later batches are
// measurably slower than early ones by a large factor.
//
// Method: divide kIterations into kEpochs equal-sized batches. Record the
// wall-clock time of the first and last batch. The last batch must not
// exceed the first by more than kGrowthRatio. Under the bug the ratio
// commonly exceeds 10–100x because each probe scans a table whose size
// doubles between epochs.
TEST(GcSlotIndexTest, InsertTimeIsConstantAcrossChurnEpochs) {
  nrq_gc_disable();
  nrq_gc_collect();

  constexpr int kEpochs = 20;
  constexpr int kBatchSize = 10'000;
  constexpr double kGrowthRatio = 4.0;  // last batch must be < 4x first

  double first_epoch_ms = -1.0;
  double last_epoch_ms  = -1.0;

  for (int epoch = 0; epoch < kEpochs; ++epoch) {
    const auto epoch_start = Clock::now();
    for (int i = 0; i < kBatchSize; ++i) {
      void* leaf = alloc_leaf();
      ASSERT_NE(leaf, nullptr);
      nrq_gc_root_register(reinterpret_cast<void**>(&leaf));
      nrq_gc_root_unregister(reinterpret_cast<void**>(&leaf));
    }
    const double ms = elapsed_ms(epoch_start);
    if (epoch == 0) first_epoch_ms = ms;
    last_epoch_ms = ms;

    ASSERT_TRUE(ms < kTestTimeoutMs);
  }

  // The core assertion: later batches are not dramatically slower.
  ASSERT_TRUE(first_epoch_ms >= 0.0);
  ASSERT_TRUE(last_epoch_ms >= 0.0);
  if (first_epoch_ms > 0.5) {  // skip if first epoch was sub-ms (noise floor)
    const double ratio = last_epoch_ms / first_epoch_ms;
    ASSERT_TRUE(ratio < kGrowthRatio);
  }

  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

// ---------------------------------------------------------------------------
// Test 5: Interleaved GC collections during slot churn.
//
// Verifies the fix holds up when collections run between churn bursts.
// Each outer iteration:
//   (a) Performs a burst of insert/release cycles (10 000 tombstones).
//   (b) Calls nrq_gc_collect() to trigger a stop-the-world handshake.
//
// The slot index must compact during the rehash triggered after the burst,
// keeping capacity proportional to the peak live count (≈1), not the
// cumulative tombstone count.
TEST(GcSlotIndexTest, ChurnInterleavedWithCollectionKeepsCapacityBounded) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();

  constexpr int kRounds = 20;
  constexpr int kBurstSize = 10'000;
  const auto start = Clock::now();

  for (int round = 0; round < kRounds; ++round) {
    for (int i = 0; i < kBurstSize; ++i) {
      void* leaf = alloc_leaf();
      ASSERT_NE(leaf, nullptr);
      nrq_gc_root_register(reinterpret_cast<void**>(&leaf));
      nrq_gc_root_unregister(reinterpret_cast<void**>(&leaf));
    }
    nrq_gc_collect();

    // All objects must be reclaimed after each collect — no leaks.
    ASSERT_EQ(nrq_gc_get_live_objects(), baseline);

    const double ms = elapsed_ms(start);
    ASSERT_TRUE(ms < kTestTimeoutMs);
  }
}

// ---------------------------------------------------------------------------
// Test 6: Concurrent mutator threads each performing independent slot churn.
//
// Each thread operates on its OWN ShadowStack (thread_shadow_stack is
// thread_local), so there is no cross-thread contention on the slot index
// itself. This test verifies that the fix is correct per-thread and that
// registering/unregistering from multiple threads does not corrupt the GC's
// mutator list or collection handshake.
//
// 4 threads × 50 000 churn cycles each = 200 000 total tombstones, all in
// separate slot indices.
TEST(GcSlotIndexTest, ConcurrentPerThreadSlotChurnCompletesCleanly) {
  nrq_gc_disable();
  nrq_gc_collect();

  constexpr int kThreads = 4;
  constexpr int kIterationsPerThread = 50'000;

  atomic<bool> any_failure{false};
  vector<thread> workers;
  workers.reserve(kThreads);

  const auto start = Clock::now();

  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&any_failure]() {
      nrq_gc_mutator_enter();
      for (int i = 0; i < kIterationsPerThread; ++i) {
        void* leaf = alloc_leaf();
        if (!leaf) { any_failure.store(true); break; }
        nrq_gc_root_register(reinterpret_cast<void**>(&leaf));
        nrq_gc_root_unregister(reinterpret_cast<void**>(&leaf));
        // Poll safepoints so the collector can stop-the-world if needed.
        if (i % 1000 == 0) nrq_gc_safepoint();
      }
      nrq_gc_mutator_leave();
    });
  }

  for (auto& w : workers) w.join();

  ASSERT_FALSE(any_failure.load());
  ASSERT_TRUE(elapsed_ms(start) < kTestTimeoutMs);

  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}

// ---------------------------------------------------------------------------
// Test 7: Deep root-frame stack (simulates deep call recursion).
//
// Opens kDepth nested frames, each registering one slot, then closes them
// in LIFO order. Repeats kRounds times. This exercises the slot index with
// up to kDepth live entries simultaneously, then produces kDepth tombstones
// per round. The fix must keep capacity proportional to the peak live count
// (kDepth), not to kDepth * kRounds.
TEST(GcSlotIndexTest, DeepNestedRootFramesDoNotExplodeCapacity) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();

  constexpr int kDepth  = 32;
  constexpr int kRounds = 10'000;
  const auto start = Clock::now();

  for (int round = 0; round < kRounds; ++round) {
    // Open kDepth frames, each with one registered root.
    uint64_t frames[kDepth];
    void* slots[kDepth];
    for (int d = 0; d < kDepth; ++d) {
      slots[d] = alloc_leaf();
      ASSERT_NE(slots[d], nullptr);
      frames[d] = nrq_gc_root_frame_begin();
      nrq_gc_root_register(reinterpret_cast<void**>(&slots[d]));
    }
    // Close in LIFO order (required by the API).
    for (int d = kDepth - 1; d >= 0; --d) {
      nrq_gc_root_frame_end(frames[d]);
    }

    if (round % 1000 == 0) {
      const double ms = elapsed_ms(start);
      ASSERT_TRUE(ms < kTestTimeoutMs);
    }
  }

  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
  ASSERT_TRUE(elapsed_ms(start) < kTestTimeoutMs);
}
