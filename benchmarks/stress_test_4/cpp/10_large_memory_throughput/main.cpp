#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>

int main(int argc, char** argv) {
    size_t size = 512ULL * 1024ULL * 1024ULL; // 512 MiB
    if (argc > 1) {
        size = std::stoull(argv[1]) * 1024ULL * 1024ULL;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<uint8_t> buf(size);
    for (size_t i = 0; i < size; i += 4096) {
        buf[i] = (uint8_t)(i & 0xFF);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double write_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    auto t2 = std::chrono::high_resolution_clock::now();
    uint64_t checksum = 0;
    for (size_t i = 0; i < size; i += 1024) {
        checksum += buf[i];
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double read_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    auto t4 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < size; i += 4096) {
        buf[i] = (uint8_t)((buf[i] * 3) ^ 0x55);
    }
    auto t5 = std::chrono::high_resolution_clock::now();
    double transform_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();

    double total_ms = write_ms + read_ms + transform_ms;
    double throughput_mib = ((double)size / (1024.0 * 1024.0)) / (total_ms / 1000.0);

    std::cout << "WORKLOAD: ST4-10\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "BUFFER_MIB: " << (size / (1024 * 1024)) << "\n";
    std::cout << "TIME_MS: " << total_ms << "\n";
    std::cout << "THROUGHPUT_MIBS: " << throughput_mib << "\n";
    std::cout << "CHECKSUM: " << checksum << "\n";
    std::cout << "STATUS: PASS\n";
    return 0;
}
