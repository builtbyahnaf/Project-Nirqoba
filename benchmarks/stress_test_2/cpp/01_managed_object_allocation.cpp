// Benchmark 01 - Managed-object allocation, C++ track.
//
// Native new/delete chain baseline. This is intentionally not routed through the
// embedded collector: it represents the fastest ordinary C++ object lifetime and
// makes the no-GC cost explicit next to Nirqoba and Java.
#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "bench_clock.hpp"

namespace {

constexpr std::uint32_t kObjectCount = 1000000u;
constexpr int kWarmupRounds = 2;
constexpr int kMeasureRounds = 5;

struct Node {
  Node* next = nullptr;
  std::uint64_t value = 0;
};

Node* build_chain(std::size_t count) {
  Node* head = new Node();
  Node* tail = head;
  for (std::size_t index = 1; index < count; ++index) {
    Node* node = new Node();
    node->value = index;
    tail->next = node;
    tail = node;
  }
  return head;
}

std::uint64_t sum_chain(const Node* head, std::size_t count) {
  std::uint64_t total = 0;
  const Node* cursor = head;
  for (std::size_t index = 0; index < count; ++index) {
    total += cursor->value;
    cursor = cursor->next;
  }
  return total;
}

void free_chain(Node* head) {
  while (head != nullptr) {
    Node* next = head->next;
    delete head;
    head = next;
  }
}

std::uint64_t run_round(bool verbose) {
  const double alloc_start = bench_now_ms();
  Node* head = build_chain(kObjectCount);
  const double alloc_ms = bench_now_ms() - alloc_start;

  const double traverse_start = bench_now_ms();
  const std::uint64_t checksum = sum_chain(head, kObjectCount);
  const double traverse_ms = bench_now_ms() - traverse_start;

  const double collect_start = bench_now_ms();
  free_chain(head);
  const double collect_ms = bench_now_ms() - collect_start;

  if (verbose) {
    std::printf("ALLOC_MS %.3f\n", alloc_ms);
    std::printf("TRAVERSE_MS %.3f\n", traverse_ms);
    std::printf("COLLECT_MS %.3f\n", collect_ms);
    std::printf("CHECKSUM %" PRIu64 "\n", checksum);
    std::printf("LIVE_AFTER 0\n");
  }
  return checksum;
}

}  // namespace

int main() {
  for (int warmup = 0; warmup < kWarmupRounds; ++warmup) {
    (void)run_round(false);
  }

  std::printf("TEST managed_object_allocation\n");
  std::printf("IMPL cpp\n");
  std::printf("OBJECTS %u\n", kObjectCount);
  std::printf("WARMUP_ROUNDS %d\n", kWarmupRounds);
  std::printf("MEASURE_ROUNDS %d\n", kMeasureRounds);

  for (int round = 0; round < kMeasureRounds; ++round) {
    std::printf("ROUND %d\n", round);
    (void)run_round(true);
  }
  return 0;
}
