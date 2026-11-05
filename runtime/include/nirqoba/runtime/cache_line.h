// cache_line.h — cache-line granularity for state shared between threads.
//
// Two independent variables on one cache line are false sharing: a thread that
// writes one invalidates the other thread's copy of the whole line, so the
// hardware ping-pongs a line nobody actually shares. Keeping unrelated hot
// state on separate lines is a pure layout change with no semantic effect.

#pragma once

#include <cstddef>

namespace nirq {

// x86-64 lines are 64 bytes; the value is also a safe upper bound for the
// neighbouring x86 core families this runtime targets.
inline constexpr std::size_t nrq_cache_line_bytes = 64;

// Pads a value up to a whole cache line so it cannot share one with anything
// declared next to it.
template <typename T>
struct alignas(nrq_cache_line_bytes) NrqCacheLineAligned {
  T value{};

  T& get() { return value; }
  const T& get() const { return value; }
};

}  // namespace nirq
