#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>
#include <algorithm>

int main(int argc, char** argv) {
    size_t n = 5000000;
    if (argc > 1) {
        n = std::stoull(argv[1]);
    }

    std::vector<int32_t> arr(n);
    uint32_t state = 0x12345678U;
    for (size_t i = 0; i < n; i++) {
        state = (state * 1664525U + 1013904223U);
        arr[i] = (int32_t)state;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    std::sort(arr.begin(), arr.end());
    auto t1 = std::chrono::high_resolution_clock::now();
    double sort_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    bool is_sorted = std::is_sorted(arr.begin(), arr.begin() + (n < 10000 ? n : 10000));
    uint64_t checksum = 0;
    for (size_t i = 0; i < n; i += 1000) {
        checksum += (uint64_t)arr[i];
    }

    std::cout << "WORKLOAD: ST4-06\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "ELEMENTS: " << n << "\n";
    std::cout << "TIME_MS: " << sort_ms << "\n";
    std::cout << "CHECKSUM: " << checksum << "\n";
    std::cout << "STATUS: " << (is_sorted ? "PASS" : "FAIL") << "\n";
    return 0;
}
