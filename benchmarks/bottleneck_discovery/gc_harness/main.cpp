// Nirqoba GC regression harness (diagnostic only; not part of the build).
//
// Reproduces the object graph and the call sequence the Nirqoba compiler emits
// for benchmarks/stress_test_2 tests 02 and 03, so the collector can be built
// from any historical commit and measured against the same workload.
//
// It uses only the public C ABI in runtime/include/nirqoba/runtime/gc.h.
//
// Emitted per node (see `nirqoba build ... --emit=llvm` for 02_mark_and_sweep):
//   nrq_gc_root_frame_begin
//   nrq_gc_root_register_frame_static        (2 slots)
//   nrq_gc_allocate_array_typed_rooted
//   nrq_gc_root_store                        (node -> parent slot)
//   ... recursion ...
//   nrq_gc_unpin + nrq_gc_root_store          (child -> node field)
//   nrq_gc_pin_transfer
//   nrq_gc_root_frame_end

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nirqoba/runtime/gc.h"

namespace {

double now_ms() {
  static LARGE_INTEGER frequency{};
  if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}

// Descriptor layout consumed by nrq_gc_root_register_frame_static:
//   [0] slot count, [1] total words, then per slot (object size, ref count,
//   offsets...). Two slots: each a 8-byte pointer root with one offset at 0.
constexpr size_t kRootDescriptor[] = {2, 8, 8, 1, 0, 8, 1, 0};
// Reference layout for one TreeNode: two pointers at offsets 0 and 8.
constexpr size_t kTreeNodeLayout[] = {0, 8};
// Reference layout for one cycle Node: one pointer at offset 0.
constexpr size_t kCycleNodeLayout[] = {0};

struct TreeNode {
  void* left;
  void* right;
  int64_t value;
};
static_assert(sizeof(TreeNode) == 24, "TreeNode payload must be 24 bytes");

struct CycleNode {
  void* next;
  int64_t value;
};
static_assert(sizeof(CycleNode) == 16, "CycleNode payload must be 16 bytes");

void* build_tree(int64_t depth, int64_t value) {
  const uint64_t marker = nrq_gc_root_frame_begin();
  void* slots[2] = {nullptr, nullptr};
  void* table[2] = {&slots[0], &slots[1]};
  nrq_gc_root_register_frame_static(table, 2, kRootDescriptor);
  void* node = nrq_gc_allocate_array_typed_rooted(
      &slots[0], 1, sizeof(TreeNode), kTreeNodeLayout, 2);
  nrq_gc_root_store(&slots[0], node);
  static_cast<TreeNode*>(node)->value = value;
  if (depth > 0) {
    void* left = build_tree(depth - 1, value * 2);
    nrq_gc_unpin(left);
    void* parent = slots[0];
    nrq_gc_root_store(reinterpret_cast<void**>(parent), left);
    void* right = build_tree(depth - 1, value * 2 + 1);
    nrq_gc_unpin(right);
    parent = slots[0];
    nrq_gc_root_store(reinterpret_cast<void**>(
                          static_cast<unsigned char*>(parent) + 8),
                      right);
  }
  void* result = static_cast<void*>(slots[0]);
  result = nrq_gc_pin_transfer(result);
  nrq_gc_root_frame_end(marker);
  return result;
}

int64_t sum_tree(void* raw, int64_t depth) {
  auto* node = static_cast<TreeNode*>(raw);
  if (depth == 0) return node->value;
  return node->value + sum_tree(node->left, depth - 1) +
         sum_tree(node->right, depth - 1);
}

void make_one_cycle(int64_t index) {
  const uint64_t marker = nrq_gc_root_frame_begin();
  void* slots[2] = {nullptr, nullptr};
  void* table[2] = {&slots[0], &slots[1]};
  nrq_gc_root_register_frame_static(table, 2, kRootDescriptor);
  void* first = nrq_gc_allocate_array_typed_rooted(
      &slots[0], 1, sizeof(CycleNode), kCycleNodeLayout, 1);
  nrq_gc_root_store(&slots[0], first);
  void* second = nrq_gc_allocate_array_typed_rooted(
      &slots[1], 1, sizeof(CycleNode), kCycleNodeLayout, 1);
  nrq_gc_root_store(&slots[1], second);
  static_cast<CycleNode*>(first)->value = index;
  static_cast<CycleNode*>(second)->value = index + 1;
  nrq_gc_root_store(reinterpret_cast<void**>(first), second);
  nrq_gc_root_store(reinterpret_cast<void**>(second), first);
  nrq_gc_root_frame_end(marker);
}

struct Stats {
  double median;
  double mean;
  double min;
  double max;
};

Stats summarize(std::vector<double> values) {
  Stats out{};
  if (values.empty()) return out;
  std::sort(values.begin(), values.end());
  out.min = values.front();
  out.max = values.back();
  double sum = 0;
  for (double v : values) sum += v;
  out.mean = sum / static_cast<double>(values.size());
  const size_t n = values.size();
  out.median = (n % 2 == 1) ? values[n / 2]
                            : 0.5 * (values[n / 2 - 1] + values[n / 2]);
  return out;
}

void print_stats(const char* label, const Stats& s) {
  std::printf("%s_MEDIAN %.3f\n", label, s.median);
  std::printf("%s_MEAN %.3f\n", label, s.mean);
  std::printf("%s_MIN %.3f\n", label, s.min);
  std::printf("%s_MAX %.3f\n", label, s.max);
}

// test 02: one complete binary tree, every object live, nothing reclaimed.
void run_mark_and_sweep(int64_t depth, int collections_per_batch,
                        int warmup_batches, int measure_rounds) {
  const int64_t nodes = (int64_t{1} << (depth + 1)) - 1;
  nrq_gc_disable();
  nrq_gc_collect();

  const double build_start = now_ms();
  void* root = build_tree(depth, 1);
  const double build_ms = now_ms() - build_start;

  const double traverse_start = now_ms();
  const int64_t checksum = sum_tree(root, depth);
  const double traverse_ms = now_ms() - traverse_start;

  for (int batch = 0; batch < warmup_batches; ++batch)
    for (int i = 0; i < collections_per_batch; ++i) nrq_gc_collect();

  std::printf("TEST 02\nOBJECTS %lld\nBUILD_MS %.3f\nTRAVERSE_MS %.3f\n"
              "CHECKSUM %lld\n",
              static_cast<long long>(nodes), build_ms, traverse_ms,
              static_cast<long long>(checksum));
  std::fflush(stdout);

  std::vector<double> samples;
  for (int round = 0; round < measure_rounds; ++round) {
    const uint64_t before = nrq_gc_get_stats().collections;
    const double start = now_ms();
    for (int i = 0; i < collections_per_batch; ++i) nrq_gc_collect();
    const double elapsed = now_ms() - start;
    const uint64_t after = nrq_gc_get_stats().collections;
    const double per_collection =
        elapsed / static_cast<double>(collections_per_batch);
    samples.push_back(per_collection);
    std::printf("ROUND %d COLLECT_MS %.3f LIVE_AFTER %llu COLLECTIONS_DELTA %llu "
                "PAGES %llu RECYCLED %llu SPAN_REUSE %llu INDEX %llu\n",
                round, per_collection,
                static_cast<unsigned long long>(nrq_gc_get_live_objects()),
                static_cast<unsigned long long>(after - before),
                static_cast<unsigned long long>(nrq_gc_page_count()),
                static_cast<unsigned long long>(nrq_gc_recycled_page_count()),
                static_cast<unsigned long long>(nrq_gc_span_reuse_count()),
                static_cast<unsigned long long>(nrq_gc_object_index_entries()));
    std::fflush(stdout);
  }
  print_stats("COLLECT", summarize(samples));

  // Keep the root observably live across the measured batches.
  std::printf("ROOT_VALUE %lld\n", static_cast<long long>(
                                      static_cast<TreeNode*>(root)->value));
  nrq_gc_unpin(root);
  nrq_gc_disable();
}

// test 03: two-node cycles with no external root: all garbage at collection.
void run_cyclic(int64_t pairs, int warmup_rounds, int measure_rounds) {
  nrq_gc_disable();
  nrq_gc_collect();

  for (int round = 0; round < warmup_rounds; ++round) {
    for (int64_t index = 0; index < pairs; ++index) make_one_cycle(index * 2);
    nrq_gc_collect();
  }

  std::printf("TEST 03\nOBJECTS %lld\n", static_cast<long long>(pairs * 2));
  std::fflush(stdout);

  std::vector<double> build_samples;
  std::vector<double> collect_samples;
  for (int round = 0; round < measure_rounds; ++round) {
    const double build_start = now_ms();
    for (int64_t index = 0; index < pairs; ++index) make_one_cycle(index * 2);
    const double build_ms = now_ms() - build_start;

    const uint64_t live_before = nrq_gc_get_live_objects();
    const double start = now_ms();
    nrq_gc_collect();
    const double collect_ms = now_ms() - start;
    const uint64_t live_after = nrq_gc_get_live_objects();
    build_samples.push_back(build_ms);
    collect_samples.push_back(collect_ms);
    std::printf("ROUND %d BUILD_MS %.3f COLLECT_MS %.3f LIVE_BEFORE %llu "
                "LIVE_AFTER %llu PAGES %llu\n",
                round, build_ms, collect_ms,
                static_cast<unsigned long long>(live_before),
                static_cast<unsigned long long>(live_after),
                static_cast<unsigned long long>(nrq_gc_page_count()));
    std::fflush(stdout);
  }
  print_stats("BUILD", summarize(build_samples));
  print_stats("COLLECT", summarize(collect_samples));
}

// Allocation cost of one 24-byte managed node with no graph kept alive.
void run_allocation(int64_t count) {
  nrq_gc_disable();
  nrq_gc_collect();
  const double start = now_ms();
  for (int64_t index = 0; index < count; ++index) make_one_cycle(index * 2);
  const double elapsed = now_ms() - start;
  const uint64_t live = nrq_gc_get_live_objects();
  const double collect_start = now_ms();
  nrq_gc_collect();
  const double collect_ms = now_ms() - collect_start;
  std::printf("TEST ALLOC\nOBJECTS %lld\nALLOC_MS %.3f\nLIVE %llu\n"
              "COLLECT_MS %.3f\n",
              static_cast<long long>(count * 2), elapsed,
              static_cast<unsigned long long>(live), collect_ms);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "02";
  nrq_gc_mutator_enter();
  const uint64_t main_marker = nrq_gc_root_frame_begin();

  if (mode == "02") {
    run_mark_and_sweep(20, 5, 3, 5);
  } else if (mode == "02small") {
    // Same workload shape, 16x smaller heap (131,071 nodes, ~15 MB). Used only
    // to test whether per-object mark cost depends on heap working-set size.
    run_mark_and_sweep(16, 5, 3, 5);
  } else if (mode == "03") {
    run_cyclic(500000, 2, 5);
  } else if (mode == "alloc") {
    run_allocation(500000);
  } else if (mode == "01") {
    // Chain of 1 M nodes: same allocation sequence, then one traversal, then
    // one full collection of a fully live heap.
    nrq_gc_disable();
    nrq_gc_collect();
    const double build_start = now_ms();
    void* head = nullptr;
    {
      const uint64_t marker = nrq_gc_root_frame_begin();
      void* slot = nullptr;
      void* table[1] = {&slot};
      constexpr size_t descriptor[] = {1, 5, 8, 1, 0};
      nrq_gc_root_register_frame_static(table, 1, descriptor);
      for (int64_t index = 0; index < 1000000; ++index) {
        void* node = nrq_gc_allocate_array_typed_rooted(
            &slot, 1, sizeof(CycleNode), kCycleNodeLayout, 1);
        static_cast<CycleNode*>(node)->value = index;
        nrq_gc_root_store(reinterpret_cast<void**>(node), head);
        head = node;
      }
      nrq_gc_root_frame_end(marker);
    }
    const double build_ms = now_ms() - build_start;
    const double traverse_start = now_ms();
    int64_t sum = 0;
    for (void* p = head; p; p = static_cast<CycleNode*>(p)->next)
      sum += static_cast<CycleNode*>(p)->value;
    const double traverse_ms = now_ms() - traverse_start;
    std::printf("TEST 01\nBUILD_MS %.3f\nTRAVERSE_MS %.3f\nCHECKSUM %lld\n",
                build_ms, traverse_ms, static_cast<long long>(sum));
    std::vector<double> samples;
    for (int round = 0; round < 5; ++round) {
      const double start = now_ms();
      nrq_gc_collect();
      samples.push_back(now_ms() - start);
    }
    print_stats("COLLECT", summarize(samples));
  }

  nrq_gc_root_frame_end(main_marker);
  nrq_gc_mutator_leave();
  return 0;
}
