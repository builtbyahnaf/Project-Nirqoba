#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>
#include <algorithm>

int main(int argc, char** argv) {
    size_t n = 20000000;
    size_t queries = 10000000;
    if (argc > 1) {
        n = std::stoull(argv[1]);
        queries = n / 2;
    }

    std::vector<int64_t> arr(n);
    for (size_t i = 0; i < n; i++) {
        arr[i] = (int64_t)i * 2;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    int64_t hit_count = 0;
    int64_t checksum = 0;
    int64_t max_val = (int64_t)n * 2;

    for (size_t q = 0; q < queries; q++) {
        int64_t target = ((int64_t)q * 3) % max_val;
        auto it = std::lower_bound(arr.begin(), arr.end(), target);
        if (it != arr.end() && *it == target) {
            hit_count++;
            checksum += (it - arr.begin());
        } else {
            checksum -= 1;
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "WORKLOAD: ST4-08\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "ARRAY_SIZE: " << n << "\n";
    std::cout << "QUERIES: " << queries << "\n";
    std::cout << "HITS: " << hit_count << "\n";
    std::cout << "TIME_MS: " << time_ms << "\n";
    std::cout << "CHECKSUM: " << checksum << "\n";
    std::cout << "STATUS: PASS\n";
    return 0;
}
