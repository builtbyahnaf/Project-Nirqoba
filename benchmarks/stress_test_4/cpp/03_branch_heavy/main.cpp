#include <iostream>
#include <cstdint>
#include <chrono>

int main(int argc, char** argv) {
    long long n = 100000000LL;
    if (argc > 1) {
        n = std::stoll(argv[1]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    uint32_t state = 123456789U;
    uint64_t checksum = 0;

    for (long long i = 0; i < n; i++) {
        state = (state * 1664525U + 1013904223U);
        if ((i & 1) == 0) {
            if ((state & 1) != 0) {
                checksum += (uint64_t)(state ^ 0x55555555U);
            } else {
                if (((state >> 16) & 1) != 0) {
                    checksum += (uint64_t)(state ^ 0xAAAAAAAAU);
                } else {
                    checksum += (uint64_t)(state * 3U);
                }
            }
        } else {
            if (state < 0x80000000U) {
                checksum ^= (uint64_t)(state >> 3);
            } else {
                checksum -= (uint64_t)(state & 0xFFFFU);
            }
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "WORKLOAD: ST4-03\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "INPUTS: " << n << "\n";
    std::cout << "TIME_MS: " << time_ms << "\n";
    std::cout << "CHECKSUM: " << checksum << "\n";
    std::cout << "STATUS: PASS\n";
    return 0;
}
