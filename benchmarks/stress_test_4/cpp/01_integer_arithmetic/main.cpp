#include <iostream>
#include <cstdint>
#include <chrono>

int main(int argc, char** argv) {
    uint64_t total_ops = 500000000ULL;
    if (argc > 1) {
        total_ops = std::stoull(argv[1]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    uint64_t total = 0;

    for (uint64_t i = 1; i <= total_ops; i++) {
        total = (total + i);
        total = (total - (i & 0xFF));
        total = (total * 3ULL);
        uint64_t div = (i % 7ULL) + 1ULL;
        total = (total ^ (total / div));
        total = (total ^ (total << 5) ^ (total >> 59));
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "WORKLOAD: ST4-01\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "OPS: " << total_ops << "\n";
    std::cout << "TIME_MS: " << time_ms << "\n";
    std::cout << "CHECKSUM: " << total << "\n";
    std::cout << "STATUS: PASS\n";
    return 0;
}
