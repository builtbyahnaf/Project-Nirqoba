// Phase 9 optimization and stabilization benchmark harness.
//
// Diagnostic only: uses the public runtime C ABI, matching the entry points the
// compiler emits. Each mode is a fresh process launched by run_benchmark.py.
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/gc.h"

namespace {

using Clock = std::chrono::steady_clock;

double now_ns() {
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 Clock::now().time_since_epoch())
                                 .count());
}

struct Node {
  int64_t value;
  int64_t payload[3];
};

uint64_t peak_rss_bytes() {
  PROCESS_MEMORY_COUNTERS counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return 0;
  return static_cast<uint64_t>(counters.PeakWorkingSetSize);
}

uint64_t current_rss_bytes() {
  PROCESS_MEMORY_COUNTERS counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return 0;
  return static_cast<uint64_t>(counters.WorkingSetSize);
}

void print_result(const char* mode, uint64_t operations, double elapsed_ns,
                  uint64_t checksum, const char* extra = "") {
  std::printf(
      "{\"mode\":\"%s\",\"operations\":%llu,\"elapsed_ns\":%.0f,"
      "\"ns_per_op\":%.3f,\"checksum\":%llu,\"peak_rss_bytes\":%llu,"
      "\"current_rss_bytes\":%llu%s}\n",
      mode, static_cast<unsigned long long>(operations), elapsed_ns,
      operations == 0 ? 0.0 : elapsed_ns / static_cast<double>(operations),
      static_cast<unsigned long long>(checksum),
      static_cast<unsigned long long>(peak_rss_bytes()),
      static_cast<unsigned long long>(current_rss_bytes()), extra);
}

void owned_alloc(uint64_t count) {
  const double start = now_ns();
  uint64_t checksum = 0;
  for (uint64_t index = 0; index < count; ++index) {
    auto* node = static_cast<Node*>(nrq_memory_allocate(sizeof(Node), alignof(Node)));
    if (!node) { std::printf("{\"error\":\"owned allocation failed\"}\n"); return; }
    node->value = static_cast<int64_t>(index);
    checksum += static_cast<uint64_t>(node->value);
    nrq_memory_deallocate_checked(node);
  }
  print_result("owned_alloc", count, now_ns() - start, checksum);
}

void owned_retained(uint64_t count) {
  std::vector<Node*> nodes;
  nodes.reserve(static_cast<size_t>(count));
  uint64_t checksum = 0;
  const double start = now_ns();
  for (uint64_t index = 0; index < count; ++index) {
    auto* node = static_cast<Node*>(nrq_memory_allocate(sizeof(Node), alignof(Node)));
    if (!node) { std::printf("{\"error\":\"owned retained allocation failed\"}\n"); return; }
    node->value = static_cast<int64_t>(index);
    nodes.push_back(node);
    checksum += static_cast<uint64_t>(node->value);
  }
  const double alloc_ns = now_ns() - start;
  print_result("owned_retained", count, alloc_ns, checksum);
  const double free_start = now_ns();
  for (Node* node : nodes) nrq_memory_deallocate_checked(node);
  print_result("owned_release", count, now_ns() - free_start, checksum);
}

void manual_alloc(uint64_t count) {
  const double start = now_ns();
  uint64_t checksum = 0;
  for (uint64_t index = 0; index < count; ++index) {
    auto* node = static_cast<Node*>(nrq_memory_allocate(sizeof(Node), alignof(Node)));
    if (!node) { std::printf("{\"error\":\"manual allocation failed\"}\n"); return; }
    node->value = static_cast<int64_t>(index);
    checksum += static_cast<uint64_t>(node->value);
    nrq_memory_deallocate_checked(node);
  }
  print_result("manual_alloc", count, now_ns() - start, checksum);
}

