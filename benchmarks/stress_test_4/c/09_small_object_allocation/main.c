#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

typedef struct Node {
    uint64_t a;
    uint64_t b;
    struct Node* next;
} Node;

int main(int argc, char** argv) {
    size_t n = 10000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
    }

    double t0 = bench_now_ms();
    Node* head = NULL;
    Node* curr = NULL;
    for (size_t i = 0; i < n; i++) {
        Node* node = (Node*)malloc(sizeof(Node));
        node->a = (uint64_t)i;
        node->b = (uint64_t)i * 2;
        node->next = NULL;
        if (!head) {
            head = node;
            curr = node;
        } else {
            curr->next = node;
            curr = node;
        }
    }
    double t1 = bench_now_ms();
    double alloc_ms = t1 - t0;

    double t2 = bench_now_ms();
    uint64_t checksum = 0;
    Node* walker = head;
    while (walker) {
        checksum += walker->a + walker->b;
        walker = walker->next;
    }
    double t3 = bench_now_ms();
    double traverse_ms = t3 - t2;

    double t4 = bench_now_ms();
    walker = head;
    while (walker) {
        Node* next = walker->next;
        free(walker);
        walker = next;
    }
    double t5 = bench_now_ms();
    double reclaim_ms = t5 - t4;

    double total_ms = alloc_ms + traverse_ms + reclaim_ms;
    printf("WORKLOAD: ST4-09\n");
    printf("LANGUAGE: C\n");
    printf("OBJECTS: %zu\n", n);
    printf("ALLOC_MS: %.3f\n", alloc_ms);
    printf("TRAVERSE_MS: %.3f\n", traverse_ms);
    printf("RECLAIM_MS: %.3f\n", reclaim_ms);
    printf("TIME_MS: %.3f\n", total_ms);
    printf("CHECKSUM: %" PRIu64 "\n", checksum);
    printf("STATUS: PASS\n");
    return 0;
}
