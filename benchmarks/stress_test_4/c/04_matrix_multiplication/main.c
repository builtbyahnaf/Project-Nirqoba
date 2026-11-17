#include <stdio.h>
#include <stdlib.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    int size = 1024;
    if (argc > 1) {
        size = atoi(argv[1]);
    }

    size_t total_elements = (size_t)size * (size_t)size;
    double* a = (double*)malloc(total_elements * sizeof(double));
    double* b = (double*)malloc(total_elements * sizeof(double));
    double* c = (double*)calloc(total_elements, sizeof(double));

    if (!a || !b || !c) {
        fprintf(stderr, "Allocation failed\n");
        return 1;
    }

    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            a[i * size + j] = (double)((i + j) % 100) / 100.0;
            b[i * size + j] = (double)((i * j) % 100) / 100.0;
        }
    }

    double t0 = bench_now_ms();
    // Cache-friendly i-k-j loop
    for (int i = 0; i < size; i++) {
        int i_off = i * size;
        for (int k = 0; k < size; k++) {
            double a_ik = a[i_off + k];
            int k_off = k * size;
            for (int j = 0; j < size; j++) {
                c[i_off + j] += a_ik * b[k_off + j];
            }
        }
    }
    double t1 = bench_now_ms();
    double time_ms = t1 - t0;

    double checksum = 0.0;
    for (size_t i = 0; i < total_elements; i++) {
        checksum += c[i];
    }

    printf("WORKLOAD: ST4-04\n");
    printf("LANGUAGE: C\n");
    printf("MATRIX_DIM: %dx%d\n", size, size);
    printf("TIME_MS: %.3f\n", time_ms);
    printf("CHECKSUM: %.6f\n", checksum);
    printf("STATUS: PASS\n");

    free(a);
    free(b);
    free(c);
    return 0;
}
