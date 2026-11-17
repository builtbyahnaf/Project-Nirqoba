#include <iostream>
#include <chrono>
#include <cmath>
#include <iomanip>

int main(int argc, char** argv) {
    long long total_ops = 100000000LL;
    if (argc > 1) {
        total_ops = std::stoll(argv[1]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    double acc = 1.0;

    for (long long i = 1; i <= total_ops; i++) {
        acc = acc + 0.001;
        acc = acc * 1.000001;
        acc = acc / 1.0000005;
        acc = acc + std::sqrt(acc) * 0.00001;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "WORKLOAD: ST4-02\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "OPS: " << total_ops << "\n";
    std::cout << "TIME_MS: " << time_ms << "\n";
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "CHECKSUM: " << acc << "\n";
    std::cout << "STATUS: PASS\n";
    return 0;
}
