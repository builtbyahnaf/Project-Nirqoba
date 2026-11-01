// Selectable explicit allocators used by `memory.allocate<T>`.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NrqAllocatorKind {
  NRQ_ALLOCATOR_GENERAL = 0,
  NRQ_ALLOCATOR_POOL = 1,
  NRQ_ALLOCATOR_ARENA = 2,
  NRQ_ALLOCATOR_STACK = 3,
  NRQ_ALLOCATOR_SLAB = 4,
  NRQ_ALLOCATOR_BUDDY = 5,
  NRQ_ALLOCATOR_FREE_LIST = 6,
  NRQ_ALLOCATOR_TLSF = 7,
  NRQ_ALLOCATOR_CUSTOM = 8
} NrqAllocatorKind;

typedef void* (*NrqCustomAllocateFn)(void* context, size_t bytes,
                                     size_t alignment);
typedef void (*NrqCustomDeallocateFn)(void* context, void* pointer,
                                      size_t bytes, size_t alignment);
typedef void (*NrqCustomAllocatorDestroyFn)(void* context);

// The runtime serializes callbacks under its allocator registry lock. Callbacks
// must not re-enter allocator APIs and must pair each pointer with its original
// size and alignment. The runtime copies this table at registration time.
typedef struct NrqCustomAllocatorVTable {
  void* context;
  NrqCustomAllocateFn allocate;
  NrqCustomDeallocateFn deallocate;
  NrqCustomAllocatorDestroyFn destroy;
} NrqCustomAllocatorVTable;

// Report produced by `memory.profiler.capture()`.
//
// The first nine fields describe counters accumulated since
// `nrq_memory_profiler_start`. The remaining fields answer the Phase 6.9
// profiling requirements: region occupancy (free bytes, largest single request
// a region can still serve, and the fragmentation estimate derived from the
// difference), the allocation-size distribution, and the bytes currently held
// by each allocator kind.
//
// Region occupancy is sampled at capture time and can only describe allocators
// whose free space is enumerable (pool, slab, arena, stack, buddy, free-list
// and TLSF). The general-purpose and custom allocators report zero because
// inspecting their backing heaps would require non-portable heap walking; zero
// therefore means "not measurable", not "no free space".
//
// `fragmentation_bytes` is an estimate: free bytes that cannot serve the
// largest single request. It is not a percentage and it ignores virtual
// address-space fragmentation outside the runtime's regions.
typedef struct NrqMemoryProfile {
  uint64_t current_bytes;
  uint64_t total_bytes;
  uint64_t peak_bytes;
  uint64_t allocations;
  uint64_t deallocations;
  uint64_t gc_allocated_bytes;
  uint64_t gc_collected_bytes;
  uint64_t gc_collections;
  uint64_t gc_live_objects;
  uint64_t free_bytes;
  uint64_t fragmentation_bytes;
  uint64_t largest_free_block;
  uint64_t largest_allocation;
  uint64_t allocations_le_64;
  uint64_t allocations_le_256;
  uint64_t allocations_le_1024;
  uint64_t allocations_le_4096;
  uint64_t allocations_le_65536;
  uint64_t allocations_gt_65536;
  uint64_t general_bytes;
  uint64_t pool_bytes;
  uint64_t arena_bytes;
  uint64_t stack_bytes;
  uint64_t slab_bytes;
  uint64_t buddy_bytes;
  uint64_t free_list_bytes;
  uint64_t tlsf_bytes;
  uint64_t custom_bytes;
} NrqMemoryProfile;

// `capacity` is the fixed region size for arena, stack, buddy, free-list, and
// TLSF allocators; it is the largest object size for pool and slab allocators.
void* nrq_allocator_create(NrqAllocatorKind kind, size_t capacity);
void* nrq_allocator_create_custom(const NrqCustomAllocatorVTable* callbacks);
int32_t nrq_allocator_destroy(void* allocator);
int32_t nrq_allocator_reset(void* allocator);
int32_t nrq_memory_allocator_kind(uint64_t allocator_handle);
void* nrq_memory_get_allocator(void);
void* nrq_memory_use_allocator(void* allocator);
void* nrq_memory_allocate(size_t bytes, size_t alignment);
void* nrq_memory_allocate_array(size_t count, size_t element_size,
                                size_t alignment);
void nrq_memory_array_bounds_check(uint64_t index, uint64_t length);
void nrq_memory_array_range_check(uint64_t start, uint64_t count, uint64_t length);
void* nrq_memory_shared_allocate(size_t bytes, size_t alignment);
void* nrq_memory_shared_retain(void* pointer);
int32_t nrq_memory_shared_release(void* pointer);
uint64_t nrq_memory_shared_count(void* pointer);
void* nrq_memory_allocate_from(void* allocator, size_t bytes, size_t alignment);
void nrq_memory_deallocate_checked(void* pointer);
// Resizes an explicit allocation, preserving the allocator identity and the
// alignment recorded at allocation time. Contents survive up to the smaller of
// the old and new sizes. A null result means the request failed and the
// original allocation is still valid and owned by the caller; `bytes == 0`
// releases the allocation and also returns null. A null pointer behaves like a
// fresh allocation from the active allocator. The runtime stops with a
// diagnostic when the pointer is not a live explicit allocation.
void* nrq_memory_reallocate(void* pointer, size_t bytes);
// Process-wide default for explicit allocations made without a per-allocation
// handle. Thread-local selection still takes precedence, and a handle of zero
// restores the general-purpose default. Both return the previous handle.
void* nrq_memory_set_default_allocator(void* allocator);
void* nrq_memory_get_default_allocator(void);
void nrq_memory_profiler_start(void);
void nrq_memory_profiler_stop(void);
NrqMemoryProfile nrq_memory_profiler_capture(void);
void nrq_memory_profiler_capture_into(NrqMemoryProfile* output);
uint64_t nrq_memory_profiler_get_current_bytes(void);
uint64_t nrq_memory_profiler_get_total_bytes(void);
uint64_t nrq_memory_profiler_get_peak_bytes(void);
uint64_t nrq_memory_profiler_get_allocations(void);
uint64_t nrq_memory_profiler_get_deallocations(void);
uint64_t nrq_memory_profiler_get_gc_allocated_bytes(void);
uint64_t nrq_memory_profiler_get_gc_collected_bytes(void);
uint64_t nrq_memory_profiler_get_gc_collections(void);
uint64_t nrq_memory_profiler_get_gc_live_objects(void);
uint64_t nrq_memory_allocator_free_bytes(uint64_t allocator_handle);
uint64_t nrq_memory_allocator_fragmentation_bytes(uint64_t allocator_handle);
void* nrq_memory_secure_allocate(size_t bytes, size_t alignment);
void nrq_memory_secure_zero(void* pointer, size_t bytes);
void nrq_memory_secure_release(void* pointer);
uint64_t nrq_memory_available_bytes(void);
void* nrq_memory_map_file(const char* path, int32_t writable);
int32_t nrq_memory_unmap_file(void* address);
uint64_t nrq_memory_mapping_size(void* address);
int32_t nrq_memory_mapping_read(void* address, size_t offset, size_t bytes,
                                void* output);
int32_t nrq_memory_mapping_write(void* address, size_t offset, size_t bytes,
                                 const void* input);

#ifdef __cplusplus
}
#endif
