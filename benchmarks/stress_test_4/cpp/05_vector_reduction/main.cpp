#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>
#include <numeric>

int main(int argc, char** argv) {
    size_t n = 50000000;
    if (argc > 1) {
        n = std::stoull(argv[1]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<uint64_t> vec(n);
    for (size_t i = 0; i < n; i++) {
        vec[i] = (uint64_t)i ^ 0x55555555ULL;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double construct_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    auto t2 = std::chrono::high_resolution_clock::now();
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) {
        total += vec[i];
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double reduce_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    std::cout << "WORKLOAD: ST4-05\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "ELEMENTS: " << n << "\n";
    std::cout << "CONSTRUCTION_MS: " << construct_ms << "\n";
    std::cout << "REDUCTION_MS: " << reduce_ms << "\n";
    std::cout << "TIME_MS: " << (construct_ms + reduce_ms) << "\n";
    std::cout << "CHECKSUM: " << total << "\n";
    std::cout << "STATUS: PASS\n";
    return 0;
}
