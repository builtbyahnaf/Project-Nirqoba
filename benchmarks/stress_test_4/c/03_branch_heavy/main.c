#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    long long n = 100000000LL;
    if (argc > 1) {
        n = atoll(argv[1]);
    }

    double t0 = bench_now_ms();
    uint32_t state = 123456789U;
    uint64_t checksum = 0;

    for (long long i = 0; i < n; i++) {
        state = (state * 1664525U + 1013904223U);
        if ((i & 1) == 0) {
            if ((state & 1) != 0) {
                checksum += (uint64_t)(state ^ 0x55555555U);
            } else {
                if (((state >> 16) & 1) != 0) {
                    checksum += (uint64_t)(state ^ 0xAAAAAAAAU);
                } else {
                    checksum += (uint64_t)(state * 3U);
                }
            }
        } else {
            if (state < 0x80000000U) {
                checksum ^= (uint64_t)(state >> 3);
            } else {
                checksum -= (uint64_t)(state & 0xFFFFU);
            }
        }
    }

    double t1 = bench_now_ms();
    double time_ms = t1 - t0;

    printf("WORKLOAD: ST4-03\n");
    printf("LANGUAGE: C\n");
    printf("INPUTS: %lld\n", n);
    printf("TIME_MS: %.3f\n", time_ms);
    printf("CHECKSUM: %" PRIu64 "\n", checksum);
    printf("STATUS: PASS\n");
    return 0;
}
