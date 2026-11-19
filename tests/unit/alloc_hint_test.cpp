// Phase 6: the compiler hint to allocation plan.
//
// The planner is a pure function, so these tests pin the policy it encodes and
// the rejection rules without allocating anything. The companion GC test then
// checks that the runtime's own choice agrees with the plan.

#include "test_harness.h"

#include <cstdint>

#include "nirqoba/runtime/alloc_hint.h"
#include "nirqoba/runtime/gc.h"

namespace {

NrqAllocHint hint(uint64_t payload_bytes, uint64_t element_count,
                  uint32_t flags = NRQ_ALLOC_HINT_MANAGED) {
  NrqAllocHint value{};
  value.payload_bytes = payload_bytes;
  value.element_count = element_count;
  value.alignment = 8;
  value.flags = flags;
  return value;
}

}  // namespace

TEST(AllocHintTest, SmallFixedRecordIsHeaderless) {
  const NrqAllocHint value = hint(64, 1);
  const NrqAllocPlan plan = nrq_alloc_plan(&value);
  ASSERT_EQ(plan.representation, NRQ_ALLOC_REPR_HEADERLESS);
  ASSERT_EQ(plan.header_bytes, uint64_t{0});
  ASSERT_EQ(plan.payload_bytes, uint64_t{64});
  ASSERT_EQ(plan.total_bytes, uint64_t{64});
  ASSERT_EQ(plan.slot_bytes, uint64_t{64});
  ASSERT_EQ(plan.flags & NRQ_ALLOC_PLAN_REJECTED, 0u);
}

TEST(AllocHintTest, LargeFixedRecordIsCompact) {
  const NrqAllocHint value = hint(1024, 1);
  const NrqAllocPlan plan = nrq_alloc_plan(&value);
  ASSERT_EQ(plan.representation, NRQ_ALLOC_REPR_COMPACT);
  ASSERT_EQ(plan.header_bytes, uint64_t{8});
  ASSERT_EQ(plan.slot_bytes, uint64_t{1024});
}

TEST(AllocHintTest, TheHeaderlessBoundaryIsTwoHundredFiftySixBytes) {
  const NrqAllocHint at_limit = hint(256, 1);
  const NrqAllocHint past_limit = hint(257, 1);
  ASSERT_EQ(nrq_alloc_plan(&at_limit).representation,
            NRQ_ALLOC_REPR_HEADERLESS);
  ASSERT_EQ(nrq_alloc_plan(&past_limit).representation, NRQ_ALLOC_REPR_COMPACT);
}

TEST(AllocHintTest, AnArrayIsExtendedExceptEligibleSingleElement) {
  const NrqAllocHint value = hint(16, 100, NRQ_ALLOC_HINT_MANAGED |
                                               NRQ_ALLOC_HINT_ARRAY);
  const NrqAllocPlan plan = nrq_alloc_plan(&value);
  ASSERT_EQ(plan.representation, NRQ_ALLOC_REPR_EXTENDED);
  ASSERT_EQ(plan.header_bytes, uint64_t{16});
  ASSERT_EQ(plan.payload_bytes, uint64_t{16});
  ASSERT_EQ(plan.total_bytes, uint64_t{1600});
  ASSERT_TRUE((plan.flags & NRQ_ALLOC_PLAN_ARRAY) != 0);
  // Eligible single-element arrays use Compact (H8) while keeping the array flag.
  const NrqAllocHint single = hint(24, 1, NRQ_ALLOC_HINT_MANAGED |
                                             NRQ_ALLOC_HINT_ARRAY);
  const NrqAllocPlan single_plan = nrq_alloc_plan(&single);
  ASSERT_EQ(single_plan.representation, NRQ_ALLOC_REPR_COMPACT);
  ASSERT_EQ(single_plan.header_bytes, uint64_t{8});
  ASSERT_TRUE((single_plan.flags & NRQ_ALLOC_PLAN_ARRAY) != 0);
  // Single-element arrays whose header+payload exceeds one page stay Extended.
  const NrqAllocHint oversized = hint(65529, 1, NRQ_ALLOC_HINT_MANAGED |
                                                  NRQ_ALLOC_HINT_ARRAY);
  ASSERT_EQ(nrq_alloc_plan(&oversized).representation, NRQ_ALLOC_REPR_EXTENDED);
}

