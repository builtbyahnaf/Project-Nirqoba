// Benchmark 02 - Mark and sweep, C track.
//
// Workload: build one precise binary tree whose every node is reachable from a
// single root, then force repeated full mark-and-sweep collections. Marking must
// visit every node and every reference field; sweeping must visit every object
// but reclaim none because all are reachable. Only the collection batches are
// timed, so tree construction and checksum traversal do not pollute the GC
// measurement.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "bench_clock.h"
#include "mini_gc.h"

#define TREE_DEPTH 20
#define TREE_NODES ((1u << (TREE_DEPTH + 1)) - 1u)
#define WARMUP_BATCHES 3
#define COLLECT_BATCH 5
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

static Node* build_tree(MiniGc* gc, int depth, uint64_t value) {
  Node* node = (Node*)mini_gc_alloc(gc, sizeof(Node), trace_node);
  node->value = value;
  if (depth > 0) {
    node->left = build_tree(gc, depth - 1, value * 2);
    node->right = build_tree(gc, depth - 1, value * 2 + 1);
  }
  return node;
}

static uint64_t sum_tree(const Node* node, int depth) {
  if (depth == 0) {
    return node->value;
  }
  return node->value + sum_tree(node->left, depth - 1) +
         sum_tree(node->right, depth - 1);
}

int main(void) {
  MiniGc gc;
  mini_gc_init(&gc, 64u * 1024u * 1024u);

  double build_start = bench_now_ms();
  Node* root = build_tree(&gc, TREE_DEPTH, 1);
  double build_ms = bench_now_ms() - build_start;
  mini_gc_set_root(&gc, &root->header);

  double traverse_start = bench_now_ms();
  uint64_t checksum = sum_tree(root, TREE_DEPTH);
  double traverse_ms = bench_now_ms() - traverse_start;

  for (int warmup = 0; warmup < WARMUP_BATCHES; ++warmup) {
    for (int collection = 0; collection < COLLECT_BATCH; ++collection) {
      mini_gc_collect(&gc);
    }
  }

  printf("TEST mark_and_sweep\n");
  printf("IMPL c\n");
  printf("OBJECTS %u\n", (unsigned)TREE_NODES);
  printf("WARMUP_BATCHES %d\n", WARMUP_BATCHES);
  printf("COLLECT_BATCH %d\n", COLLECT_BATCH);
  printf("MEASURE_ROUNDS %d\n", MEASURE_ROUNDS);
  printf("BUILD_MS %.3f\n", build_ms);
  printf("TRAVERSE_MS %.3f\n", traverse_ms);
  printf("CHECKSUM %" PRIu64 "\n", checksum);

  for (int round = 0; round < MEASURE_ROUNDS; ++round) {
    size_t before = gc.collections;
    double start = bench_now_ms();
    for (int collection = 0; collection < COLLECT_BATCH; ++collection) {
      mini_gc_collect(&gc);
    }
    double elapsed = bench_now_ms() - start;
    printf("ROUND %d\n", round);
    printf("COLLECT_MS %.3f\n", elapsed / (double)COLLECT_BATCH);
    printf("LIVE_AFTER %zu\n", gc.live_objects);
    printf("COLLECTIONS_DELTA %zu\n", gc.collections - before);
  }

  mini_gc_destroy(&gc);
  return 0;
}
