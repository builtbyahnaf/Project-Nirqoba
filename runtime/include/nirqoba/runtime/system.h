// runtime/include/nirqoba/runtime/system.h
// C ABI System, Time, GC, and Memory Allocation functions for Nirqoba Phases 6.5-6.7.
#pragma once

#include <stdint.h>
#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/gc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns milliseconds since Unix epoch (January 1, 1970).
int64_t nrq_time_now_ms(void);

// Exits the process with the given exit code.
void nrq_system_exit(int32_t exit_code);

// Memory allocation (Phase 6.7).
void* nrq_memory_allocate(size_t bytes, size_t alignment);
void nrq_memory_deallocate(void* ptr, size_t bytes, size_t alignment);
void nrq_memory_deallocate_checked(void* ptr);

#ifdef __cplusplus
}
#endif
