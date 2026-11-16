// Shared-control contention benchmark. The language has no thread-spawn API,
// so this measures the runtime contract directly: one shard lock and one atomic
// count for each retain/release.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include "nirqoba/runtime/allocator.h"

namespace {
constexpr size_t kThreads = 4;
constexpr size_t kOpsPerThread = 250000;
constexpr size_t kFinalObjects = 100000;

uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct Result {
  uint64_t ns = 0;
  bool ok = true;
};

Result run_shared(bool independent) {
  std::vector<void*> objects(kThreads, nullptr);
  if (independent) {
    for (void*& object : objects)
      object = nrq_memory_shared_allocate(64, 16);
  } else {
    objects.assign(kThreads, nrq_memory_shared_allocate(64, 16));
  }
  for (void* object : objects)
    if (!object) return {0, false};
  std::atomic<bool> start{false};
  std::atomic<bool> ok{true};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (size_t index = 0; index < kThreads; ++index) {
    workers.emplace_back([&, index] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (size_t op = 0; op < kOpsPerThread; ++op) {
        if (nrq_memory_shared_retain(objects[index]) == nullptr ||
            nrq_memory_shared_release(objects[index]) != 1) {
          ok.store(false, std::memory_order_release);
          return;
        }
      }
    });
  }
  const uint64_t begin = now_ns();
  start.store(true, std::memory_order_release);
  for (std::thread& worker : workers) worker.join();
  const uint64_t elapsed = now_ns() - begin;
  for (size_t index = 0; index < kThreads; ++index) {
    if (independent) {
      if (nrq_memory_shared_release(objects[index]) != 1) ok.store(false);
    } else if (index == 0) {
      if (nrq_memory_shared_release(objects[index]) != 1) ok.store(false);
    }
  }
  return {elapsed, ok.load()};
}

SIZE_T working_set_bytes() {
  PROCESS_MEMORY_COUNTERS counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return 0;
  return counters.WorkingSetSize;
}

struct MemoryProbe {
  size_t objects = 100000;
  SIZE_T before = 0;
  SIZE_T after = 0;
  bool ok = true;
};

MemoryProbe measure_shared_overhead() {
  MemoryProbe probe;
  std::vector<void*> objects;
  objects.reserve(probe.objects);
  probe.before = working_set_bytes();
  for (size_t index = 0; index < probe.objects; ++index) {
    void* pointer = nrq_memory_shared_allocate(32, 16);
    if (!pointer) {
      probe.ok = false;
      break;
    }
    objects.push_back(pointer);
  }
  probe.after = working_set_bytes();
  for (void* object : objects)
    if (nrq_memory_shared_release(object) != 1) probe.ok = false;
  return probe;
}

Result run_uncontended() {
  void* object = nrq_memory_shared_allocate(64, 16);
  if (!object) return {0, false};
  bool ok = true;
  const uint64_t begin = now_ns();
  for (size_t op = 0; op < kOpsPerThread * kThreads; ++op) {
    if (nrq_memory_shared_retain(object) == nullptr ||
        nrq_memory_shared_release(object) != 1)
      ok = false;
  }
  const uint64_t elapsed = now_ns() - begin;
  if (nrq_memory_shared_release(object) != 1) ok = false;
  return {elapsed, ok};
}

Result run_final_release() {
  std::vector<void*> objects;
  objects.reserve(kFinalObjects);
  for (size_t index = 0; index < kFinalObjects; ++index) {
    void* object = nrq_memory_shared_allocate(64, 16);
    if (!object) return {0, false};
    objects.push_back(object);
  }
  bool ok = true;
  const uint64_t begin = now_ns();
  for (void* object : objects) {
    if (nrq_memory_shared_release(object) != 1) ok = false;
  }
  const uint64_t elapsed = now_ns() - begin;
  return {elapsed, ok};
}
}  // namespace

int main() {
  const MemoryProbe memory = measure_shared_overhead();
  const Result uncontended = run_uncontended();
  const Result independent = run_shared(true);
  const Result contended = run_shared(false);
  const Result final_release = run_final_release();
  const long long delta = static_cast<long long>(memory.after) -
                          static_cast<long long>(memory.before);
  const double bytes_per_object = memory.objects == 0
      ? 0.0
      : static_cast<double>(delta) / static_cast<double>(memory.objects);
  const bool ok = memory.ok && uncontended.ok && independent.ok && contended.ok &&
                  final_release.ok;
    std::printf("{\"uncontended_ns\":%llu,\"independent_ns\":%llu,"
              "\"contended_ns\":%llu,\"final_release_ns\":%llu,"
              "\"final_release_objects\":%zu,"
              "\"ops_per_thread\":%zu,\"threads\":%zu,"
              "\"memory_objects\":%zu,\"memory_delta_bytes\":%lld,"
              "\"memory_bytes_per_object\":%.2f,\"ok\":%s}\n",
              static_cast<unsigned long long>(uncontended.ns),
              static_cast<unsigned long long>(independent.ns),
              static_cast<unsigned long long>(contended.ns),
              static_cast<unsigned long long>(final_release.ns), kFinalObjects,
              kOpsPerThread, kThreads, memory.objects, delta, bytes_per_object,
              ok ? "true" : "false");
  return ok ? 0 : 1;
}
