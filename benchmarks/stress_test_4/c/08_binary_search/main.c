#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

static inline int64_t binary_search(const int64_t* arr, size_t n, int64_t target) {
    size_t low = 0;
    size_t high = n - 1;
    while (low <= high) {
        size_t mid = low + ((high - low) >> 1);
        int64_t mid_val = arr[mid];
        if (mid_val == target) {
            return (int64_t)mid;
        } else if (mid_val < target) {
            low = mid + 1;
        } else {
            if (mid == 0) break;
            high = mid - 1;
        }
    }
    return -1;
}

int main(int argc, char** argv) {
    size_t n = 20000000;
    size_t queries = 10000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
        queries = n / 2;
    }

    int64_t* arr = (int64_t*)malloc(n * sizeof(int64_t));
    if (!arr) return 1;

    for (size_t i = 0; i < n; i++) {
        arr[i] = (int64_t)i * 2;
    }

    double t0 = bench_now_ms();
    int64_t hit_count = 0;
    int64_t checksum = 0;
    int64_t max_val = (int64_t)n * 2;

    for (size_t q = 0; q < queries; q++) {
        int64_t target = ((int64_t)q * 3) % max_val;
        int64_t idx = binary_search(arr, n, target);
        if (idx != -1) {
            hit_count++;
            checksum += idx;
        } else {
            checksum -= 1;
        }
    }

    double t1 = bench_now_ms();
    double time_ms = t1 - t0;

    printf("WORKLOAD: ST4-08\n");
    printf("LANGUAGE: C\n");
    printf("ARRAY_SIZE: %zu\n", n);
    printf("QUERIES: %zu\n", queries);
    printf("HITS: %" PRId64 "\n", hit_count);
    printf("TIME_MS: %.3f\n", time_ms);
    printf("CHECKSUM: %" PRId64 "\n", checksum);
    printf("STATUS: PASS\n");

    free(arr);
    return 0;
}
