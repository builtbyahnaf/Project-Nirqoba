#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    size_t size = 512ULL * 1024ULL * 1024ULL; // 512 MiB
    if (argc > 1) {
        size = (size_t)atoll(argv[1]) * 1024ULL * 1024ULL;
    }

    double t0 = bench_now_ms();
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) return 1;

    for (size_t i = 0; i < size; i += 4096) {
        buf[i] = (uint8_t)(i & 0xFF);
    }
    double t1 = bench_now_ms();
    double write_ms = t1 - t0;

    double t2 = bench_now_ms();
    uint64_t checksum = 0;
    for (size_t i = 0; i < size; i += 1024) {
        checksum += buf[i];
    }
    double t3 = bench_now_ms();
    double read_ms = t3 - t2;

    double t4 = bench_now_ms();
    for (size_t i = 0; i < size; i += 4096) {
        buf[i] = (uint8_t)((buf[i] * 3) ^ 0x55);
    }
    double t5 = bench_now_ms();
    double transform_ms = t5 - t4;

    double total_ms = write_ms + read_ms + transform_ms;
    double throughput_mib = ((double)size / (1024.0 * 1024.0)) / (total_ms / 1000.0);

    printf("WORKLOAD: ST4-10\n");
    printf("LANGUAGE: C\n");
    printf("BUFFER_MIB: %zu\n", size / (1024 * 1024));
    printf("TIME_MS: %.3f\n", total_ms);
    printf("THROUGHPUT_MIBS: %.2f\n", throughput_mib);
    printf("CHECKSUM: %" PRIu64 "\n", checksum);
    printf("STATUS: PASS\n");

    free(buf);
    return 0;
}
