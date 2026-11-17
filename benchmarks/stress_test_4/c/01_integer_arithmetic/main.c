#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    uint64_t total_ops = 500000000ULL;
    if (argc > 1) {
        total_ops = strtoull(argv[1], NULL, 10);
    }

    double t0 = bench_now_ms();
    uint64_t total = 0;

    for (uint64_t i = 1; i <= total_ops; i++) {
        total = (total + i);
        total = (total - (i & 0xFF));
        total = (total * 3ULL);
        uint64_t div = (i % 7ULL) + 1ULL;
        total = (total ^ (total / div));
        total = (total ^ (total << 5) ^ (total >> 59));
    }

    double t1 = bench_now_ms();
    double time_ms = t1 - t0;

    printf("WORKLOAD: ST4-01\n");
    printf("LANGUAGE: C\n");
    printf("OPS: %" PRIu64 "\n", total_ops);
    printf("TIME_MS: %.3f\n", time_ms);
    printf("CHECKSUM: %" PRIu64 "\n", total);
    printf("STATUS: PASS\n");
    return 0;
}
