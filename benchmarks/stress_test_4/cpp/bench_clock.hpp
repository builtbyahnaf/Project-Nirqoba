#ifndef BENCH_CLOCK_HPP
#define BENCH_CLOCK_HPP

#include <chrono>

static inline double bench_now_ms() {
    using clock = std::chrono::high_resolution_clock;
    static auto start = clock::now();
    auto now = clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
}

#endif // BENCH_CLOCK_HPP
