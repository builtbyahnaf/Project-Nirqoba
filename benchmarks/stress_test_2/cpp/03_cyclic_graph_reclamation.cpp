// Benchmark 03 - Cyclic graph reclamation, C++ track.
//
// Two-node cycles with no external root. The embedded precise mark-sweep
// collector must reclaim all of them; reference counting alone would leak the
// cycle. Construction and collection are timed separately.
#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "bench_clock.hpp"
#include "mini_gc.hpp"

namespace {

constexpr std::uint32_t kCyclePairs = 500000u;
constexpr std::uint32_t kCycleNodes = kCyclePairs * 2u;
constexpr int kWarmupRounds = 2;
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

std::uint64_t build_cycles(MiniGc& gc, std::size_t pairs) {
  std::uint64_t checksum = 0;
  for (std::size_t index = 0; index < pairs; ++index) {
    auto* first = reinterpret_cast<Node*>(gc.alloc(sizeof(Node), trace_node));
    auto* second = reinterpret_cast<Node*>(gc.alloc(sizeof(Node), trace_node));
    first->value = index * 2u;
    second->value = index * 2u + 1u;
    first->left = second;
    second->left = first;
    checksum += first->value + second->value;
  }
  return checksum;
}

}  // namespace

int main() {
  MiniGc gc(64u * 1024u * 1024u);
  gc.set_root(nullptr);

  std::uint64_t checksum = 0;
  for (int warmup = 0; warmup < kWarmupRounds; ++warmup) {
    checksum = build_cycles(gc, kCyclePairs);
    gc.collect();
  }

  std::printf("TEST cyclic_graph_reclamation\n");
  std::printf("IMPL cpp\n");
  std::printf("OBJECTS %u\n", kCycleNodes);
  std::printf("WARMUP_ROUNDS %d\n", kWarmupRounds);
  std::printf("MEASURE_ROUNDS %d\n", kMeasureRounds);

  for (int round = 0; round < kMeasureRounds; ++round) {
    const double build_start = bench_now_ms();
    const std::uint64_t round_checksum = build_cycles(gc, kCyclePairs);
    const double build_ms = bench_now_ms() - build_start;
    checksum = round_checksum;

    const double collect_start = bench_now_ms();
    const std::size_t live = gc.collect();
    const double collect_ms = bench_now_ms() - collect_start;

    std::printf("ROUND %d\n", round);
    std::printf("BUILD_MS %.3f\n", build_ms);
    std::printf("COLLECT_MS %.3f\n", collect_ms);
    std::printf("CHECKSUM %" PRIu64 "\n", round_checksum);
    std::printf("LIVE_AFTER %zu\n", live);
  }

  std::printf("FINAL_CHECKSUM %" PRIu64 "\n", checksum);
  return 0;
}
