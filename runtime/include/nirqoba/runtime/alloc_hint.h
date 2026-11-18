// Compiler-facing allocation hints and the plan they produce.
//
// A hint states what the compiler knows about an allocation site: the payload
// size, how many times it repeats, how many managed references it contains, and
// which lifetime the site has. `nrq_alloc_plan` turns that into the decision the
// allocator would otherwise have to rediscover on every allocation: which
// representation (H0, H8 or H16), how many header bytes, the total payload, and
// the aligned slot size.
//
// The plan is advisory. Every runtime entry point re-checks the facts it depends
// on and keeps the final say, because a compiler guarantee that does not hold is
// exactly the case where choosing a representation on trust would corrupt a
// heap.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nirqoba/runtime/gc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Which representation the plan selects. The numeric values are independent of
// the runtime's enum on purpose: this header is compiled into generated programs
// and must not need the collector's internal constants.
typedef enum NrqAllocPlanRepresentation {
  NRQ_ALLOC_REPR_NONE = 0,
  NRQ_ALLOC_REPR_HEADERLESS = 1,
  NRQ_ALLOC_REPR_COMPACT = 2,
  NRQ_ALLOC_REPR_EXTENDED = 3
} NrqAllocPlanRepresentation;

#define NRQ_ALLOC_PLAN_ARRAY (1u << 0)
#define NRQ_ALLOC_PLAN_REPEATED (1u << 1)
#define NRQ_ALLOC_PLAN_LARGE (1u << 2)
#define NRQ_ALLOC_PLAN_HAS_REFERENCES (1u << 3)
#define NRQ_ALLOC_PLAN_REJECTED (1u << 4)
#define NRQ_ALLOC_PLAN_DETERMINISTIC (1u << 5)
// The representation occupies two bits at a fixed shift, so a compiler can emit
// the whole plan as one 32-bit immediate at the allocation site.
#define NRQ_ALLOC_PLAN_REPR_SHIFT 8
#define NRQ_ALLOC_PLAN_REPR_MASK (3u << NRQ_ALLOC_PLAN_REPR_SHIFT)
#define NRQ_ALLOC_PLAN_REPR_BITS(representation) \
  ((uint32_t)(representation) << NRQ_ALLOC_PLAN_REPR_SHIFT)
#define NRQ_ALLOC_PLAN_REPR_OF(flags) \
  (((flags) & NRQ_ALLOC_PLAN_REPR_MASK) >> NRQ_ALLOC_PLAN_REPR_SHIFT)

// What the compiler knows about one allocation site.
typedef struct NrqAllocHint {
  // Bytes of one record, or of one array element when the array flag is set.
  uint64_t payload_bytes;
  // Instances of the element; a record is one.
  uint64_t element_count;
  // Required alignment; zero means the natural alignment.
  uint64_t alignment;
  uint32_t reference_count;
  uint32_t flags;
} NrqAllocHint;

#define NRQ_ALLOC_HINT_ARRAY (1u << 0)
#define NRQ_ALLOC_HINT_MANAGED (1u << 1)
#define NRQ_ALLOC_HINT_DETERMINISTIC (1u << 2)
#define NRQ_ALLOC_HINT_ARENA (1u << 3)
#define NRQ_ALLOC_HINT_POOL (1u << 4)
// The payload owns resources that need a destructor; an arena or pool site that
// sets this must register one.
#define NRQ_ALLOC_HINT_OWNS_RESOURCES (1u << 5)

typedef struct NrqAllocPlan {
  uint32_t representation;  // NrqAllocPlanRepresentation
  uint32_t flags;           // NRQ_ALLOC_PLAN_*
  uint64_t header_bytes;
  uint64_t payload_bytes;   // bytes of one object's payload
  uint64_t total_bytes;     // payload_bytes * element_count
  uint64_t slot_bytes;      // payload rounded to the representation's alignment
  uint64_t alignment;
} NrqAllocPlan;

// The plan for a hint. Never allocates and never takes a lock; an impossible
// hint (zero payload, a non-power-of-two alignment) produces a plan with
// NRQ_ALLOC_PLAN_REJECTED set and NRQ_ALLOC_REPR_NONE.
NrqAllocPlan nrq_alloc_plan(const NrqAllocHint* hint);
// The plan for an existing descriptor, which is what the runtime's own layout
// interning produces. `element_count` is the instance count for an array and one
// for a record.
NrqAllocPlan nrq_alloc_plan_for_layout(const struct nrq_object_layout* layout,
                                       uint64_t element_count);
// Packs a plan into one 32-bit word a compiler can emit as a constant.
uint32_t nrq_alloc_plan_flags(const NrqAllocPlan* plan);

#ifdef __cplusplus
}
#endif
