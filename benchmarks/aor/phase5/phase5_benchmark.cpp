// CDMM Phase 5 arena and pool benchmark.
//
// Compares the two explicit deterministic strategies against ordinary owned
// allocation on comparable workloads. Every strategy runs the same shape:
//
//   uniform     N fixed-size objects allocated, then released together
//   mixed-size  N objects cycling through 16..512 bytes (arena only; a pool is
//               one size by definition)
//   churn       R rounds of a 500-object working set, released at round end
//
//   phase5_benchmark [objects] [runs]
//
// Prints one row per sample and a median row per workload. It reports timings and
// the strategy's own byte accounting; it does not compute a derived comparison.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/cdmm.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kUniformBytes = 64;
constexpr size_t kChurnObjects = 500;

struct Sample {
  double allocate_ns = 0;
  double release_ns = 0;
  double allocated_bytes = 0;   // payload bytes per object
  double metadata_bytes = 0;    // strategy metadata per object, 0 when none
  size_t chunks = 0;            // arena chunks or pool chunks committed
  size_t committed_bytes = 0;
};

double ns_per_op(Clock::duration elapsed, size_t operations) {
  if (operations == 0) return 0;
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(operations);
}

size_t mixed_size(size_t index) {
  static const size_t sizes[] = {16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512};
  return sizes[index % (sizeof(sizes) / sizeof(sizes[0]))];
}

// --- ordinary owned allocation ---------------------------------------------

Sample run_owned(size_t count) {
  Sample sample;
  std::vector<void*> objects(count, nullptr);
  const auto allocate_start = Clock::now();
  for (size_t index = 0; index < count; ++index) {
    objects[index] = nrq_memory_allocate(kUniformBytes, 8);
    if (!objects[index]) {
      std::fprintf(stderr, "owned allocation failed at %zu\n", index);
      std::exit(2);
    }
  }
  const auto allocate_end = Clock::now();
  for (size_t index = 0; index < count; ++index)
    nrq_memory_deallocate_checked(objects[index]);
  const auto release_end = Clock::now();
  sample.allocate_ns = ns_per_op(allocate_end - allocate_start, count);
  sample.release_ns = ns_per_op(release_end - allocate_end, count);
  sample.allocated_bytes = static_cast<double>(kUniformBytes);
  // The ordinary allocator's registry record is not exposed as a per-object
  // figure, so this stays zero rather than guessing.
  sample.metadata_bytes = 0;
  return sample;
}

Sample run_owned_churn(size_t rounds) {
  Sample sample;
  const size_t count = rounds * kChurnObjects;
  std::vector<void*> objects(kChurnObjects, nullptr);
  const auto allocate_start = Clock::now();
  for (size_t round = 0; round < rounds; ++round) {
    for (size_t index = 0; index < kChurnObjects; ++index)
      objects[index] = nrq_memory_allocate(kUniformBytes, 8);
    for (size_t index = 0; index < kChurnObjects; ++index)
      nrq_memory_deallocate_checked(objects[index]);
  }
  const auto release_end = Clock::now();
  sample.allocate_ns = 0;
  sample.release_ns = ns_per_op(release_end - allocate_start, count);
  sample.allocated_bytes = static_cast<double>(kUniformBytes);
  return sample;
}

// --- arena ------------------------------------------------------------------

Sample run_arena(size_t count) {
  Sample sample;
  void* arena = nrq_cdmm_arena_create(64 * 1024, 0);
  if (!arena) {
    std::fprintf(stderr, "arena creation failed\n");
    std::exit(2);
  }
  std::vector<void*> objects(count, nullptr);
  NrqCdmmArenaScope scope{};
  if (!nrq_cdmm_arena_begin(arena, &scope)) {
    std::fprintf(stderr, "arena scope failed\n");
    std::exit(2);
  }
  const auto allocate_start = Clock::now();
  for (size_t index = 0; index < count; ++index) {
    objects[index] = nrq_cdmm_arena_scope_allocate(&scope, kUniformBytes, 8);
    if (!objects[index]) {
      std::fprintf(stderr, "arena allocation failed at %zu\n", index);
      std::exit(2);
    }
  }
  const auto allocate_end = Clock::now();
  nrq_cdmm_arena_end(&scope);
  // The peak, measured before the reset releases growth chunks: a reset that
  // shrank the arena would otherwise report capacity the workload never held.
  sample.committed_bytes = static_cast<size_t>(nrq_cdmm_arena_committed(arena));
  const auto release_start = Clock::now();
  if (!nrq_cdmm_arena_reset(arena)) {
    std::fprintf(stderr, "arena reset failed\n");
    std::exit(2);
  }
  const auto release_end = Clock::now();
  sample.allocate_ns = ns_per_op(allocate_end - allocate_start, count);
  sample.release_ns = ns_per_op(release_end - release_start, count);
  sample.allocated_bytes = static_cast<double>(kUniformBytes);
  sample.metadata_bytes = 0;  // no per-object metadata at all
  sample.chunks = (sample.committed_bytes + 64 * 1024 - 1) / (64 * 1024);
  if (!nrq_cdmm_arena_destroy(arena)) {
    std::fprintf(stderr, "arena destroy failed\n");
    std::exit(2);
  }
  return sample;
}

