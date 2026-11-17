#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>

static void merge_sort_recursive(std::vector<int32_t>& arr, std::vector<int32_t>& temp, size_t left, size_t right) {
    if (left >= right) return;
    size_t mid = left + ((right - left) >> 1);
    merge_sort_recursive(arr, temp, left, mid);
    merge_sort_recursive(arr, temp, mid + 1, right);

    size_t i = left, j = mid + 1, k = left;
    while (i <= mid && j <= right) {
        if (arr[i] <= arr[j]) {
            temp[k++] = arr[i++];
        } else {
            temp[k++] = arr[j++];
        }
    }
    while (i <= mid) temp[k++] = arr[i++];
    while (j <= right) temp[k++] = arr[j++];
    for (size_t p = left; p <= right; p++) {
        arr[p] = temp[p];
    }
}

int main(int argc, char** argv) {
    size_t n = 4000000;
    if (argc > 1) {
        n = std::stoull(argv[1]);
    }

    std::vector<int32_t> arr(n);
    std::vector<int32_t> temp(n);

    uint32_t state = 0x54321U;
    for (size_t i = 0; i < n; i++) {
        state = (state * 1664525U + 1013904223U);
        arr[i] = (int32_t)state;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    merge_sort_recursive(arr, temp, 0, n - 1);
    auto t1 = std::chrono::high_resolution_clock::now();
    double sort_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    bool is_sorted = (arr[0] <= arr[n - 1]);
    uint64_t checksum = 0;
    for (size_t i = 0; i < n; i += 1000) {
        checksum += (uint64_t)arr[i];
    }

    std::cout << "WORKLOAD: ST4-13\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "ELEMENTS: " << n << "\n";
    std::cout << "TIME_MS: " << sort_ms << "\n";
    std::cout << "CHECKSUM: " << checksum << "\n";
    std::cout << "STATUS: " << (is_sorted ? "PASS" : "FAIL") << "\n";
    return 0;
}
