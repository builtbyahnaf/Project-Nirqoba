// gc_debug_internal.h — how the collector reaches the observation layer.
//
// The collector itself contains no trace code in a production build. Each
// emission point calls one of the helpers below, and because they are
// `if constexpr` on the build configuration, a production compile removes the
// call and its arguments entirely: no branch, no atomic load, no symbol
// reference. There is nothing to strip after the fact and no way for the two
// paths to drift, because the emission point is the same statement in both.
//
// In a development build the same helper costs one relaxed load of a
// read-mostly word plus a branch. Unless a consumer has selected the kind, the
// collector does not touch the collector lock, allocate, or call into the ring.

#pragma once

#include <atomic>
#include <cstdint>

#include <nirqoba/runtime/gc_debug.h>

#include "runtime_config.h"

namespace nirq::gc::detail {

// Process-wide mask of NRQ_GC_EVENT_* kinds a consumer asked for. Zero (the
// default) means "record nothing", which is the state an ordinary development
// run stays in unless a tool opts in.
inline std::atomic<uint32_t>& debug_event_mask() {
  static std::atomic<uint32_t> mask{0};
  return mask;
}

// Appends one record to the calling thread's ring. Defined in gc_debug.cpp.
void debug_record_event(uint32_t kind, uint32_t aux, uint64_t value,
                        const void* address);

// Appends one allocation tag to the calling thread's tag ring.
void debug_record_tag(uint64_t site, const void* object);

// True when the kind is selected. Always false in a production build.
inline bool debug_wants(uint32_t kind) {
  if constexpr (!nirq::runtime_debug_enabled) {
    return false;
  } else {
    return (debug_event_mask().load(std::memory_order_relaxed) & kind) != 0;
  }
}

inline void debug_emit(uint32_t kind, uint32_t aux, uint64_t value,
                       const void* address) {
  if constexpr (nirq::runtime_debug_enabled) {
    if (debug_wants(kind)) debug_record_event(kind, aux, value, address);
  }
}

inline void debug_tag(uint64_t site, const void* object) {
  if constexpr (nirq::runtime_debug_enabled) {
    debug_record_tag(site, object);
  }
}

}  // namespace nirq::gc::detail
