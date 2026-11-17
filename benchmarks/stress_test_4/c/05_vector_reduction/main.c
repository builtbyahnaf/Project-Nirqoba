#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    size_t n = 50000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
    }

    double t0 = bench_now_ms();
    uint64_t* vec = (uint64_t*)malloc(n * sizeof(uint64_t));
    if (!vec) return 1;

    for (size_t i = 0; i < n; i++) {
        vec[i] = (uint64_t)i ^ 0x55555555ULL;
    }
    double t1 = bench_now_ms();
    double construct_ms = t1 - t0;

    double t2 = bench_now_ms();
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) {
        total += vec[i];
    }
    double t3 = bench_now_ms();
    double reduce_ms = t3 - t2;

    printf("WORKLOAD: ST4-05\n");
    printf("LANGUAGE: C\n");
    printf("ELEMENTS: %zu\n", n);
    printf("CONSTRUCTION_MS: %.3f\n", construct_ms);
    printf("REDUCTION_MS: %.3f\n", reduce_ms);
    printf("TIME_MS: %.3f\n", construct_ms + reduce_ms);
    printf("CHECKSUM: %" PRIu64 "\n", total);
    printf("STATUS: PASS\n");

    free(vec);
    return 0;
}
