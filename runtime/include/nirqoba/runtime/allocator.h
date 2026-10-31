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
  NRQ_ALLOCATOR_TLSF = 7
} NrqAllocatorKind;

typedef struct NrqMemoryProfile {
  uint64_t current_bytes;
  uint64_t peak_bytes;
  uint64_t allocations;
  uint64_t deallocations;
} NrqMemoryProfile;

// `capacity` is the fixed region size for arena, stack, buddy, free-list, and
// TLSF allocators; it is the largest object size for pool and slab allocators.
void* nrq_allocator_create(NrqAllocatorKind kind, size_t capacity);
int32_t nrq_allocator_destroy(void* allocator);
int32_t nrq_allocator_reset(void* allocator);
void* nrq_memory_get_allocator(void);
void* nrq_memory_use_allocator(void* allocator);
void* nrq_memory_allocate(size_t bytes, size_t alignment);
void nrq_memory_deallocate_checked(void* pointer);
void nrq_memory_profiler_start(void);
void nrq_memory_profiler_stop(void);
NrqMemoryProfile nrq_memory_profiler_capture(void);
uint64_t nrq_memory_profiler_get_current_bytes(void);
uint64_t nrq_memory_profiler_get_peak_bytes(void);
uint64_t nrq_memory_profiler_get_allocations(void);
uint64_t nrq_memory_profiler_get_deallocations(void);
void* nrq_memory_secure_allocate(size_t bytes, size_t alignment);
void nrq_memory_secure_zero(void* pointer, size_t bytes);
void nrq_memory_secure_release(void* pointer);
uint64_t nrq_memory_available_bytes(void);
void* nrq_memory_map_file(const char* path, int32_t writable);
int32_t nrq_memory_unmap_file(void* address);
uint64_t nrq_memory_mapping_size(void* address);

#ifdef __cplusplus
}
#endif