Sample run_arena_destructible(size_t count) {
  Sample sample;
  void* arena = nrq_cdmm_arena_create(64 * 1024, 0);
  if (!arena) {
    std::fprintf(stderr, "arena creation failed\n");
    std::exit(2);
  }
  static int destructor_calls = 0;
  destructor_calls = 0;
  std::vector<void*> objects(count, nullptr);
  NrqCdmmArenaScope scope{};
  if (!nrq_cdmm_arena_begin(arena, &scope)) std::exit(2);
  const auto allocate_start = Clock::now();
  for (size_t index = 0; index < count; ++index) {
    objects[index] = nrq_cdmm_arena_scope_allocate_destructible(
        &scope, kUniformBytes, 8,
        [](void*, void* context) { ++*static_cast<int*>(context); },
        &destructor_calls);
    if (!objects[index]) {
      std::fprintf(stderr, "arena destructible allocation failed at %zu\n", index);
      std::exit(2);
    }
  }
  const auto allocate_end = Clock::now();
  nrq_cdmm_arena_end(&scope);
  sample.committed_bytes = static_cast<size_t>(nrq_cdmm_arena_committed(arena));
  const auto release_start = Clock::now();
  if (!nrq_cdmm_arena_reset(arena)) std::exit(2);
  const auto release_end = Clock::now();
  if (destructor_calls != static_cast<int>(count)) {
    std::fprintf(stderr, "expected %zu destructor calls, saw %d\n", count,
                 destructor_calls);
    std::exit(2);
  }
  sample.allocate_ns = ns_per_op(allocate_end - allocate_start, count);
  sample.release_ns = ns_per_op(release_end - release_start, count);
  sample.allocated_bytes = static_cast<double>(kUniformBytes);
  // One 32-byte destructor record per object lives inside the arena.
  sample.metadata_bytes = 32;
  sample.chunks = (sample.committed_bytes + 64 * 1024 - 1) / (64 * 1024);
  nrq_cdmm_arena_destroy(arena);
  return sample;
}

Sample run_arena_mixed_sizes(size_t count) {
  Sample sample;
  void* arena = nrq_cdmm_arena_create(64 * 1024, 0);
  if (!arena) std::exit(2);
  std::vector<void*> objects(count, nullptr);
  std::vector<size_t> sizes(count);
  for (size_t index = 0; index < count; ++index) sizes[index] = mixed_size(index);
  NrqCdmmArenaScope scope{};
  if (!nrq_cdmm_arena_begin(arena, &scope)) std::exit(2);
  size_t total = 0;
  const auto allocate_start = Clock::now();
  for (size_t index = 0; index < count; ++index) {
    objects[index] = nrq_cdmm_arena_scope_allocate(&scope, sizes[index], 8);
    if (!objects[index]) {
      std::fprintf(stderr, "mixed arena allocation failed at %zu\n", index);
      std::exit(2);
    }
    total += sizes[index];
  }
  const auto allocate_end = Clock::now();
  nrq_cdmm_arena_end(&scope);
  sample.committed_bytes = static_cast<size_t>(nrq_cdmm_arena_committed(arena));
  const auto release_start = Clock::now();
  if (!nrq_cdmm_arena_reset(arena)) std::exit(2);
  const auto release_end = Clock::now();
  sample.allocate_ns = ns_per_op(allocate_end - allocate_start, count);
  sample.release_ns = ns_per_op(release_end - release_start, count);
  sample.allocated_bytes =
      static_cast<double>(total) / static_cast<double>(count);
  sample.chunks = (sample.committed_bytes + 64 * 1024 - 1) / (64 * 1024);
  nrq_cdmm_arena_destroy(arena);
  return sample;
}

