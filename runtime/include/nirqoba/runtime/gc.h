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

// Root slots are addresses containing managed object pointers. Register the
// slot before a collection can run and unregister it before its storage ends.
// Root slot updates shared with concurrent collection must use root_store.
void nrq_gc_root_register(void** slot);
void nrq_gc_root_register_typed(void* slot, size_t object_size,
                                const size_t* reference_offsets,
                                size_t reference_count);
void nrq_gc_root_unregister(void** slot);
void nrq_gc_root_store(void** slot, void* value);
void nrq_gc_root_copy(void* destination, const void* source, size_t bytes);
uint64_t nrq_gc_root_frame_begin(void);
void nrq_gc_root_frame_end(uint64_t marker);

void nrq_gc_enable(void);
void nrq_gc_disable(void);
void nrq_gc_collect(void);
void nrq_gc_set_threshold(size_t bytes);
struct nrq_gc_stats nrq_gc_get_stats(void);
uint64_t nrq_gc_get_allocated_bytes(void);
uint64_t nrq_gc_get_collected_bytes(void);
uint64_t nrq_gc_get_collection_count(void);
uint64_t nrq_gc_get_live_objects(void);
void* nrq_gc_pin(void* object);
void nrq_gc_unpin(void* object);

#ifdef __cplusplus
}
#endif
