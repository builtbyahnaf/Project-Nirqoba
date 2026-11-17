#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

static int compare_i32(const void* a, const void* b) {
    int32_t arg1 = *(const int32_t*)a;
    int32_t arg2 = *(const int32_t*)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

int main(int argc, char** argv) {
    size_t n = 5000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
    }

    int32_t* arr = (int32_t*)malloc(n * sizeof(int32_t));
    if (!arr) return 1;

    uint32_t state = 0x12345678U;
    for (size_t i = 0; i < n; i++) {
        state = (state * 1664525U + 1013904223U);
        arr[i] = (int32_t)state;
    }

    double t0 = bench_now_ms();
    qsort(arr, n, sizeof(int32_t), compare_i32);
    double t1 = bench_now_ms();
    double sort_ms = t1 - t0;

    int is_sorted = 1;
    for (size_t i = 0; i < (n < 10000 ? n - 1 : 10000); i++) {
        if (arr[i] > arr[i + 1]) {
            is_sorted = 0;
            break;
        }
    }

    uint64_t checksum = 0;
    for (size_t i = 0; i < n; i += 1000) {
        checksum += (uint64_t)arr[i];
    }

    printf("WORKLOAD: ST4-06\n");
    printf("LANGUAGE: C\n");
    printf("ELEMENTS: %zu\n", n);
    printf("TIME_MS: %.3f\n", sort_ms);
    printf("CHECKSUM: %" PRIu64 "\n", checksum);
    printf("STATUS: %s\n", is_sorted ? "PASS" : "FAIL");

    free(arr);
    return 0;
}
