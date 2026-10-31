// runtime/src/system_windows.cpp
// Windows x64 implementation of System, Time, GC, and Memory Allocation functions for Nirqoba Phases 6.5-6.7 using Win32 API.

#ifndef _WIN32
#  error "system_windows.cpp is the Windows-only runtime system implementation."
#endif

#include "nirqoba/runtime/system.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Simple bump-pointer garbage collector state.
struct nrq_gc_state {
  bool enabled;
  uint64_t allocated_bytes;
  uint64_t collected_bytes;
  uint64_t collections;
};

static struct nrq_gc_state gc_state;

// Heap allocation tracking for GC.
static uint64_t nrq_gc_tracked_bytes = 0;

// Phase 6.7: Simple default allocator using process heap.

extern "C" {

int64_t nrq_time_now_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    // Combine high and low parts into a 64-bit integer (100-ns intervals since Jan 1, 1601).
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    // Difference between Windows epoch (1601) and Unix epoch (1970) in 100-nanosecond units:
    // 116444736000000000ULL
    const unsigned __int64 kEpochDiff = 116444736000000000ULL;
    if (uli.QuadPart < kEpochDiff) return 0;

    unsigned __int64 unix_100ns = uli.QuadPart - kEpochDiff;
    // 10,000 intervals of 100ns = 1 millisecond.
    return static_cast<int64_t>(unix_100ns / 10000ULL);
}

void nrq_system_exit(int32_t exit_code) {
    ExitProcess(static_cast<UINT>(exit_code));
}

void nrq_gc_enable(void) {
  gc_state.enabled = true;
}

void nrq_gc_disable(void) {
  gc_state.enabled = false;
}

void nrq_gc_collect(void) {
  if (!gc_state.enabled) return;
  gc_state.collections++;
  // In Phase 6.5 initial implementation, GC is a no-op collect request.
  // The full tracing collector will be implemented in later phases.
  // Track bytes allocated for GC to report statistics.
  gc_state.collected_bytes += nrq_gc_tracked_bytes;
  nrq_gc_tracked_bytes = 0;
}

struct nrq_gc_stats nrq_gc_get_stats(void) {
  struct nrq_gc_stats stats;
  stats.allocated_bytes = nrq_gc_tracked_bytes;
  stats.collected_bytes = gc_state.collected_bytes;
  stats.collections = gc_state.collections;
  return stats;
}

// Phase 6.7: Simple default allocator using process heap.
void* nrq_memory_allocate(size_t bytes, size_t alignment) {
  // Align up to the requested alignment.
  size_t alignment_mask = alignment - 1;
  size_t aligned_bytes = (bytes + alignment_mask) & ~alignment_mask;
  return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, aligned_bytes);
}

void nrq_memory_deallocate(void* ptr, size_t bytes, size_t alignment) {
  // For the simple default allocator, just free to the process heap.
  // Alignment parameters are accepted for interface compatibility.
  if (ptr) {
    HeapFree(GetProcessHeap(), 0, ptr);
  }
}

}  // extern "C"
