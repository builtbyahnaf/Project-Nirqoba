// runtime/include/nirqoba/runtime/system.h
// C ABI System, Time, GC, and Memory Allocation functions for Nirqoba Phases 6.5-6.7.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns milliseconds since Unix epoch (January 1, 1970).
int64_t nrq_time_now_ms(void);

// Exits the process with the given exit code.
void nrq_system_exit(int32_t exit_code);

// Garbage collection control.
void nrq_gc_enable(void);
void nrq_gc_disable(void);
void nrq_gc_collect(void);

// Get GC statistics.
struct nrq_gc_stats {
  uint64_t allocated_bytes;
  uint64_t collected_bytes;
  uint64_t collections;
};
struct nrq_gc_stats nrq_gc_get_stats(void);

// Memory allocation (Phase 6.7).
void* nrq_memory_allocate(size_t bytes, size_t alignment);
void nrq_memory_deallocate(void* ptr, size_t bytes, size_t alignment);

#ifdef __cplusplus
}
#endif
