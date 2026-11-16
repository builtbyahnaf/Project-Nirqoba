// Phase 7 GC isolation and explicit-selection benchmark harness.
//
// Diagnostic only: this file is not part of the compiler or runtime build. It
// uses the public C ABI so every measurement follows the same allocation, root,
// safepoint and barrier entry points the compiler emits. Run through
// run_benchmark.py, which compiles it against nirqoba_runtime.lib and summarizes
// fresh-process samples.
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
};

struct CycleNode {
  void* next;
  int64_t value;
};

constexpr size_t kCycleLayout[] = {0};

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

std::string gc_fields() {
  const nrq_gc_stats stats = nrq_gc_get_stats();
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer),
                ",\"gc_allocated_bytes\":%llu,\"gc_collections\":%llu,"
                "\"gc_live_objects\":%llu,\"gc_pages\":%llu,"
                "\"gc_layouts\":%llu",
                static_cast<unsigned long long>(stats.allocated_bytes),
                static_cast<unsigned long long>(stats.collections),
                static_cast<unsigned long long>(stats.live_objects),
                static_cast<unsigned long long>(nrq_gc_page_count()),
                static_cast<unsigned long long>(nrq_gc_layout_count()));
  return buffer;
}

void managed_allocation(uint64_t count, int rounds) {
  nrq_gc_disable();
  nrq_gc_collect();
  const double start = now_ns();
  uint64_t checksum = 0;
  for (int round = 0; round < rounds; ++round) {
    for (uint64_t index = 0; index < count; ++index) {
      auto* node = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), nullptr));
      if (!node) {
        std::printf("{\"error\":\"managed allocation failed\"}\n");
        return;
      }
      node->value = static_cast<int64_t>(index);
      checksum += static_cast<uint64_t>(node->value);
      nrq_gc_unpin(node);
    }
  }
  const double elapsed = now_ns() - start;
  const std::string extra = gc_fields();
  print_result("managed_alloc", count * static_cast<uint64_t>(rounds), elapsed,
               checksum, extra.c_str());
  const double collect_start = now_ns();
  nrq_gc_collect();
  const double collect_ns = now_ns() - collect_start;
  std::printf("{\"mode\":\"managed_alloc_collect\",\"operations\":1,"
              "\"elapsed_ns\":%.0f,\"ns_per_op\":%.3f,\"checksum\":%llu%s}\n",
              collect_ns, collect_ns,
              static_cast<unsigned long long>(checksum), extra.c_str());
  nrq_gc_enable();
}

void root_registration(uint64_t count) {
  void* slot = nullptr;
  const double start = now_ns();
  for (uint64_t index = 0; index < count; ++index) {
    nrq_gc_root_register(&slot);
    nrq_gc_root_unregister(&slot);
  }
  print_result("root_register", count, now_ns() - start, 0);
}

void root_frames(uint64_t count) {
  const double start = now_ns();
  for (uint64_t index = 0; index < count; ++index) {
    const uint64_t marker = nrq_gc_root_frame_begin();
    nrq_gc_root_frame_end(marker);
  }
  print_result("root_frame", count, now_ns() - start, 0);
}

void safepoints(uint64_t count) {
  nrq_gc_mutator_enter();
  const double start = now_ns();
  for (uint64_t index = 0; index < count; ++index) nrq_gc_safepoint();
  const double elapsed = now_ns() - start;
  nrq_gc_mutator_leave();
  print_result("safepoint", count, elapsed, 0);
}

void write_barriers(uint64_t count) {
  nrq_gc_disable();
  nrq_gc_mutator_enter();
  void* slot = nullptr;
  void* object = nrq_gc_allocate(sizeof(Node), nullptr);
  nrq_gc_root_register(&slot);
  const double start = now_ns();
  for (uint64_t index = 0; index < count; ++index)
    nrq_gc_root_store(&slot, (index & 1) ? object : nullptr);
  const double elapsed = now_ns() - start;
  nrq_gc_root_unregister(&slot);
  nrq_gc_unpin(object);
  nrq_gc_mutator_leave();
  nrq_gc_enable();
  print_result("write_barrier", count, elapsed, 0);
}

