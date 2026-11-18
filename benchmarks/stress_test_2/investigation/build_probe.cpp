// Replica of the two stress-2 build phases, using the runtime entry points the
// compiler emits for them. Diagnostic only: it is not part of the measured suite
// and does not write any benchmark result file.
//
// The replicas exist to attribute the build phases to individual calls and to
// compare the emitted sequence against the alternative lowering a record
// allocation would produce, without rebuilding the compiler for each variant.
#define NOMINMAX
#include "nirqoba/runtime/gc.h"
#include "nirqoba/runtime/alloc_hint.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>

namespace {

double now_ms() {
  static LARGE_INTEGER frequency{};
  static bool initialized = false;
  if (!initialized) {
    QueryPerformanceFrequency(&frequency);
    initialized = true;
  }
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

struct TreeNode {
  void* left;
  void* right;
  int64_t value;
};

struct CycleNode {
  void* next;
  int64_t value;
};

// The two managed-reference offsets of TreeNode, as the compiler emits them.
constexpr size_t kTreeOffsets[2] = {0, 8};
constexpr size_t kCycleOffsets[1] = {0};

// Same layout the compiler emits: array of one element, extended representation.
constexpr uint32_t kArrayExtended =
    NRQ_ALLOC_PLAN_ARRAY | NRQ_ALLOC_PLAN_REPR_BITS(NRQ_ALLOC_REPR_EXTENDED);
// The plan a record allocation of the same payload would produce.
constexpr uint32_t kRecordHeaderless =
    NRQ_ALLOC_PLAN_HAS_REFERENCES |
    NRQ_ALLOC_PLAN_REPR_BITS(NRQ_ALLOC_REPR_HEADERLESS);

void* g_tree_site = nullptr;
void* g_cycle_site = nullptr;

bool g_use_record = false;

// build_tree, exactly as emitted: one root frame with two slots, one planned
// allocation, a store into the frame slot, one store per child, and a pin
// transfer of the result.
// Attribution switches. Each is read once per process; the branch costs a load
// and is dwarfed by the call it guards, which is what makes the subtraction
// meaningful.
bool probe_off(const char* name) {
  static const bool no_root_store = getenv("PROBE_NO_ROOT_STORE") != nullptr;
  static const bool no_pin = getenv("PROBE_NO_PIN") != nullptr;
  static const bool no_frame = getenv("PROBE_NO_FRAME") != nullptr;
  static const bool no_alloc = getenv("PROBE_NO_ALLOC") != nullptr;
  if (name[6] == 'R') return no_root_store;
  if (name[6] == 'P') return no_pin;
  if (name[6] == 'F') return no_frame;
  return no_alloc;
}

void* build_tree(int depth, int64_t value) {
  void* slots[2] = {nullptr, nullptr};
  const uint64_t frame = probe_off("PROBE_NO_FRAME") ? 0
                                                     : nrq_gc_root_frame_begin();
  if (!probe_off("PROBE_NO_FRAME")) {
    nrq_gc_root_register(&slots[0]);
    nrq_gc_root_register(&slots[1]);
  }
  void* node = g_use_record
                   ? nrq_gc_allocate_planned(&slots[0], 24, kTreeOffsets, 2,
                                             &g_tree_site, kRecordHeaderless)
                   : nrq_gc_allocate_array_planned(&slots[0], 1, 24,
                                                   kTreeOffsets, 2,
                                                   &g_tree_site, kArrayExtended);
  if (!probe_off("PROBE_NO_ROOT_STORE")) nrq_gc_root_store(&slots[1], node);
  auto* typed = static_cast<TreeNode*>(node);
  typed->value = value;
  if (depth > 0) {
    void* left = build_tree(depth - 1, value * 2);
    if (!probe_off("PROBE_NO_PIN")) nrq_gc_unpin(left);
    if (!probe_off("PROBE_NO_ROOT_STORE"))
      nrq_gc_root_store(&static_cast<TreeNode*>(slots[1])->left, left);
    void* right = build_tree(depth - 1, value * 2 + 1);
    if (!probe_off("PROBE_NO_PIN")) nrq_gc_unpin(right);
    if (!probe_off("PROBE_NO_ROOT_STORE"))
      nrq_gc_root_store(&static_cast<TreeNode*>(slots[1])->right, right);
  }
  if (!probe_off("PROBE_NO_PIN")) nrq_gc_pin_transfer(slots[1]);
  if (!probe_off("PROBE_NO_FRAME")) nrq_gc_root_frame_end(frame);
  return slots[1];
}

// make_one_cycle, exactly as emitted: a frame with two slots, two planned
// allocations, two field stores, no managed return.
void make_one_cycle(int64_t index) {
  void* slots[2] = {nullptr, nullptr};
  const uint64_t frame = nrq_gc_root_frame_begin();
  nrq_gc_root_register(&slots[0]);
  nrq_gc_root_register(&slots[1]);
  void* first = g_use_record
                    ? nrq_gc_allocate_planned(&slots[0], 16, kCycleOffsets, 1,
                                              &g_cycle_site, kRecordHeaderless)
                    : nrq_gc_allocate_array_planned(
                          &slots[0], 1, 16, kCycleOffsets, 1, &g_cycle_site,
                          kArrayExtended);
  nrq_gc_root_store(&slots[1], first);
  void* second = g_use_record
                     ? nrq_gc_allocate_planned(&slots[1], 16, kCycleOffsets, 1,
                                               &g_cycle_site, kRecordHeaderless)
                     : nrq_gc_allocate_array_planned(
                           &slots[1], 1, 16, kCycleOffsets, 1, &g_cycle_site,
                           kArrayExtended);
  static_cast<CycleNode*>(first)->value = index;
  static_cast<CycleNode*>(second)->value = index + 1;
  nrq_gc_root_store(&static_cast<CycleNode*>(first)->next, second);
  nrq_gc_root_store(&static_cast<CycleNode*>(second)->next, first);
  nrq_gc_root_frame_end(frame);
}

volatile int64_t checksum;

double measure_tree(int depth) {
  const int64_t nodes = (int64_t{1} << (depth + 1)) - 1;
  double best = 0.0;
  for (int sample = 0; sample < 3; ++sample) {
    nrq_gc_collect();
    const double start = now_ms();
    void* root = build_tree(depth, 1);
    const double elapsed = now_ms() - start;
    checksum += static_cast<int64_t>(reinterpret_cast<uintptr_t>(root) & 1);
    if (best == 0.0 || elapsed < best) best = elapsed;
    printf("  tree sample %d: %.1f ms  %.2f ns/node\n", sample, elapsed,
           elapsed * 1.0e6 / static_cast<double>(nodes));
  }
  return best;
}

double measure_cycles(int64_t pairs) {
  double best = 0.0;
  for (int sample = 0; sample < 3; ++sample) {
    nrq_gc_collect();
    const double start = now_ms();
    for (int64_t i = 0; i < pairs; ++i) make_one_cycle(i * 2);
    const double elapsed = now_ms() - start;
    checksum += 1;
    if (best == 0.0 || elapsed < best) best = elapsed;
    printf("  cycle sample %d: %.1f ms  %.2f ns/node\n", sample, elapsed,
           elapsed * 1.0e6 / static_cast<double>(pairs * 2));
  }
  return best;
}

// Allocation alone: the same planned array call the tree makes, no frame, no
// stores, no recursion.
double measure_alloc_only(int64_t count) {
  void* slot = nullptr;
  nrq_gc_root_register(&slot);
  double best = 0.0;
  for (int sample = 0; sample < 3; ++sample) {
    nrq_gc_collect();
    const double start = now_ms();
    for (int64_t i = 0; i < count; ++i) {
      void* node = nrq_gc_allocate_array_planned(&slot, 1, 24, kTreeOffsets, 2,
                                                 &g_tree_site, kArrayExtended);
      static_cast<TreeNode*>(node)->value = i;
    }
    const double elapsed = now_ms() - start;
    if (best == 0.0 || elapsed < best) best = elapsed;
    printf("  alloc sample %d: %.1f ms  %.2f ns/obj\n", sample, elapsed,
           elapsed * 1.0e6 / static_cast<double>(count));
  }
  return best;
}

// The per-activation protocol without recursion: a frame, its two roots, one
// allocation, its stores, and the frame end.
double measure_flat_frame(int64_t count) {
  double best = 0.0;
  for (int sample = 0; sample < 3; ++sample) {
    nrq_gc_collect();
    const double start = now_ms();
    for (int64_t i = 0; i < count; ++i) {
      void* slots[2] = {nullptr, nullptr};
      const uint64_t frame = nrq_gc_root_frame_begin();
      nrq_gc_root_register(&slots[0]);
      nrq_gc_root_register(&slots[1]);
      void* node = nrq_gc_allocate_array_planned(&slots[0], 1, 24, kTreeOffsets,
                                                 2, &g_tree_site, kArrayExtended);
      nrq_gc_root_store(&slots[1], node);
      static_cast<TreeNode*>(node)->value = i;
      nrq_gc_pin_transfer(slots[1]);
      nrq_gc_root_frame_end(frame);
    }
    const double elapsed = now_ms() - start;
    if (best == 0.0 || elapsed < best) best = elapsed;
    printf("  flat sample %d: %.1f ms  %.2f ns/obj\n", sample, elapsed,
           elapsed * 1.0e6 / static_cast<double>(count));
  }
  return best;
}

// The phase split of a live collection over the tree the benchmark builds.
void measure_collect(int depth) {
  void* root = build_tree(depth, 1);
  nrq_gc_root_register(&root);
  const int64_t nodes = (int64_t{1} << (depth + 1)) - 1;
  for (int sample = 0; sample < 4; ++sample) {
    nrq_gc_reset_cycle_timing();
    const double start = now_ms();
    nrq_gc_collect();
    const double elapsed = now_ms() - start;
    const nrq_gc_cycle_timing timing = nrq_gc_get_cycle_timing();
    printf(
        "  collect %d: %.1f ms  prepare %.2f  mark %.2f  sweep %.2f  "
        "finish %.2f ms | mark %.2f ns/obj sweep %.2f ns/obj\n",
        sample, elapsed, timing.prepare_ns / 1.0e6, timing.mark_ns / 1.0e6,
        timing.sweep_ns / 1.0e6, timing.finish_ns / 1.0e6,
        static_cast<double>(timing.mark_ns) / static_cast<double>(nodes),
        static_cast<double>(timing.sweep_ns) / static_cast<double>(nodes));
  }
}

}  // namespace

int main(int argc, char** argv) {
  nrq_gc_disable();
  nrq_gc_mutator_enter();
  const char* mode = argc > 1 ? argv[1] : "tree_array";
  if (std::strcmp(mode, "record") == 0) g_use_record = true;

  if (std::strcmp(mode, "nogen") == 0) nrq_gc_set_generational(0);
  if (std::strcmp(mode, "tree") == 0 || std::strcmp(mode, "record") == 0 ||
      std::strcmp(mode, "nogen") == 0) {
    printf("%s: build_tree depth 20\n", mode);
    measure_tree(20);
  } else if (std::strcmp(mode, "collect") == 0) {
    printf("collect: phase split over a depth 20 tree\n");
    measure_collect(20);
  } else if (std::strcmp(mode, "alloc") == 0) {
    printf("alloc: 2097151 planned array allocations\n");
    measure_alloc_only(2097151);
  } else {
    printf("%s: 500000 two-node cycles\n", mode);
    measure_cycles(500000);
  }
  nrq_gc_mutator_leave();
  return checksum == 12345 ? 1 : 0;
}
