// Dynamic array backing storage.
//
// Only the buffer is managed here; the language value carries length and
// capacity. Allocation routes through a dedicated deterministic allocator
// instance (never the GC, never the default allocator's small-allocation cache),
// because an array's lifetime is lexical and its teardown is a scope exit.

#include "nirqoba/runtime/array.h"

#include <atomic>
#include <limits>
#include <mutex>

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/fatal.h"

namespace {

std::atomic<int32_t> live_array_buffers{0};
std::mutex array_allocator_mutex;
void* array_allocator = nullptr;

// Created on first use rather than during static initialization: the allocator
// runtime has its own initialization, and a namespace-scope handle would depend
// on translation-unit ordering.
void* array_allocator_handle() {
    std::lock_guard<std::mutex> lock(array_allocator_mutex);
    if (array_allocator == nullptr) {
        array_allocator = nrq_allocator_create(NRQ_ALLOCATOR_GENERAL, 0);
    }
    return array_allocator;
}

}  // namespace

void* nrq_array_create(size_t capacity, size_t element_size, size_t align) {
    if (capacity == 0 || element_size == 0) return nullptr;
    // A capacity that cannot be addressed is a caller bug, not something to
    // allocate and hope about: multiplying first would wrap and hand back a
    // buffer smaller than the caller believes it owns.
    if (capacity > std::numeric_limits<size_t>::max() / element_size) {
        return nullptr;
    }
    void* allocator = array_allocator_handle();
    if (allocator == nullptr) return nullptr;
    const size_t alignment = align == 0 ? element_size : align;
    void* buffer =
        nrq_memory_allocate_array_from(allocator, capacity, element_size, alignment);
    if (!buffer) return nullptr;
    live_array_buffers.fetch_add(1, std::memory_order_relaxed);
    return buffer;
}

void* nrq_array_reallocate(void* old_ptr, size_t old_capacity,
                           size_t new_capacity, size_t element_size,
                           size_t align) {
    // A zero element size means the caller has no layout to grow into; the
    // compiler rejects such an element type long before this point.
    if (element_size == 0)
        nrq_runtime_fatal("dynamic array growth needs a fixed element size");
    if (new_capacity <= old_capacity) return old_ptr;
    if (new_capacity > std::numeric_limits<size_t>::max() / element_size)
        nrq_runtime_fatal("dynamic array capacity exceeds the address space");

    if (old_ptr == nullptr) {
        // First growth: the empty representation carries no allocator domain, so
        // the array allocator is the one to start in.
        const size_t alignment = align == 0 ? element_size : align;
        void* allocator = array_allocator_handle();
        void* buffer = nrq_memory_allocate_array_from(allocator, new_capacity,
                                                     element_size, alignment);
        if (!buffer) nrq_runtime_fatal("dynamic array allocation failed");
        live_array_buffers.fetch_add(1, std::memory_order_relaxed);
        return buffer;
    }

    // The registry knows the owning allocator, the old byte count and the
    // alignment, so the grown block stays in this domain and keeps its
    // alignment. The old block is untouched when the request fails.
    void* grown = nrq_memory_reallocate(old_ptr, new_capacity * element_size);
    if (!grown) nrq_runtime_fatal("dynamic array growth failed");
    return grown;
}

void nrq_array_free(void* buffer) {
    if (!buffer) return;
    // The allocation was registered with the array allocator, so the registry
    // returns it to that domain without the caller naming a size or an owner.
    nrq_memory_deallocate_checked(buffer);
    live_array_buffers.fetch_sub(1, std::memory_order_relaxed);
}

int32_t nrq_array_live_count(void) {
    return static_cast<int32_t>(live_array_buffers.load(std::memory_order_relaxed));
}
