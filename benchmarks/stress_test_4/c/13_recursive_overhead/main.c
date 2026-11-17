#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

static void merge_sort_recursive(int32_t* arr, int32_t* temp, size_t left, size_t right) {
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
        n = (size_t)atoll(argv[1]);
    }

    int32_t* arr = (int32_t*)malloc(n * sizeof(int32_t));
    int32_t* temp = (int32_t*)malloc(n * sizeof(int32_t));
    if (!arr || !temp) return 1;

    uint32_t state = 0x54321U;
    for (size_t i = 0; i < n; i++) {
        state = (state * 1664525U + 1013904223U);
        arr[i] = (int32_t)state;
    }

    double t0 = bench_now_ms();
    merge_sort_recursive(arr, temp, 0, n - 1);
    double t1 = bench_now_ms();
    double sort_ms = t1 - t0;

    int is_sorted = (arr[0] <= arr[n - 1]);
    uint64_t checksum = 0;
    for (size_t i = 0; i < n; i += 1000) {
        checksum += (uint64_t)arr[i];
    }

    printf("WORKLOAD: ST4-13\n");
    printf("LANGUAGE: C\n");
    printf("ELEMENTS: %zu\n", n);
    printf("TIME_MS: %.3f\n", sort_ms);
    printf("CHECKSUM: %" PRIu64 "\n", checksum);
    printf("STATUS: %s\n", is_sorted ? "PASS" : "FAIL");

    free(arr);
    free(temp);
    return 0;
}
