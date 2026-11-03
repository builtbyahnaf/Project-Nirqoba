// Monotonic high-resolution wall clock for the C++ benchmark track.
#ifndef NIRQOBA_BENCH_STRESS_TEST_2_CPP_BENCH_CLOCK_HPP
#define NIRQOBA_BENCH_STRESS_TEST_2_CPP_BENCH_CLOCK_HPP

#include <chrono>

static inline double bench_now_ms() {
  using Clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(
             Clock::now().time_since_epoch())
      .count();
}

#endif  // NIRQOBA_BENCH_STRESS_TEST_2_CPP_BENCH_CLOCK_HPP
