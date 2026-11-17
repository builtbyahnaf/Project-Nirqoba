#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>

int main(int argc, char** argv) {
    int size = 1024;
    if (argc > 1) {
        size = std::stoi(argv[1]);
    }

    size_t total_elements = (size_t)size * (size_t)size;
    std::vector<double> a(total_elements);
    std::vector<double> b(total_elements);
    std::vector<double> c(total_elements, 0.0);

    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            a[i * size + j] = (double)((i + j) % 100) / 100.0;
            b[i * size + j] = (double)((i * j) % 100) / 100.0;
        }
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < size; i++) {
        int i_off = i * size;
        for (int k = 0; k < size; k++) {
            double a_ik = a[i_off + k];
            int k_off = k * size;
            for (int j = 0; j < size; j++) {
                c[i_off + j] += a_ik * b[k_off + j];
            }
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    double checksum = 0.0;
    for (size_t i = 0; i < total_elements; i++) {
        checksum += c[i];
    }

    std::cout << "WORKLOAD: ST4-04\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "MATRIX_DIM: " << size << "x" << size << "\n";
    std::cout << "TIME_MS: " << time_ms << "\n";
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "CHECKSUM: " << checksum << "\n";
    std::cout << "STATUS: PASS\n";
    return 0;
}
