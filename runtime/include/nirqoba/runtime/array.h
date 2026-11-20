// Deterministic backing storage for `dynamic_array<T>`.
//
// The runtime owns only the bytes. Length and capacity live in the language
// value ({buffer, length, capacity}), so growth, appends and element teardown are
// compiler decisions and the runtime never inspects an element.
//
// The empty representation is `buffer == nullptr`, which is what an array with
// capacity 0 receives. Every helper here is null-safe, so a declared-but-unused
// array neither allocates nor needs a special case at its destructor.
//
// Array buffers come from a dedicated allocator instance, not the default one:
// the default allocator keeps a small-allocation cache whose identity and
// accounting other code depends on, and a container that allocates and frees
// routinely would disturb it. Isolation also keeps array growth from sharing a
// cache with unrelated program allocations.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Allocates `capacity` slots of `element_size` bytes, aligned to `align` (0
// selects the element size's natural alignment), from the array allocator.
// Returns nullptr for a zero capacity, a zero element size, or a size overflow,
// so no caller can observe a partial allocation.
void* nrq_array_create(size_t capacity, size_t element_size, size_t align);

// Grows `old_ptr` to `new_capacity` slots of `element_size` bytes each and
// returns the buffer to use; the whole old buffer is copied, so the elements
// already stored survive the move. A null `old_ptr` is the empty representation,
// so the first growth is the first allocation; a request that does not extend
// the capacity returns `old_ptr` unchanged.
//
// The allocator domain belongs to the buffer, not to the call, so a growth stays
// in the array allocator and keeps the alignment recorded when the buffer was
// created. A growth that cannot be satisfied is fatal: the language has no error
// channel on this path, and carrying on with the old capacity would silently
// drop the element the caller is about to store.
void* nrq_array_reallocate(void* old_ptr, size_t old_capacity,
                           size_t new_capacity, size_t element_size, size_t align);

// Releases a buffer from nrq_array_create. Null is a no-op, matching the empty
// representation.
void nrq_array_free(void* buffer);

// Number of live array buffers. Integration tests assert this returns to its
// starting value, so a leak is observable rather than inferred.
int32_t nrq_array_live_count(void);

#ifdef __cplusplus
}
#endif