void collection(uint64_t count) {
  nrq_gc_disable();
  nrq_gc_collect();
  for (uint64_t index = 0; index < count; ++index) {
    auto* node = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), nullptr));
    node->value = static_cast<int64_t>(index);
    nrq_gc_unpin(node);
  }
  const uint64_t before = nrq_gc_get_live_objects();
  const double start = now_ns();
  nrq_gc_collect();
  const double elapsed = now_ns() - start;
  const uint64_t after = nrq_gc_get_live_objects();
  char extra[128];
  std::snprintf(extra, sizeof(extra),
                ",\"live_before\":%llu,\"live_after\":%llu",
                static_cast<unsigned long long>(before),
                static_cast<unsigned long long>(after));
  print_result("collection", 1, elapsed, after, extra);
  nrq_gc_enable();
}

void cycles(uint64_t pairs) {
  nrq_gc_disable();
  nrq_gc_collect();
  const double start = now_ns();
  for (uint64_t index = 0; index < pairs; ++index) {
    void* first = nrq_gc_allocate_array_typed(1, sizeof(CycleNode), kCycleLayout, 1);
    void* second = nrq_gc_allocate_array_typed(1, sizeof(CycleNode), kCycleLayout, 1);
    if (!first || !second) {
      std::printf("{\"error\":\"cycle allocation failed\"}\n");
      return;
    }
    static_cast<CycleNode*>(first)->value = static_cast<int64_t>(index);
    static_cast<CycleNode*>(second)->value = static_cast<int64_t>(index);
    nrq_gc_root_store(&static_cast<CycleNode*>(first)->next, second);
    nrq_gc_root_store(&static_cast<CycleNode*>(second)->next, first);
    nrq_gc_unpin(first);
    nrq_gc_unpin(second);
  }
  const double elapsed = now_ns() - start;
  const uint64_t before = nrq_gc_get_live_objects();
  const double collect_start = now_ns();
  nrq_gc_collect();
  const double collect_ns = now_ns() - collect_start;
  char extra[128];
  std::snprintf(extra, sizeof(extra), ",\"live_before\":%llu,\"live_after\":%llu",
                static_cast<unsigned long long>(before),
                static_cast<unsigned long long>(nrq_gc_get_live_objects()));
  print_result("cycle_build", pairs * 2, elapsed, 0, extra);
  print_result("cycle_collect", 1, collect_ns, 0, extra);
  nrq_gc_enable();
}

void multithreaded(int threads, uint64_t per_thread) {
  nrq_gc_disable();
  nrq_gc_collect();
  std::atomic<uint64_t> allocated{0};
  std::atomic<uint64_t> checksum{0};
  std::vector<std::thread> workers;
  const double start = now_ns();
  for (int worker = 0; worker < threads; ++worker) {
    workers.emplace_back([&, worker]() {
      nrq_gc_mutator_enter();
      const uint64_t marker = nrq_gc_root_frame_begin();
      uint64_t local = 0;
      for (uint64_t index = 0; index < per_thread; ++index) {
        auto* node = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), nullptr));
        if (!node) break;
        node->value = static_cast<int64_t>(index + static_cast<uint64_t>(worker));
        local += static_cast<uint64_t>(node->value);
        nrq_gc_unpin(node);
        allocated.fetch_add(1, std::memory_order_relaxed);
      }
      checksum.fetch_add(local, std::memory_order_relaxed);
      nrq_gc_root_frame_end(marker);
      nrq_gc_mutator_leave();
    });
  }
  const uint64_t total = static_cast<uint64_t>(threads) * per_thread;
  uint64_t collections = 0;
  while (allocated.load(std::memory_order_relaxed) < total) {
    nrq_gc_collect();
    ++collections;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  for (std::thread& worker : workers) worker.join();
  const double elapsed = now_ns() - start;
  nrq_gc_collect();
  char extra[160];
  std::snprintf(extra, sizeof(extra),
                ",\"threads\":%d,\"collections\":%llu,\"allocated\":%llu,"
                "\"live_after\":%llu",
                threads, static_cast<unsigned long long>(collections),
                static_cast<unsigned long long>(allocated.load()),
                static_cast<unsigned long long>(nrq_gc_get_live_objects()));
  print_result("multithreaded", total, elapsed, checksum.load(), extra);
  nrq_gc_enable();
}