Sample run_arena_churn(size_t rounds) {
  Sample sample;
  void* arena = nrq_cdmm_arena_create(64 * 1024, 0);
  if (!arena) std::exit(2);
  const size_t count = rounds * kChurnObjects;
  const auto allocate_start = Clock::now();
  for (size_t round = 0; round < rounds; ++round) {
    NrqCdmmArenaScope scope{};
    if (!nrq_cdmm_arena_begin(arena, &scope)) std::exit(2);
    for (size_t index = 0; index < kChurnObjects; ++index) {
      if (!nrq_cdmm_arena_scope_allocate(&scope, kUniformBytes, 8)) std::exit(2);
    }
    nrq_cdmm_arena_end(&scope);
    // A frame ends; the arena is reused for the next one. Reset is refused while
    // a scope pins the arena, which is why the scope closes first.
    if (!nrq_cdmm_arena_reset(arena)) std::exit(2);
  }
  const auto release_end = Clock::now();
  sample.release_ns = ns_per_op(release_end - allocate_start, count);
  sample.allocated_bytes = static_cast<double>(kUniformBytes);
  sample.committed_bytes = static_cast<size_t>(nrq_cdmm_arena_committed(arena));
  nrq_cdmm_arena_destroy(arena);
  return sample;
}

// --- pool -------------------------------------------------------------------

Sample run_pool(size_t count, bool generations) {
  Sample sample;
  const uint32_t flags = generations ? NRQ_CDMM_POOL_GENERATIONS : 0;
  void* pool = nrq_cdmm_pool_create(kUniformBytes, 8, 0, flags, nullptr, nullptr);
  if (!pool) {
    std::fprintf(stderr, "pool creation failed\n");
    std::exit(2);
  }
  std::vector<void*> slots(count, nullptr);
  NrqCdmmPoolScope scope{};
  if (!nrq_cdmm_pool_begin(pool, &scope)) std::exit(2);
  const auto allocate_start = Clock::now();
  for (size_t index = 0; index < count; ++index) {
    slots[index] = nrq_cdmm_pool_scope_allocate(&scope);
    if (!slots[index]) {
      std::fprintf(stderr, "pool allocation failed at %zu\n", index);
      std::exit(2);
    }
  }
  const auto allocate_end = Clock::now();
  const auto release_start = Clock::now();
  const size_t released =
      nrq_cdmm_pool_scope_release_bulk(&scope, slots.data(), slots.size());
  const auto release_end = Clock::now();
  nrq_cdmm_pool_end(&scope);
  if (released != count) {
    std::fprintf(stderr, "pool released %zu of %zu\n", released, count);
    std::exit(2);
  }
  sample.allocate_ns = ns_per_op(allocate_end - allocate_start, count);
  sample.release_ns = ns_per_op(release_end - release_start, count);
  sample.allocated_bytes = static_cast<double>(kUniformBytes);
  // Describe a live slot before releasing everything: the figure is the pool's
  // own per-slot metadata, which is a liveness bit plus the optional generation.
  NrqCdmmFacts facts{};
  if (nrq_cdmm_object_facts(slots[0], &facts)) {
    sample.metadata_bytes = static_cast<double>(facts.metadata_bytes);
  }
  sample.committed_bytes = static_cast<size_t>(nrq_cdmm_pool_committed(pool));
  sample.chunks = (sample.committed_bytes + 64 * 1024 - 1) / (64 * 1024);
  nrq_cdmm_pool_destroy(pool);
  return sample;
}

// Bulk release: every slot is reclaimed in one call. This is the pool analogue of
// an arena reset and does not run per-slot destructors unless one is registered.
Sample run_pool_reset(size_t count) {
  Sample sample;
  void* pool = nrq_cdmm_pool_create(kUniformBytes, 8, 0, 0, nullptr, nullptr);
  if (!pool) std::exit(2);
  std::vector<void*> slots(count, nullptr);
  NrqCdmmPoolScope scope{};
  if (!nrq_cdmm_pool_begin(pool, &scope)) std::exit(2);
  for (size_t index = 0; index < count; ++index) {
    slots[index] = nrq_cdmm_pool_scope_allocate(&scope);
    if (!slots[index]) std::exit(2);
  }
  nrq_cdmm_pool_end(&scope);
  sample.committed_bytes = static_cast<size_t>(nrq_cdmm_pool_committed(pool));
  const auto release_start = Clock::now();
  if (!nrq_cdmm_pool_reset(pool)) std::exit(2);
  const auto release_end = Clock::now();
  if (nrq_cdmm_pool_live(pool) != 0) std::exit(2);
  sample.allocate_ns = 0;
  sample.release_ns = ns_per_op(release_end - release_start, count);
  sample.allocated_bytes = static_cast<double>(kUniformBytes);
  sample.metadata_bytes = 0;
  sample.chunks = (sample.committed_bytes + 64 * 1024 - 1) / (64 * 1024);
  nrq_cdmm_pool_destroy(pool);
  return sample;
}

