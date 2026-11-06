// Precise, non-moving tracing collector interface.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nrq_gc_visit_fn)(void* referenced_object, void* visitor_context);
typedef void (*nrq_gc_trace_fn)(void* object, size_t object_bytes,
                                nrq_gc_visit_fn visit, void* visitor_context);

struct nrq_gc_stats {
  uint64_t allocated_bytes;
  uint64_t collected_bytes;
  uint64_t collections;
  uint64_t live_objects;
};

// Allocates a zero-initialized managed object. The trace callback visits only
// fields which contain managed references; scalar fields must not be visited.
// A null callback describes an object with no managed references.
void* nrq_gc_allocate(size_t bytes, nrq_gc_trace_fn trace);
void* nrq_gc_allocate_array(size_t count, size_t element_size,
                            nrq_gc_trace_fn trace);
// Allocates a contiguous array whose elements contain managed references at
// the supplied byte offsets. The collector copies the offsets into metadata.
void* nrq_gc_allocate_array_typed(size_t count, size_t element_size,
                                  const size_t* reference_offsets,
                                  size_t reference_count);
// Allocates and publishes the result into an already registered pointer root
// in one collector critical section. The root slot must be registered with
// nrq_gc_root_register before calling this function.
void* nrq_gc_allocate_array_typed_rooted(
    void** root_slot, size_t count, size_t element_size,
    const size_t* reference_offsets, size_t reference_count);

// Root slots are addresses containing managed object pointers. Register the
// slot before a collection can run and unregister it before its storage ends.
// Root slot updates shared with incremental/concurrent collection must use
// root_store (or root_copy for aggregates); those functions are write barriers.
// Managed object fields written by native code must use the same barriers.
void nrq_gc_root_register(void** slot);
void nrq_gc_root_register_typed(void* slot, size_t object_size,
                                const size_t* reference_offsets,
                                size_t reference_count);
// Registers an externally owned contiguous array as a root region. Offsets are
// relative to each element and are repeated count times with element_size as
// the stride. The caller must unregister the data address before freeing it.
// `nrq_gc_root_clear_array_references` is for freshly allocated, uninitialized
// storage: it zeros only the described pointer slots before root registration.
void nrq_gc_root_clear_array_references(void* data, size_t count,
                                       size_t element_size,
                                       const size_t* reference_offsets,
                                       size_t reference_count);
void nrq_gc_root_register_array(void* data, size_t count, size_t element_size,
                                const size_t* reference_offsets,
                                size_t reference_count);
// Ordinary root slots must be unregistered by a thread that registered them.
// Repeated array roots are external storage and may be unregistered by owner.
void nrq_gc_root_unregister(void** slot);
void nrq_gc_root_store(void** slot, void* value);
void nrq_gc_root_copy(void* destination, const void* source, size_t bytes);
uint64_t nrq_gc_root_frame_begin(void);
// Root frames are thread-local and must be ended in strict LIFO order.
void nrq_gc_root_frame_end(uint64_t marker);

void nrq_gc_enable(void);
void nrq_gc_disable(void);
// A full collection reclaims both generations. Marking is incremental and
// interleaved with mutator execution; sweeping proceeds slice by slice while
// mutators continue executing ordinary code. Mutators are parked only briefly at
// cycle boundaries to publish thread-local allocations and prepare roots.
void nrq_gc_collect(void);
// A young-generation collection reclaims unreachable young objects and
// promotes survivors after a few cycles. Unreachable old objects are retained
// until a full collection. Root-store/root-copy barriers record old-to-young
// references for the next minor cycle. When generational collection is disabled
// this performs a full collection.
void nrq_gc_collect_minor(void);

// Generational collection is enabled by default. Disabling it makes automatic
// collections full collections; explicit minor collections remain available.
void nrq_gc_set_generational(int32_t enabled);
int32_t nrq_gc_is_generational(void);
uint64_t nrq_gc_get_minor_collection_count(void);
uint64_t nrq_gc_get_major_collection_count(void);

// Generated mutators register each function activation and poll at function
// entry and loop backedges. Native code that accesses managed references over
// long-running work may use the same enter/safepoint/leave protocol.
void nrq_gc_mutator_enter(void);
void nrq_gc_mutator_leave(void);
void nrq_gc_safepoint(void);
void nrq_gc_set_threshold(size_t bytes);
struct nrq_gc_stats nrq_gc_get_stats(void);
// Reports the per-object metadata bytes the collector charges for every managed
// allocation. Exposed so the layout can be audited by tests and tooling.
size_t nrq_gc_object_header_bytes(void);
// Page-pool diagnostics. `nrq_gc_page_count` counts active bump pages,
// `nrq_gc_recycled_page_count` counts empty pages retained for reuse, and
// `nrq_gc_page_reuse_count` counts how often a retained page was handed back
// out instead of allocating fresh storage.
size_t nrq_gc_page_count(void);
size_t nrq_gc_recycled_page_count(void);
uint64_t nrq_gc_page_reuse_count(void);
uint64_t nrq_gc_get_allocated_bytes(void);
uint64_t nrq_gc_get_collected_bytes(void);
uint64_t nrq_gc_get_collection_count(void);
uint64_t nrq_gc_get_live_objects(void);
void* nrq_gc_pin(void* object);
void nrq_gc_unpin(void* object);
// Balanced compiler handoff pin for managed-pointer returns. Unlike the
// diagnostic user pin API, this has no small per-object nesting limit.
void* nrq_gc_pin_transfer(void* object);
// Pinning diagnostics. `nrq_gc_pin_count` reports how many distinct objects are
// pinned right now. A pin request that would exceed the documented per-object
// pin limit is rejected (returns null) and counted by `nrq_gc_pin_violations`,
// which makes excessive pinning observable from the language without failing
// the program.
uint64_t nrq_gc_pin_count(void);
uint64_t nrq_gc_pin_violations(void);
// Reports whether automatic collection is currently enabled. Explicit
// collection stays available in both states.
int32_t nrq_gc_is_enabled(void);
void nrq_gc_ref_count_start(void);
void nrq_gc_ref_count_stop(void);
int64_t nrq_gc_get_ref_count(void* object);

#ifdef __cplusplus
}
#endif
