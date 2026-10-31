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

// `capacity` is the fixed region size for arena, stack, buddy, free-list, and
// TLSF allocators; it is the largest object size for pool and slab allocators.
void* nrq_allocator_create(NrqAllocatorKind kind, size_t capacity);
int32_t nrq_allocator_destroy(void* allocator);
int32_t nrq_allocator_reset(void* allocator);
void* nrq_memory_get_allocator(void);
void* nrq_memory_use_allocator(void* allocator);

#ifdef __cplusplus
}
#endif
