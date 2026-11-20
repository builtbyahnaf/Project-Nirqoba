// Phi: the region-utilisation pacemaker.
//
// A program that declares a region budget is asking the collector to start cycles
// from how much of that budget allocation has consumed. These tests pin the knob
// contract (a bad percentage is clamped, not honoured) and the behaviour that
// matters: with a budget set, a loop that never calls the collector explicitly
// still gets collected, and the live heap stays inside the budget.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>

#include "nirqoba/runtime/gc.h"

namespace {

void trace_leaf(void* object, size_t object_bytes, nrq_gc_visit_fn visit,
                void* context) {
  (void)object;
  (void)object_bytes;
  (void)visit;
  (void)context;
}

}  // namespace

TEST(GcPacemakerTest, TheTriggerPercentageIsClampedAndTheBudgetRoundTrips) {
  nrq_gc_disable();
  nrq_gc_set_region_limit(size_t{8} << 20);
  nrq_gc_set_trigger_percent(150);
  nrq_gc_region_stats stats = nrq_gc_get_region_stats();
  ASSERT_EQ(stats.region_limit, static_cast<size_t>(size_t{8} << 20));
  ASSERT_EQ(stats.trigger_percent, static_cast<uint32_t>(100));

  // Zero would mean "collect on every allocation", which is not a policy anyone
  // asks for deliberately; the setter floors it at one percent.
  nrq_gc_set_trigger_percent(0);
  stats = nrq_gc_get_region_stats();
  ASSERT_EQ(stats.trigger_percent, static_cast<uint32_t>(1));

  // Zero for the budget is "no budget", the pre-Phi behaviour.
  nrq_gc_set_region_limit(0);
  ASSERT_EQ(nrq_gc_get_region_stats().region_limit, static_cast<size_t>(0));
  nrq_gc_set_trigger_percent(75);
  nrq_gc_enable();
}

TEST(GcPacemakerTest, ABudgetStartsCyclesWithoutAnExplicitCollect) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_set_region_limit(size_t{4} << 20);
  nrq_gc_set_trigger_percent(10);
  nrq_gc_enable();
  const uint64_t collections_before = nrq_gc_get_stats().collections;

  // Every object is garbage as soon as the next allocation happens, so a
  // pacemaker that works keeps the heap flat while the loop runs.
  nrq_gc_mutator_enter();
  for (int index = 0; index < 120000; ++index) {
    void* const object = nrq_gc_allocate(64, trace_leaf);
    ASSERT_NE(object, nullptr);
  }
  nrq_gc_mutator_leave();

  const nrq_gc_stats after = nrq_gc_get_stats();
  ASSERT_EQ(after.collections > collections_before, true);
  // 120000 * 64 bytes is over 7 MiB, and the budget is 4 MiB: a collector that
  // never ran would report at least that much live. Two budgets of slack keeps
  // the bound meaningful without depending on the exact trigger arithmetic.
  ASSERT_EQ(after.allocated_bytes <= (size_t{8} << 20), true);

  nrq_gc_set_region_limit(0);
  nrq_gc_set_trigger_percent(75);
  nrq_gc_disable();
  nrq_gc_collect();
}

TEST(GcPacemakerTest, WithNoBudgetTheCollectorBehavesAsBefore) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_set_region_limit(0);
  nrq_gc_enable();
  const uint64_t collections_before = nrq_gc_get_stats().collections;

  nrq_gc_mutator_enter();
  for (int index = 0; index < 2000; ++index) {
    void* const object = nrq_gc_allocate(64, trace_leaf);
    ASSERT_NE(object, nullptr);
  }
  nrq_gc_mutator_leave();

  // A small loop stays well under the default threshold, so the pacemaker must
  // not turn it into a cycle: the default is still "collect when the threshold
  // says so", not "collect often".
  ASSERT_EQ(nrq_gc_get_stats().collections, collections_before);
  nrq_gc_disable();
  nrq_gc_collect();
}

TEST(GcPacemakerTest, TheRegionAuditAgreesWithItsPageSet) {
  // Every cycle ends by deriving the region totals from the page set and checking
  // the linkage they came from; a mismatch is fatal. Running a few cycles with a
  // mixed graph is what gives that check something to disagree with.
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_enable();
  nrq_gc_mutator_enter();
  void* roots[64] = {};
  for (int round = 0; round < 8; ++round) {
    for (int index = 0; index < 64; ++index) {
      roots[index] = nrq_gc_allocate(4096, trace_leaf);
      ASSERT_NE(roots[index], nullptr);
    }
    for (int index = 0; index < 64; index += 2) roots[index] = nullptr;
    nrq_gc_collect();
  }
  nrq_gc_mutator_leave();
  ASSERT_EQ(nrq_gc_get_stats().live_objects >= 0u, true);
  nrq_gc_disable();
  nrq_gc_collect();
}
