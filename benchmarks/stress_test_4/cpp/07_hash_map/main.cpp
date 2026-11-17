#include <iostream>
#include <unordered_map>
#include <cstdint>
#include <chrono>

int main(int argc, char** argv) {
    size_t n = 5000000;
    size_t removals = 1000000;
    if (argc > 1) {
        n = std::stoull(argv[1]);
        removals = n / 5;
    }

    std::unordered_map<uint64_t, uint64_t> map;
    map.reserve(n);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < n; i++) {
        uint64_t key = (uint64_t)i * 2654435761ULL;
        map[key] = (uint64_t)i;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double insert_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    auto t2 = std::chrono::high_resolution_clock::now();
    uint64_t lookup_sum = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t key = (uint64_t)i * 2654435761ULL;
        lookup_sum += map[key];
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double lookup_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    auto t4 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < removals * 5; i += 5) {
        uint64_t key = (uint64_t)i * 2654435761ULL;
        map.erase(key);
    }
    auto t5 = std::chrono::high_resolution_clock::now();
    double remove_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();

    double total_ms = insert_ms + lookup_ms + remove_ms;
    std::cout << "WORKLOAD: ST4-07\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "INSERT_MS: " << insert_ms << "\n";
    std::cout << "LOOKUP_MS: " << lookup_ms << "\n";
    std::cout << "REMOVE_MS: " << remove_ms << "\n";
    std::cout << "TIME_MS: " << total_ms << "\n";
    std::cout << "REMAINING: " << map.size() << "\n";
    std::cout << "CHECKSUM: " << lookup_sum << "\n";
    std::cout << "STATUS: " << (map.size() == (n - removals) ? "PASS" : "FAIL") << "\n";
    return 0;
}