void isolation() {
  const nrq_gc_stats before = nrq_gc_get_stats();
  const size_t layouts = nrq_gc_layout_count();
  const size_t pages = nrq_gc_page_count();
  uint64_t checksum = 0;

  const double owned_start = now_ns();
  for (int index = 0; index < 500000; ++index) {
    auto* value = static_cast<int*>(nrq_memory_allocate(sizeof(int), alignof(int)));
    *value = index;
    checksum += static_cast<uint64_t>(*value);
    nrq_memory_deallocate_checked(value);
  }
  const double owned_ns = now_ns() - owned_start;

  auto* values = static_cast<int*>(nrq_memory_allocate_array(1024, sizeof(int), alignof(int)));
  const double borrowed_start = now_ns();
  for (int round = 0; round < 1000; ++round)
    for (int index = 0; index < 1024; ++index) checksum += static_cast<uint64_t>(values[index]);
  const double borrowed_ns = now_ns() - borrowed_start;
  nrq_memory_deallocate_checked(values);

  void* arena = nrq_allocator_create(NRQ_ALLOCATOR_ARENA, 1 << 20);
  const double arena_start = now_ns();
  for (int index = 0; index < 200000; ++index) {
    auto* value = static_cast<int*>(nrq_arena_allocate(arena, sizeof(int), alignof(int)));
    *value = index;
    checksum += static_cast<uint64_t>(*value);
  }
  const double arena_ns = now_ns() - arena_start;
  nrq_arena_reset(arena);
  nrq_arena_destroy(arena);

  void* pool = nrq_allocator_create(NRQ_ALLOCATOR_POOL, 64);
  const double pool_start = now_ns();
  for (int index = 0; index < 200000; ++index) {
    auto* value = static_cast<int*>(nrq_memory_allocate_from(pool, sizeof(int), alignof(int)));
    *value = index;
    checksum += static_cast<uint64_t>(*value);
    nrq_memory_deallocate_checked(value);
  }
  const double pool_ns = now_ns() - pool_start;
  nrq_allocator_destroy(pool);

  const double shared_start = now_ns();
  for (int index = 0; index < 200000; ++index) {
    auto* value = static_cast<int*>(nrq_memory_shared_allocate(sizeof(int), alignof(int)));
    *value = index;
    checksum += static_cast<uint64_t>(*value);
    nrq_memory_shared_release(value);
  }
  const double shared_ns = now_ns() - shared_start;

  const nrq_gc_stats after = nrq_gc_get_stats();
  const bool clean = before.allocated_bytes == after.allocated_bytes &&
                     before.collections == after.collections &&
                     before.live_objects == after.live_objects &&
                     layouts == nrq_gc_layout_count() && pages == nrq_gc_page_count();
  char extra[256];
  std::snprintf(extra, sizeof(extra),
                ",\"gc_clean\":%s,\"gc_allocated_before\":%llu,"
                "\"gc_allocated_after\":%llu,\"gc_collections_before\":%llu,"
                "\"gc_collections_after\":%llu,\"layouts_before\":%llu,"
                "\"layouts_after\":%llu",
                clean ? "true" : "false",
                static_cast<unsigned long long>(before.allocated_bytes),
                static_cast<unsigned long long>(after.allocated_bytes),
                static_cast<unsigned long long>(before.collections),
                static_cast<unsigned long long>(after.collections),
                static_cast<unsigned long long>(layouts),
                static_cast<unsigned long long>(nrq_gc_layout_count()));
  print_result("isolation_owned", 500000, owned_ns, checksum, extra);
  print_result("isolation_borrowed", 1024000, borrowed_ns, checksum, extra);
  print_result("isolation_arena", 200000, arena_ns, checksum, extra);
  print_result("isolation_pool", 200000, pool_ns, checksum, extra);
  print_result("isolation_shared", 200000, shared_ns, checksum, extra);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  if (mode == "managed" || mode == "all") managed_allocation(200000, 3);
  if (mode == "roots" || mode == "all") {
    root_registration(1000000);
    root_frames(1000000);
  }
  if (mode == "safepoint" || mode == "all") safepoints(5000000);
  if (mode == "barrier" || mode == "all") write_barriers(5000000);
  if (mode == "collect" || mode == "all") collection(500000);
  if (mode == "cycles" || mode == "all") cycles(100000);
  if (mode == "mt" || mode == "all") multithreaded(4, 100000);
  if (mode == "isolation" || mode == "all") isolation();
  return 0;
}
