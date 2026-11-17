#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

typedef struct CycleNode {
    struct CycleNode* peer;
    int value;
} CycleNode;

int main(int argc, char** argv) {
    size_t pairs_count = 500000; // 1M nodes total
    if (argc > 1) {
        pairs_count = (size_t)atoll(argv[1]);
    }

    double t0 = bench_now_ms();
    CycleNode** a_nodes = (CycleNode**)malloc(pairs_count * sizeof(CycleNode*));
    CycleNode** b_nodes = (CycleNode**)malloc(pairs_count * sizeof(CycleNode*));

    for (size_t i = 0; i < pairs_count; i++) {
        CycleNode* a = (CycleNode*)malloc(sizeof(CycleNode));
        CycleNode* b = (CycleNode*)malloc(sizeof(CycleNode));
        a->value = (int)i;
        b->value = (int)i;
        a->peer = b;
        b->peer = a;
        a_nodes[i] = a;
        b_nodes[i] = b;
    }
    double t1 = bench_now_ms();
    double build_ms = t1 - t0;

    // Reclamation phase: break cycles and reclaim
    double t2 = bench_now_ms();
    for (size_t i = 0; i < pairs_count; i++) {
        a_nodes[i]->peer = NULL;
        b_nodes[i]->peer = NULL;
        free(a_nodes[i]);
        free(b_nodes[i]);
    }
    free(a_nodes);
    free(b_nodes);
    double t3 = bench_now_ms();
    double reclaim_ms = t3 - t2;

    double total_ms = build_ms + reclaim_ms;
    printf("WORKLOAD: ST4-16\n");
    printf("LANGUAGE: C\n");
    printf("CYCLES: %zu\n", pairs_count);
    printf("BUILD_MS: %.3f\n", build_ms);
    printf("RECLAIM_MS: %.3f\n", reclaim_ms);
    printf("TIME_MS: %.3f\n", total_ms);
    printf("STATUS: PASS\n");
    return 0;
}
