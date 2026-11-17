#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    long long total_ops = 100000000LL;
    if (argc > 1) {
        total_ops = atoll(argv[1]);
    }

    double t0 = bench_now_ms();
    double acc = 1.0;

    for (long long i = 1; i <= total_ops; i++) {
        acc = acc + 0.001;
        acc = acc * 1.000001;
        acc = acc / 1.0000005;
        acc = acc + sqrt(acc) * 0.00001;
    }

    double t1 = bench_now_ms();
    double time_ms = t1 - t0;

    printf("WORKLOAD: ST4-02\n");
    printf("LANGUAGE: C\n");
    printf("OPS: %lld\n", total_ops);
    printf("TIME_MS: %.3f\n", time_ms);
    printf("CHECKSUM: %.6f\n", acc);
    printf("STATUS: PASS\n");
    return 0;
}
