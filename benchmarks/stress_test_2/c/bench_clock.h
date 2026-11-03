// Monotonic high-resolution wall clock for the C benchmark track.
#ifndef NIRQOBA_BENCH_STRESS_TEST_2_C_BENCH_CLOCK_H
#define NIRQOBA_BENCH_STRESS_TEST_2_C_BENCH_CLOCK_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static double bench_now_ms(void) {
  static LARGE_INTEGER frequency;
  static int initialized = 0;
  LARGE_INTEGER counter;
  if (!initialized) {
    QueryPerformanceFrequency(&frequency);
    initialized = 1;
  }
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

#endif  // NIRQOBA_BENCH_STRESS_TEST_2_C_BENCH_CLOCK_H
