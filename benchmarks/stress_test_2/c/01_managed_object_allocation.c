// Benchmark 01 - Managed-object allocation, C track.
//
// C has no tracing collector, so this track measures the closest native
// equivalent: one malloc per node, a linked chain of one million nodes, a
// checksum traversal, then deterministic release. This is the "best case,
// no-GC metadata" baseline against Nirqoba's managed heap and the JVM's GC.
// The allocation and release phases are timed separately.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "bench_clock.h"

#define OBJECT_COUNT 1000000u
#define WARMUP_ROUNDS 2
#define MEASURE_ROUNDS 5

typedef struct Node {
  struct Node* next;
  uint64_t value;
} Node;

static Node* build_chain(size_t count) {
  Node* head = (Node*)malloc(sizeof(Node));
  if (head == NULL) {
    abort();
  }
  head->next = NULL;
  head->value = 0;
  Node* tail = head;
  for (size_t index = 1; index < count; ++index) {
    Node* node = (Node*)malloc(sizeof(Node));
    if (node == NULL) {
      abort();
    }
    node->next = NULL;
    node->value = (uint64_t)index;
    tail->next = node;
    tail = node;
  }
  return head;
}

static uint64_t sum_chain(const Node* head, size_t count) {
  uint64_t total = 0;
  const Node* cursor = head;
  for (size_t index = 0; index < count; ++index) {
    total += cursor->value;
    cursor = cursor->next;
  }
  return total;
}

static void free_chain(Node* head) {
  while (head != NULL) {
    Node* next = head->next;
    free(head);
    head = next;
  }
}

static uint64_t run_round(int verbose) {
  double alloc_start = bench_now_ms();
  Node* head = build_chain(OBJECT_COUNT);
  double alloc_ms = bench_now_ms() - alloc_start;

  double traverse_start = bench_now_ms();
  uint64_t checksum = sum_chain(head, OBJECT_COUNT);
  double traverse_ms = bench_now_ms() - traverse_start;

  double collect_start = bench_now_ms();
  free_chain(head);
  double collect_ms = bench_now_ms() - collect_start;

  if (verbose) {
    printf("ALLOC_MS %.3f\n", alloc_ms);
    printf("TRAVERSE_MS %.3f\n", traverse_ms);
    printf("COLLECT_MS %.3f\n", collect_ms);
    printf("CHECKSUM %" PRIu64 "\n", checksum);
    printf("LIVE_AFTER 0\n");
  }
  return checksum;
}

int main(void) {
  for (int warmup = 0; warmup < WARMUP_ROUNDS; ++warmup) {
    (void)run_round(0);
  }

  printf("TEST managed_object_allocation\n");
  printf("IMPL c\n");
  printf("OBJECTS %u\n", (unsigned)OBJECT_COUNT);
  printf("WARMUP_ROUNDS %d\n", WARMUP_ROUNDS);
  printf("MEASURE_ROUNDS %d\n", MEASURE_ROUNDS);

  for (int round = 0; round < MEASURE_ROUNDS; ++round) {
    printf("ROUND %d\n", round);
    (void)run_round(1);
  }
  return 0;
}
