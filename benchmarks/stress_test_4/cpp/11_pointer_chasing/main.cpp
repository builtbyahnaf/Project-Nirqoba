#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>

int main(int argc, char** argv) {
    size_t n = 10000000;
    if (argc > 1) {
        n = std::stoull(argv[1]);
    }

    std::vector<size_t> next_idx(n);
    for (size_t i = 0; i < n; i++) {
        next_idx[i] = i;
    }

    uint32_t state = 0xDEADBEEFU;
    for (size_t i = n - 1; i > 0; i--) {
        state = (state * 1664525U + 1013904223U);
        size_t j = state % (i + 1);
        std::swap(next_idx[i], next_idx[j]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    size_t curr = 0;
    uint64_t checksum = 0;
    for (size_t i = 0; i < n; i++) {
        checksum += curr;
        curr = next_idx[curr];
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "WORKLOAD: ST4-11\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "NODES: " << n << "\n";
    std::cout << "TIME_MS: " << time_ms << "\n";
    std::cout << "CHECKSUM: " << checksum << "\n";
    std::cout << "STATUS: PASS\n";
    return 0;
}