Sample run_pool_churn(size_t rounds) {
  Sample sample;
  void* pool = nrq_cdmm_pool_create(kUniformBytes, 8, 0,
                                    NRQ_CDMM_POOL_GENERATIONS, nullptr, nullptr);
  if (!pool) std::exit(2);
  const size_t count = rounds * kChurnObjects;
  std::vector<void*> slots(kChurnObjects, nullptr);
  std::vector<size_t> order(kChurnObjects);
  for (size_t index = 0; index < kChurnObjects; ++index) order[index] = index;
  uint64_t seed = 0x9e3779b97f4a7c15ull;
  NrqCdmmPoolScope scope{};
  if (!nrq_cdmm_pool_begin(pool, &scope)) std::exit(2);
  const auto allocate_start = Clock::now();
  for (size_t round = 0; round < rounds; ++round) {
    for (size_t index = 0; index < kChurnObjects; ++index) {
      slots[index] = nrq_cdmm_pool_scope_allocate(&scope);
      if (!slots[index]) std::exit(2);
    }
    // Release in a scrambled order, so reuse does not follow the free order.
    for (size_t index = kChurnObjects; index-- > 0;) {
      seed ^= seed << 13;
      seed ^= seed >> 7;
      seed ^= seed << 17;
      std::swap(order[index], order[seed % kChurnObjects]);
    }
    for (size_t index = 0; index < kChurnObjects; ++index)
      nrq_cdmm_pool_scope_release(&scope, slots[order[index]]);
  }
  const auto release_end = Clock::now();
  nrq_cdmm_pool_end(&scope);
  sample.release_ns = ns_per_op(release_end - allocate_start, count);
  sample.allocated_bytes = static_cast<double>(kUniformBytes);
  sample.metadata_bytes = 5;
  sample.committed_bytes = static_cast<size_t>(nrq_cdmm_pool_committed(pool));
  nrq_cdmm_pool_destroy(pool);
  return sample;
}

struct Workload {
  std::string name;
  Sample (*run)(size_t);
};

void print_row(const char* name, size_t count, const Sample& sample) {
  std::printf("%-22s %9zu %10.2f %10.2f %10.1f %8.1f %8zu %10zu\n", name, count,
              sample.allocate_ns, sample.release_ns, sample.allocated_bytes,
              sample.metadata_bytes, sample.chunks, sample.committed_bytes);
}

}  // namespace

int main(int argc, char** argv) {
  size_t count = 200000;
  size_t runs = 5;
  if (argc > 1) count = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));
  if (argc > 2) runs = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));
  if (count < 2 || runs == 0) {
    std::fprintf(stderr, "usage: phase5_benchmark [objects>=2] [runs>=1]\n");
    return 1;
  }
  std::printf("CDMM Phase 5 benchmark: %zu objects, %zu runs\n", count, runs);
  std::printf("%-22s %9s %10s %10s %10s %8s %8s %10s\n", "workload", "objects",
              "alloc ns", "release ns", "bytes/obj", "meta b/o", "chunks",
              "committed");

  const Workload workloads[] = {
      {"owned-64B", &run_owned},
      {"owned-churn-500", [](size_t objects) { return run_owned_churn(std::max<size_t>(1, objects / kChurnObjects)); }},
      {"arena-64B", &run_arena},
      {"arena-destructible", &run_arena_destructible},
      {"arena-mixed-size", &run_arena_mixed_sizes},
      {"arena-churn-500", [](size_t objects) { return run_arena_churn(std::max<size_t>(1, objects / kChurnObjects)); }},
      {"pool-64B", [](size_t objects) { return run_pool(objects, false); }},
      {"pool-generations", [](size_t objects) { return run_pool(objects, true); }},
      {"pool-reset-64B", &run_pool_reset},
      {"pool-churn-500", [](size_t objects) { return run_pool_churn(std::max<size_t>(1, objects / kChurnObjects)); }},
  };

  for (const Workload& workload : workloads) {
    std::vector<Sample> samples;
    samples.reserve(runs);
    for (size_t run = 0; run < runs; ++run)
      samples.push_back(workload.run(count));
    for (const Sample& sample : samples) print_row(workload.name.c_str(), count, sample);
    auto median = [&](double Sample::*member) {
      std::vector<double> values;
      values.reserve(samples.size());
      for (const Sample& sample : samples) values.push_back(sample.*member);
      std::sort(values.begin(), values.end());
      return values[values.size() / 2];
    };
    Sample combined;
    combined.allocate_ns = median(&Sample::allocate_ns);
    combined.release_ns = median(&Sample::release_ns);
    combined.allocated_bytes = median(&Sample::allocated_bytes);
    combined.metadata_bytes = median(&Sample::metadata_bytes);
    combined.chunks = samples.front().chunks;
    combined.committed_bytes = samples.front().committed_bytes;
    std::printf("%-22s %9s %10.2f %10.2f %10.1f %8.1f %8zu %10zu\n",
                (workload.name + "-median").c_str(), "median",
                combined.allocate_ns, combined.release_ns,
                combined.allocated_bytes, combined.metadata_bytes,
                combined.chunks, combined.committed_bytes);
  }
  return 0;
}
