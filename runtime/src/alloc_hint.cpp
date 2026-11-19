// Compiler hint to allocation plan.
//
// The plan is the allocator's own representation policy expressed as a value:
// eligible non-SoA one-element arrays up to a page choose H8 (Compact), multi-
// element or SoA arrays choose H16 (Extended), fixed records up to the
// headerless limit choose H0, larger records choose H8, and anything past a page
// is a large extended object. Keeping it here, as a pure function of the hint,
// lets a compiler compute the same answer statically and lets the tests compare
// both answers without allocating anything.

#include "nirqoba/runtime/alloc_hint.h"

namespace {

// These mirror the collector's own constants. If one moves the other must move
// with it; the planned-allocation tests compare this plan against the
// representation the runtime actually chose, so a drift fails a test rather than
// silently disagreeing with the allocator.
constexpr uint64_t kPlanHeaderlessMaxPayload = 256;
constexpr uint64_t kPlanLargeThreshold = 64 * 1024;
constexpr uint64_t kPlanAlignment = 16;
constexpr uint64_t kPlanCompactHeaderBytes = 8;
constexpr uint64_t kPlanExtendedHeaderBytes = 16;
constexpr uint64_t kPlanSpanAlignment = 8;

bool power_of_two(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

NrqAllocPlan rejected_plan(uint64_t payload_bytes, uint64_t alignment) {
  NrqAllocPlan plan{};
  plan.representation = NRQ_ALLOC_REPR_NONE;
  plan.flags = NRQ_ALLOC_PLAN_REJECTED;
  plan.payload_bytes = payload_bytes;
  plan.alignment = alignment;
  return plan;
}

NrqAllocPlan plan_for(uint64_t payload_bytes, uint64_t element_count,
                      uint64_t alignment, uint32_t reference_count,
                      bool array, bool deterministic, bool soa = false) {
  if (payload_bytes == 0) return rejected_plan(payload_bytes, alignment);
  if (element_count == 0) element_count = 1;
  if (alignment == 0) alignment = kPlanAlignment;
  if (!power_of_two(alignment)) return rejected_plan(payload_bytes, alignment);
  if (alignment < kPlanAlignment) alignment = kPlanAlignment;
  if (element_count > UINT64_MAX / payload_bytes)
    return rejected_plan(payload_bytes, alignment);

  NrqAllocPlan plan{};
  plan.alignment = alignment;
  plan.payload_bytes = payload_bytes;
  plan.total_bytes = payload_bytes * element_count;
  plan.flags = array ? (NRQ_ALLOC_PLAN_ARRAY | NRQ_ALLOC_PLAN_REPEATED) : 0;
  if (reference_count != 0) plan.flags |= NRQ_ALLOC_PLAN_HAS_REFERENCES;

  if (deterministic) {
    // An arena or pool object carries no header: the region names the layout.
    plan.representation = NRQ_ALLOC_REPR_NONE;
    plan.flags |= NRQ_ALLOC_PLAN_DETERMINISTIC;
    plan.header_bytes = 0;
    plan.slot_bytes = align_up(payload_bytes, alignment);
    return plan;
  }
  const bool compact_array =
      array && !soa && element_count == 1 &&
      payload_bytes <= kPlanLargeThreshold - kPlanCompactHeaderBytes &&
      align_up(kPlanCompactHeaderBytes + payload_bytes, kPlanSpanAlignment) <=
          kPlanLargeThreshold;
  if (array) {
    if (compact_array) {
      plan.representation = NRQ_ALLOC_REPR_COMPACT;
      plan.header_bytes = kPlanCompactHeaderBytes;
    } else {
      plan.representation = NRQ_ALLOC_REPR_EXTENDED;
      plan.header_bytes = kPlanExtendedHeaderBytes;
    }
  } else if (payload_bytes > kPlanLargeThreshold) {
    plan.representation = NRQ_ALLOC_REPR_EXTENDED;
    plan.header_bytes = kPlanExtendedHeaderBytes;
  } else if (payload_bytes <= kPlanHeaderlessMaxPayload) {
    plan.representation = NRQ_ALLOC_REPR_HEADERLESS;
    plan.header_bytes = 0;
  } else {
    plan.representation = NRQ_ALLOC_REPR_COMPACT;
    plan.header_bytes = kPlanCompactHeaderBytes;
  }
  if (plan.total_bytes > kPlanLargeThreshold) plan.flags |= NRQ_ALLOC_PLAN_LARGE;
  plan.slot_bytes = align_up(payload_bytes, alignment);
  return plan;
}

}  // namespace

extern "C" {

NrqAllocPlan nrq_alloc_plan(const NrqAllocHint* hint) {
  if (!hint) return rejected_plan(0, 0);
  const bool array = (hint->flags & NRQ_ALLOC_HINT_ARRAY) != 0 ||
                     hint->element_count > 1;
  const bool deterministic = (hint->flags & NRQ_ALLOC_HINT_DETERMINISTIC) != 0 ||
                             (hint->flags & (NRQ_ALLOC_HINT_ARENA |
                                             NRQ_ALLOC_HINT_POOL)) != 0;
  return plan_for(hint->payload_bytes, hint->element_count, hint->alignment,
                  hint->reference_count, array, deterministic);
}

NrqAllocPlan nrq_alloc_plan_for_layout(const struct nrq_object_layout* layout,
                                       uint64_t element_count) {
  if (!layout) return rejected_plan(0, 0);
  const bool array = (layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) != 0;
  const bool soa =
      layout->soa_column_pairs != 0 && layout->soa_columns != nullptr;
  return plan_for(layout->payload_size, array ? element_count : 1,
                  kPlanAlignment, layout->reference_count, array, false, soa);
}

uint32_t nrq_alloc_plan_flags(const NrqAllocPlan* plan) {
  if (!plan) return NRQ_ALLOC_PLAN_REJECTED;
  return (plan->flags & ~NRQ_ALLOC_PLAN_REPR_MASK) |
         NRQ_ALLOC_PLAN_REPR_BITS(plan->representation);
}

}  // extern "C"