TEST(AllocHintTest, ADeterministicSiteHasNoHeader) {
  const NrqAllocHint arena = hint(64, 1, NRQ_ALLOC_HINT_ARENA |
                                            NRQ_ALLOC_HINT_DETERMINISTIC);
  const NrqAllocPlan plan = nrq_alloc_plan(&arena);
  ASSERT_EQ(plan.representation, NRQ_ALLOC_REPR_NONE);
  ASSERT_EQ(plan.header_bytes, uint64_t{0});
  ASSERT_TRUE((plan.flags & NRQ_ALLOC_PLAN_DETERMINISTIC) != 0);
  const NrqAllocHint pool = hint(64, 1, NRQ_ALLOC_HINT_POOL);
  ASSERT_EQ(nrq_alloc_plan(&pool).header_bytes, uint64_t{0});
}

TEST(AllocHintTest, AnObjectPastAPageIsLargeExtended) {
  const NrqAllocHint value = hint(200000, 1);
  const NrqAllocPlan plan = nrq_alloc_plan(&value);
  ASSERT_EQ(plan.representation, NRQ_ALLOC_REPR_EXTENDED);
  ASSERT_TRUE((plan.flags & NRQ_ALLOC_PLAN_LARGE) != 0);
}

TEST(AllocHintTest, ReferencesAreReported) {
  NrqAllocHint value = hint(32, 1);
  value.reference_count = 2;
  const NrqAllocPlan plan = nrq_alloc_plan(&value);
  ASSERT_TRUE((plan.flags & NRQ_ALLOC_PLAN_HAS_REFERENCES) != 0);
}

TEST(AllocHintTest, ImpossibleHintsAreRejected) {
  const NrqAllocHint empty = hint(0, 1);
  ASSERT_TRUE((nrq_alloc_plan(&empty).flags & NRQ_ALLOC_PLAN_REJECTED) != 0);
  NrqAllocHint misaligned = hint(32, 1);
  misaligned.alignment = 3;
  const NrqAllocPlan plan = nrq_alloc_plan(&misaligned);
  ASSERT_TRUE((plan.flags & NRQ_ALLOC_PLAN_REJECTED) != 0);
  ASSERT_EQ(plan.representation, NRQ_ALLOC_REPR_NONE);
  ASSERT_TRUE((nrq_alloc_plan(nullptr).flags & NRQ_ALLOC_PLAN_REJECTED) != 0);
}

TEST(AllocHintTest, AlignmentRoundsTheSlotUp) {
  NrqAllocHint value = hint(24, 1);
  value.alignment = 16;
  ASSERT_EQ(nrq_alloc_plan(&value).slot_bytes, uint64_t{32});
  value.alignment = 64;
  ASSERT_EQ(nrq_alloc_plan(&value).slot_bytes, uint64_t{64});
}

TEST(AllocHintTest, PlanFlagsPackTheRepresentation) {
  const NrqAllocHint array = hint(24, 8, NRQ_ALLOC_HINT_ARRAY);
  const NrqAllocPlan plan = nrq_alloc_plan(&array);
  const uint32_t packed = nrq_alloc_plan_flags(&plan);
  ASSERT_TRUE((packed & NRQ_ALLOC_PLAN_ARRAY) != 0);
  ASSERT_EQ(NRQ_ALLOC_PLAN_REPR_OF(packed), NRQ_ALLOC_REPR_EXTENDED);
  ASSERT_TRUE((packed & NRQ_ALLOC_PLAN_REPEATED) != 0);
  // Array, repeated, extended: the compiler emits the same word for its sites.
  ASSERT_EQ(packed, 1u | 2u | (3u << 8));
}

TEST(AllocHintTest, LayoutDescriptorPlansMatchTheHintPolicy) {
  const nrq_object_layout* record =
      nrq_gc_layout_intern(sizeof(void*), 0, nullptr, nullptr, 0);
  ASSERT_NE(record, nullptr);
  const NrqAllocPlan record_plan = nrq_alloc_plan_for_layout(record, 1);
  ASSERT_EQ(record_plan.representation, NRQ_ALLOC_REPR_HEADERLESS);
  ASSERT_EQ(record_plan.payload_bytes, uint64_t{sizeof(void*)});

  const size_t offsets[1] = {0};
  const nrq_object_layout* array =
      nrq_gc_layout_intern(sizeof(void*), sizeof(void*), nullptr, offsets, 1);
  ASSERT_NE(array, nullptr);
  const NrqAllocPlan array_plan = nrq_alloc_plan_for_layout(array, 4);
  ASSERT_EQ(array_plan.representation, NRQ_ALLOC_REPR_EXTENDED);
  ASSERT_EQ(array_plan.total_bytes, uint64_t{4 * sizeof(void*)});
  ASSERT_EQ(nrq_alloc_plan_for_layout(nullptr, 1).flags &
                NRQ_ALLOC_PLAN_REJECTED,
            NRQ_ALLOC_PLAN_REJECTED);
}
