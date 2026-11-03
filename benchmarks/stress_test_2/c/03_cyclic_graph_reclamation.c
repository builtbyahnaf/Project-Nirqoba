// Benchmark 03 - Cyclic graph reclamation, C track.
//
// Workload: create many two-node reference cycles. The only references to a
// pair are the two nodes' mutual pointers, so after construction no external
// root exists. A tracing collector must reclaim the whole set; a reference
// counting scheme without cycle detection would leak it. Only the collection is
// timed; construction is reported separately.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "bench_clock.h"
#include "mini_gc.h"

#define CYCLE_PAIRS 500000u
#define CYCLE_NODES (CYCLE_PAIRS * 2u)
#define WARMUP_ROUNDS 2
#define MEASURE_ROUNDS 5

typedef struct Node {
  MiniGcObject header;
  struct Node* left;
  struct Node* right;
  uint64_t value;
} Node;

static void trace_node(MiniGcObject* object, MiniGcVisitFn visit,
                       void* context) {
  Node* node = (Node*)object;
  if (node->left != NULL) {
    visit(&node->left->header, context);
  }
  if (node->right != NULL) {
    visit(&node->right->header, context);
  }
}

static uint64_t build_cycles(MiniGc* gc, size_t pairs) {
  uint64_t checksum = 0;
  for (size_t index = 0; index < pairs; ++index) {
    Node* first = (Node*)mini_gc_alloc(gc, sizeof(Node), trace_node);
    Node* second = (Node*)mini_gc_alloc(gc, sizeof(Node), trace_node);
    first->value = (uint64_t)(index * 2u);
    second->value = (uint64_t)(index * 2u + 1u);
    first->left = second;
    second->left = first;
    checksum += first->value + second->value;
  }
  return checksum;
}

int main(void) {
  MiniGc gc;
  mini_gc_init(&gc, 64u * 1024u * 1024u);
  mini_gc_set_root(&gc, NULL);

  uint64_t checksum = 0;
  for (int warmup = 0; warmup < WARMUP_ROUNDS; ++warmup) {
    checksum = build_cycles(&gc, CYCLE_PAIRS);
    mini_gc_collect(&gc);
  }

  printf("TEST cyclic_graph_reclamation\n");
  printf("IMPL c\n");
  printf("OBJECTS %u\n", (unsigned)CYCLE_NODES);
  printf("WARMUP_ROUNDS %d\n", WARMUP_ROUNDS);
  printf("MEASURE_ROUNDS %d\n", MEASURE_ROUNDS);

  for (int round = 0; round < MEASURE_ROUNDS; ++round) {
    double build_start = bench_now_ms();
    uint64_t round_checksum = build_cycles(&gc, CYCLE_PAIRS);
    double build_ms = bench_now_ms() - build_start;
    checksum = round_checksum;

    double collect_start = bench_now_ms();
    size_t live = mini_gc_collect(&gc);
    double collect_ms = bench_now_ms() - collect_start;

    printf("ROUND %d\n", round);
    printf("BUILD_MS %.3f\n", build_ms);
    printf("COLLECT_MS %.3f\n", collect_ms);
    printf("CHECKSUM %" PRIu64 "\n", round_checksum);
    printf("LIVE_AFTER %zu\n", live);
  }

  printf("FINAL_CHECKSUM %" PRIu64 "\n", checksum);
  mini_gc_destroy(&gc);
  return 0;
}
