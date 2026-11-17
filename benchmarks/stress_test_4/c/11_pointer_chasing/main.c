#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    size_t n = 10000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
    }

    size_t* next_idx = (size_t*)malloc(n * sizeof(size_t));
    if (!next_idx) return 1;

    for (size_t i = 0; i < n; i++) {
        next_idx[i] = i;
    }

    uint32_t state = 0xDEADBEEFU;
    for (size_t i = n - 1; i > 0; i--) {
        state = (state * 1664525U + 1013904223U);
        size_t j = state % (i + 1);
        size_t tmp = next_idx[i];
        next_idx[i] = next_idx[j];
        next_idx[j] = tmp;
    }

    double t0 = bench_now_ms();
    size_t curr = 0;
    uint64_t checksum = 0;
    for (size_t i = 0; i < n; i++) {
        checksum += curr;
        curr = next_idx[curr];
    }
    double t1 = bench_now_ms();
    double time_ms = t1 - t0;

    printf("WORKLOAD: ST4-11\n");
    printf("LANGUAGE: C\n");
    printf("NODES: %zu\n", n);
    printf("TIME_MS: %.3f\n", time_ms);
    printf("CHECKSUM: %" PRIu64 "\n", checksum);
    printf("STATUS: PASS\n");

    free(next_idx);
    return 0;
}
