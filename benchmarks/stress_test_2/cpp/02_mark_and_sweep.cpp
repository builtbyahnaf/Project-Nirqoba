// Benchmark 02 - Mark and sweep, C++ track.
//
// Same reachable binary tree and same precise tracing contract as the C and
// Nirqoba tracks. Only full collection batches are timed.
#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "bench_clock.hpp"
#include "mini_gc.hpp"

namespace {

constexpr int kTreeDepth = 20;
constexpr std::uint32_t kTreeNodes =
    (1u << (kTreeDepth + 1)) - 1u;
constexpr int kWarmupBatches = 3;
constexpr int kCollectBatch = 5;
constexpr int kMeasureRounds = 5;

struct Node {
  MiniGcObject header;
  Node* left;
  Node* right;
  std::uint64_t value;
};

void trace_node(MiniGcObject* object, MiniGcVisitFn visit, void* context) {
  auto* node = reinterpret_cast<Node*>(object);
  if (node->left != nullptr) {
    visit(&node->left->header, context);
  }
  if (node->right != nullptr) {
    visit(&node->right->header, context);
  }
}

Node* build_tree(MiniGc& gc, int depth, std::uint64_t value) {
  auto* node = reinterpret_cast<Node*>(gc.alloc(sizeof(Node), trace_node));
  node->value = value;
  if (depth > 0) {
    node->left = build_tree(gc, depth - 1, value * 2);
    node->right = build_tree(gc, depth - 1, value * 2 + 1);
  }
  return node;
}

std::uint64_t sum_tree(const Node* node, int depth) {
  if (depth == 0) {
    return node->value;
  }
  return node->value + sum_tree(node->left, depth - 1) +
         sum_tree(node->right, depth - 1);
}

}  // namespace

int main() {
  MiniGc gc(64u * 1024u * 1024u);

  const double build_start = bench_now_ms();
  Node* root = build_tree(gc, kTreeDepth, 1);
  const double build_ms = bench_now_ms() - build_start;
  gc.set_root(&root->header);

  const double traverse_start = bench_now_ms();
  const std::uint64_t checksum = sum_tree(root, kTreeDepth);
  const double traverse_ms = bench_now_ms() - traverse_start;

  for (int warmup = 0; warmup < kWarmupBatches; ++warmup) {
    for (int collection = 0; collection < kCollectBatch; ++collection) {
      gc.collect();
    }
  }

  std::printf("TEST mark_and_sweep\n");
  std::printf("IMPL cpp\n");
  std::printf("OBJECTS %u\n", kTreeNodes);
  std::printf("WARMUP_BATCHES %d\n", kWarmupBatches);
  std::printf("COLLECT_BATCH %d\n", kCollectBatch);
  std::printf("MEASURE_ROUNDS %d\n", kMeasureRounds);
  std::printf("BUILD_MS %.3f\n", build_ms);
  std::printf("TRAVERSE_MS %.3f\n", traverse_ms);
  std::printf("CHECKSUM %" PRIu64 "\n", checksum);

  for (int round = 0; round < kMeasureRounds; ++round) {
    const std::size_t before = gc.collections();
    const double start = bench_now_ms();
    for (int collection = 0; collection < kCollectBatch; ++collection) {
      gc.collect();
    }
    const double elapsed = bench_now_ms() - start;
    std::printf("ROUND %d\n", round);
    std::printf("COLLECT_MS %.3f\n", elapsed / kCollectBatch);
    std::printf("LIVE_AFTER %zu\n", gc.live_objects());
    std::printf("COLLECTIONS_DELTA %zu\n", gc.collections() - before);
  }

  return 0;
}