void borrowed_read(uint64_t reads) {
  constexpr size_t kCount = 4096;
  auto* values = static_cast<int64_t*>(
      nrq_memory_allocate(kCount * sizeof(int64_t), alignof(int64_t)));
  if (!values) { std::printf("{\"error\":\"borrow allocation failed\"}\n"); return; }
  for (size_t index = 0; index < kCount; ++index)
    values[index] = static_cast<int64_t>(index);
  uint64_t checksum = 0;
  const uint64_t rounds = reads / kCount;
  const double start = now_ns();
  for (uint64_t round = 0; round < rounds; ++round)
    for (size_t index = 0; index < kCount; ++index) checksum += values[index];
  const double elapsed = now_ns() - start;
  nrq_memory_deallocate_checked(values);
  print_result("borrowed_read", rounds * kCount, elapsed, checksum);
}

void arena_alloc(uint64_t count, uint64_t reset_rounds) {
  void* arena = nrq_allocator_create(NRQ_ALLOCATOR_ARENA, 8 << 20);
  if (!arena) { std::printf("{\"error\":\"arena creation failed\"}\n"); return; }
  uint64_t checksum = 0;
  const double start = now_ns();
  for (uint64_t index = 0; index < count; ++index) {
    auto* value = static_cast<int64_t*>(
        nrq_arena_allocate(arena, sizeof(int64_t), alignof(int64_t)));
    if (!value) { std::printf("{\"error\":\"arena allocation failed\"}\n"); return; }
    *value = static_cast<int64_t>(index);
    checksum += static_cast<uint64_t>(*value);
  }
  const double alloc_ns = now_ns() - start;
  print_result("arena_alloc", count, alloc_ns, checksum);
  const double reset_start = now_ns();
  for (uint64_t round = 0; round < reset_rounds; ++round)
    nrq_arena_reset(arena);
  print_result("arena_reset", reset_rounds, now_ns() - reset_start, checksum);
  nrq_arena_destroy(arena);
}

void pool_alloc(uint64_t count) {
  void* pool = nrq_allocator_create(NRQ_ALLOCATOR_POOL, 64);
  if (!pool) { std::printf("{\"error\":\"pool creation failed\"}\n"); return; }
  uint64_t checksum = 0;
  const double start = now_ns();
  for (uint64_t index = 0; index < count; ++index) {
    auto* value = static_cast<int64_t*>(
        nrq_memory_allocate_from(pool, sizeof(int64_t), alignof(int64_t)));
    if (!value) { std::printf("{\"error\":\"pool allocation failed\"}\n"); return; }
    *value = static_cast<int64_t>(index);
    checksum += static_cast<uint64_t>(*value);
    nrq_memory_deallocate_checked(value);
  }
  print_result("pool_alloc", count, now_ns() - start, checksum);
  nrq_allocator_destroy(pool);
}

void shared_alloc(uint64_t count, int retains) {
  const double start = now_ns();
  uint64_t checksum = 0;
  for (uint64_t index = 0; index < count; ++index) {
    auto* value = static_cast<int64_t*>(
        nrq_memory_shared_allocate(sizeof(int64_t), alignof(int64_t)));
    if (!value) { std::printf("{\"error\":\"shared allocation failed\"}\n"); return; }
    *value = static_cast<int64_t>(index);
    for (int retain = 0; retain < retains; ++retain)
      nrq_memory_shared_retain(value);
    checksum += static_cast<uint64_t>(*value);
    for (int release = 0; release < retains; ++release)
      nrq_memory_shared_release(value);
    nrq_memory_shared_release(value);
  }
  print_result("shared_alloc", count, now_ns() - start, checksum);
}

void shared_contention(int threads, uint64_t per_thread) {
  auto* value = static_cast<int64_t*>(
      nrq_memory_shared_allocate(sizeof(int64_t), alignof(int64_t)));
  if (!value) { std::printf("{\"error\":\"shared allocation failed\"}\n"); return; }
  *value = 7;
  std::atomic<uint64_t> checksum{0};
  std::vector<std::thread> workers;
  const double start = now_ns();
  for (int worker = 0; worker < threads; ++worker) {
    workers.emplace_back([&]() {
      uint64_t local = 0;
      for (uint64_t index = 0; index < per_thread; ++index) {
        void* retained = nrq_memory_shared_retain(value);
        local += *static_cast<int64_t*>(retained);
        nrq_memory_shared_release(retained);
      }
      checksum.fetch_add(local, std::memory_order_relaxed);
    });
  }
  for (std::thread& worker : workers) worker.join();
  const double elapsed = now_ns() - start;
  nrq_memory_shared_release(value);
  print_result("shared_contention", static_cast<uint64_t>(threads) * per_thread,
               elapsed, checksum.load());
}

void gc_alloc(uint64_t count) {
  nrq_gc_disable();
  nrq_gc_collect();
  uint64_t checksum = 0;
  const double start = now_ns();
  for (uint64_t index = 0; index < count; ++index) {
    auto* node = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), nullptr));
    if (!node) { std::printf("{\"error\":\"managed allocation failed\"}\n"); return; }
    node->value = static_cast<int64_t>(index);
    checksum += static_cast<uint64_t>(node->value);
    nrq_gc_unpin(node);
  }
  const double alloc_ns = now_ns() - start;
  const uint64_t before = nrq_gc_get_live_objects();
  const double collect_start = now_ns();
  nrq_gc_collect();
  const double collect_ns = now_ns() - collect_start;
  const uint64_t after = nrq_gc_get_live_objects();
  char extra[160];
  std::snprintf(extra, sizeof(extra),
                ",\"live_before\":%llu,\"live_after\":%llu",
                static_cast<unsigned long long>(before),
                static_cast<unsigned long long>(after));
  print_result("gc_alloc", count, alloc_ns, checksum, extra);
  print_result("gc_collect", 1, collect_ns, checksum, extra);
  nrq_gc_enable();
}

void mixed(uint64_t rounds) {
  void* arena = nrq_allocator_create(NRQ_ALLOCATOR_ARENA, 4 << 20);
  void* pool = nrq_allocator_create(NRQ_ALLOCATOR_POOL, 64);
  uint64_t checksum = 0;
  const double start = now_ns();
  for (uint64_t index = 0; index < rounds; ++index) {
    auto* owned = static_cast<int64_t*>(
        nrq_memory_allocate(sizeof(int64_t), alignof(int64_t)));
    *owned = static_cast<int64_t>(index);
    checksum += static_cast<uint64_t>(*owned);
    nrq_memory_deallocate_checked(owned);

    auto* arena_value = static_cast<int64_t*>(
        nrq_arena_allocate(arena, sizeof(int64_t), alignof(int64_t)));
    *arena_value = static_cast<int64_t>(index);
    checksum += static_cast<uint64_t>(*arena_value);

    auto* pooled = static_cast<int64_t*>(
        nrq_memory_allocate_from(pool, sizeof(int64_t), alignof(int64_t)));
    *pooled = static_cast<int64_t>(index);
    checksum += static_cast<uint64_t>(*pooled);
    nrq_memory_deallocate_checked(pooled);

    auto* shared = static_cast<int64_t*>(
        nrq_memory_shared_allocate(sizeof(int64_t), alignof(int64_t)));
    *shared = static_cast<int64_t>(index);
    checksum += static_cast<uint64_t>(*shared);
    nrq_memory_shared_release(shared);

    auto* manual = static_cast<int64_t*>(
        nrq_memory_allocate(sizeof(int64_t), alignof(int64_t)));
    *manual = static_cast<int64_t>(index);
    checksum += static_cast<uint64_t>(*manual);
    nrq_memory_deallocate_checked(manual);
  }
  print_result("mixed", rounds * 5, now_ns() - start, checksum);
  nrq_arena_destroy(arena);
  nrq_allocator_destroy(pool);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  if (mode == "owned" || mode == "all") {
    owned_alloc(1000000);
    owned_retained(200000);
    manual_alloc(1000000);
    borrowed_read(4096000);
  }
  if (mode == "arena" || mode == "all") arena_alloc(1000000, 1000);
  if (mode == "pool" || mode == "all") pool_alloc(1000000);
  if (mode == "shared" || mode == "all") {
    shared_alloc(200000, 8);
    shared_contention(4, 250000);
  }
  if (mode == "gc" || mode == "all") gc_alloc(500000);
  if (mode == "mixed" || mode == "all") mixed(200000);
  return 0;
}
