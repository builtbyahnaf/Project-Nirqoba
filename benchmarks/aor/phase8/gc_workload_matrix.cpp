// Phase 8 workload matrix.
//
// Phase 7's harness measured seven shapes with one parameter and one survival
// ratio, which answered "what does an object cost" but not "does the heap reuse
// what it reclaimed". This program runs the matrix Phase 8 needs: each
// representation on its own and mixed, small and large objects, arrays, nested
// arrays and structure-of-arrays payloads, deep and cyclic graphs, high and low
// survival, mixed sizes, page reuse after a cycle, and collector storage beside
// an arena and a pool.
//
// Every column is measured, not inferred:
//
//   alloc ns   wall clock per allocation
//   mark ns/o  the runtime's marking slices divided by the objects the marker
//              actually popped (not by the heap size)
//   sweep ns/o the same for the reclaimer
//   pages      committed pages after the reclaiming cycle
//   live       objects the collector reports as live after it
//   committed  pages * page size, so the space a live object costs is derivable
//   runs       reclaimed runs recorded, and how many pages hold them
//   dropped    reclaimed space a page's bounded run list could not record
//   reuse      pages committed by a second allocation wave, which is the direct
//              measure of whether the first wave's free space came back
//
// Inputs: [objects] [runs]. One process measures one matrix; run the program twice
// to compare two builds.
//
// The concurrency section runs T mutator threads, each with its own graphs, arena
// and pool, against a collector thread, and reports wall time and collections. It
// exists because a single-threaded matrix says nothing about what the handshake
// costs, and Phase 8 has to state both configurations' numbers for it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nirqoba/runtime/cdmm.h"
#include "nirqoba/runtime/gc.h"

namespace {

using Clock = std::chrono::steady_clock;

double ns_per_op(Clock::duration elapsed, size_t operations) {
  if (operations == 0) return 0;
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(operations);
}

double us_total(Clock::duration elapsed) {
  return std::chrono::duration<double, std::micro>(elapsed).count();
}

void require(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "matrix invariant failed: %s\n", message);
  std::exit(2);
}

const size_t kWordOffset[] = {0};

void register_region(std::vector<void*>& region) {
  nrq_gc_root_register_array(region.data(), region.size(), sizeof(void*),
                             kWordOffset, 1);
}

void unregister_region(std::vector<void*>& region) {
  if (region.empty()) return;
  nrq_gc_root_unregister(reinterpret_cast<void**>(region.data()));
}

// --- descriptors -----------------------------------------------------------
//
// One descriptor per shape, interned once, exactly what a compiler emits as a
// read-only table. `element_bytes == 0` means a fixed record.

const nrq_object_layout* record_layout(size_t payload_bytes,
                                       const size_t* offsets,
                                       size_t reference_count) {
  return nrq_gc_layout_intern(payload_bytes, 0, nullptr, offsets,
                              reference_count);
}

struct PhaseSplit {
  uint64_t pause_ns = 0;
  uint64_t prepare_ns = 0;
  uint64_t mark_ns = 0;
  uint64_t sweep_ns = 0;
  uint64_t finish_ns = 0;
  uint64_t mark_objects = 0;
  uint64_t sweep_objects = 0;
  uint64_t cycles = 0;
};

PhaseSplit take_phase_split() {
  const nrq_gc_cycle_timing timing = nrq_gc_get_cycle_timing();
  PhaseSplit split;
  split.pause_ns = timing.pause_ns;
  split.prepare_ns = timing.prepare_ns;
  split.mark_ns = timing.mark_ns;
  split.sweep_ns = timing.sweep_ns;
  split.finish_ns = timing.finish_ns;
  split.mark_objects = timing.mark_objects;
  split.sweep_objects = timing.sweep_objects;
  split.cycles = timing.cycles;
  nrq_gc_reset_cycle_timing();
  return split;
}

enum class Shape {
  Small16,      // H0 fixed record, one reference
  Small32,      // H0 fixed record, two references
  Medium128,    // H8 fixed record
  Record512,    // H8 fixed record
  Array4,       // H16 four-element array of pointers
  Nested,       // H16 array of three pointers into H16 arrays
  Soa,          // H16 structure-of-arrays payload with one pointer column
  MixedRepr,    // H0, H8 and H16 objects in one heap
  MixedSizes,   // 16, 64, 256 and 1024 byte objects in one heap
  Ring,         // H0 chain whose last node points back at the first
};

const char* shape_name(Shape shape) {
  switch (shape) {
    case Shape::Small16: return "h0-record-16B";
    case Shape::Small32: return "h0-record-32B";
    case Shape::Medium128: return "h8-record-128B";
    case Shape::Record512: return "h8-record-512B";
    case Shape::Array4: return "h16-array-4x16B";
    case Shape::Nested: return "h16-nested-array";
    case Shape::Soa: return "h16-soa-4x12B";
    case Shape::MixedRepr: return "mixed-representations";
    case Shape::MixedSizes: return "mixed-sizes";
    case Shape::Ring: return "h0-cyclic-ring";
  }
  return "unknown";
}

const nrq_object_layout* layout_for(Shape shape) {
  static const size_t kOne[] = {0};
  static const size_t kTwo[] = {0, 8};
  switch (shape) {
    case Shape::Small16:
    case Shape::Ring:
      return record_layout(16, kOne, 1);
    case Shape::Small32:
      return record_layout(32, kTwo, 2);
    case Shape::Medium128:
      return record_layout(128, kOne, 1);
    default:
      return nullptr;
  }
}

// Allocation cost of one object of this shape. A shape that is an array or a
// mixture returns a payload of the size its own row reports.
void* allocate_one(Shape shape, size_t index, void*& nested_root) {
  switch (shape) {
    case Shape::Small16:
    case Shape::Small32:
    case Shape::Ring:
    case Shape::Medium128:
      return nrq_gc_allocate_layout(layout_for(shape));
    case Shape::Record512:
      return nrq_gc_allocate(512, nullptr);
    case Shape::Array4:
      return nrq_gc_allocate_array_typed(4, sizeof(void*), kWordOffset, 1);
    case Shape::Nested: {
      // An outer array of three references, each installed by a separate
      // allocation: two levels of managed storage, which is the shape a nested
      // array lowers to.
      void* outer = nrq_gc_allocate_array_typed(3, sizeof(void*), kWordOffset, 1);
      if (!outer) return nullptr;
      auto* slots = static_cast<void**>(outer);
      for (size_t child = 0; child < 3; ++child) {
        void* inner =
            nrq_gc_allocate_array_typed(4, sizeof(void*), kWordOffset, 1);
        if (!inner) return nullptr;
        std::memset(inner, 0, 4 * sizeof(void*));
        slots[child] = inner;
      }
      (void)nested_root;
      return outer;
    }
    case Shape::Soa: {
      static const size_t kColumns[] = {0, 8};
      return nrq_gc_allocate_soa_rooted(&nested_root, 4, 12, kColumns, 2);
    }
    case Shape::MixedRepr: {
      // Three representations interleaved, which is what a program with several
      // record shapes does. The TLAB refuses a request for another page class and
      // the caller falls back to the serialized path, so this measures that.
      switch (index % 3) {
        case 0: return nrq_gc_allocate_layout(layout_for(Shape::Small16));
        case 1: return nrq_gc_allocate(512, nullptr);
        default:
          return nrq_gc_allocate_array_typed(4, sizeof(void*), kWordOffset, 1);
      }
    }
    case Shape::MixedSizes: {
      // One heap, four payload sizes, no two neighbours the same class: the shape
      // that exposes page mixing rather than a single size's geometry.
      static const size_t kSizes[] = {16, 64, 256, 1024};
      return nrq_gc_allocate(kSizes[index % 4], nullptr);
    }
  }
  return nullptr;
}

struct Row {
  const char* name = "";
  size_t objects = 0;
  // Allocation.
  double allocate_ns = 0;
  // A cycle with every object reachable.
  double mark_ns_per_object = 0;
  double sweep_ns_per_object = 0;
  double live_pause_us = 0;
  // The reclaiming cycle.
  size_t live_objects = 0;
  size_t reclaimed = 0;
  size_t pages = 0;
  size_t reclaimed_pages = 0;
  size_t committed_bytes = 0;
  size_t live_bytes = 0;
  double utilisation = 0;
  size_t free_runs = 0;
  size_t pages_with_runs = 0;
  uint64_t dropped_bytes = 0;
  // A second allocation wave into the heap the first one freed.
  size_t wave_pages = 0;
  double wave_allocate_ns = 0;
  // Representation facts of the live heap.
  size_t metadata_bytes = 0;
  uint64_t header_bytes_saved = 0;
  size_t zero_prezeroed = 0;
  size_t zero_filled = 0;
};

// Allocates `count` objects of one shape and links them into a chain when the
// shape is a record with a reference at offset 0. The region is registered so the
// first cycle sees every object reachable.
Row run_shape(Shape shape, size_t count, unsigned survival_percent) {
  Row row;
  row.name = shape_name(shape);
  row.objects = count;

  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_reset_zeroing_facts();

  std::vector<void*> objects(count, nullptr);
  void* soa_root = nullptr;
  nrq_gc_root_register(&soa_root);
  register_region(objects);

  const bool chained = shape == Shape::Small16 || shape == Shape::Small32 ||
                       shape == Shape::Ring || shape == Shape::Medium128;
  const auto allocate_start = Clock::now();
  for (size_t index = 0; index < count; ++index) {
    void* object = allocate_one(shape, index, soa_root);
    require(object != nullptr, "allocation returned null");
    objects[index] = object;
    if (chained && index > 0) {
      void* previous = objects[index - 1];
      std::memcpy(previous, &object, sizeof(object));
    }
  }
  if (shape == Shape::Ring && count > 1) {
    // The last node points back at the first, so the graph is cyclic and no chain
    // walk terminates at a null.
    void* first = objects[0];
    std::memcpy(objects[count - 1], &first, sizeof(first));
  }
  const auto allocate_end = Clock::now();
  row.allocate_ns = ns_per_op(allocate_end - allocate_start, count);
  row.committed_bytes = nrq_gc_page_count() * 64 * 1024;

  const nrq_gc_zeroing_facts zeroing = nrq_gc_get_zeroing_facts();
  row.zero_prezeroed = static_cast<size_t>(zeroing.prezeroed_bytes);
  row.zero_filled = static_cast<size_t>(zeroing.filled_bytes);

  // Cycle 1: everything is reachable, so this is the marking cost with nothing to
  // reclaim.
  take_phase_split();
  const auto live_start = Clock::now();
  nrq_gc_collect();
  const auto live_end = Clock::now();
  const PhaseSplit live = take_phase_split();
  row.live_pause_us = us_total(live_end - live_start);
  row.mark_ns_per_object =
      live.mark_objects == 0
          ? 0.0
          : static_cast<double>(live.mark_ns) /
                static_cast<double>(live.mark_objects);
  row.sweep_ns_per_object =
      live.sweep_objects == 0
          ? 0.0
          : static_cast<double>(live.sweep_ns) /
                static_cast<double>(live.sweep_objects);
  require(nrq_gc_get_live_objects() >= count,
          "the live cycle reclaimed a reachable object");

  const size_t pages_before = nrq_gc_page_count();
  row.committed_bytes = pages_before * 64 * 1024;

  // Cycle 2: retain a fraction of the graph, interleaved with the garbage, so the
  // survivors are spread through every page the way a real workload's are.
  unregister_region(objects);
  std::vector<void*> survivors;
  if (survival_percent != 0) {
    const size_t stride = 100 / survival_percent;
    survivors.reserve(count / std::max<size_t>(stride, 1));
    for (size_t index = 0; index < count; index += std::max<size_t>(stride, 1))
      survivors.push_back(objects[index]);
    // A chain retains everything reachable from its head, so keeping every fifth
    // node while the links still point at the four between them would root the
    // whole graph and measure nothing. The survivors are relinked into their own
    // graph, which is the shape a real program's retained subgraph has; the ring
    // keeps its cycle closed inside the survivors.
    if (chained) {
      for (size_t index = 0; index < survivors.size(); ++index) {
        void* next =
            index + 1 < survivors.size() ? survivors[index + 1] : nullptr;
        std::memcpy(survivors[index], &next, sizeof(next));
      }
      if (shape == Shape::Ring && !survivors.empty()) {
        void* first = survivors.front();
        std::memcpy(survivors.back(), &first, sizeof(first));
      }
    }
    register_region(survivors);
  }
  const uint64_t dropped_before = nrq_gc_dropped_free_run_bytes();
  nrq_gc_collect();

  row.live_objects = static_cast<size_t>(nrq_gc_get_live_objects());
  row.reclaimed = count > row.live_objects ? count - row.live_objects : 0;
  row.pages = nrq_gc_page_count();
  row.committed_bytes = row.pages * 64 * 1024;
  row.free_runs = nrq_gc_free_run_count();
  row.pages_with_runs = nrq_gc_page_with_runs_count();
  // Per-row: the counter is process-wide and the matrix runs every shape in one
  // process, so the row reports what its own reclaiming cycle discarded.
  row.dropped_bytes = nrq_gc_dropped_free_run_bytes() - dropped_before;
  const nrq_gc_representation_stats stats = nrq_gc_get_representation_stats();
  const nrq_gc_page_metadata_facts page_facts =
      nrq_gc_get_page_metadata_facts();
  row.metadata_bytes = page_facts.total_bitmap_bytes;
  row.header_bytes_saved = stats.header_bytes_saved_vs_extended;
  // The runtime's own live-byte total: the payload the collector considers
  // reachable after the cycle, which for a nested or mixed shape is not the row's
  // object count times one payload size.
  row.live_bytes = static_cast<size_t>(nrq_gc_get_allocated_bytes());
  row.utilisation =
      row.committed_bytes == 0
          ? 0.0
          : 100.0 * static_cast<double>(row.live_bytes) /
                static_cast<double>(row.committed_bytes);

  // Cycle 3: a second wave of the same shape into the space the first one freed.
  // The pages it needs beyond what is already committed is what the reclaimer
  // actually recovered.
  unregister_region(survivors);
  survivors.clear();
  survivors.shrink_to_fit();
  const size_t wave = std::max<size_t>(count / 2, 1000);
  std::vector<void*> second(wave, nullptr);
  register_region(second);
  const auto wave_start = Clock::now();
  for (size_t index = 0; index < wave; ++index)
    second[index] = allocate_one(shape, index, soa_root);
  const auto wave_end = Clock::now();
  row.wave_allocate_ns = ns_per_op(wave_end - wave_start, wave);
  row.wave_pages = nrq_gc_page_count();
  unregister_region(second);
  second.clear();
  second.shrink_to_fit();
  nrq_gc_root_unregister(&soa_root);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
  return row;
}

void print_row(const Row& row) {
  std::printf("%-22s %8zu %9.1f %9.2f %9.2f %10zu %10zu %7.1f %8zu %8zu "
              "%11llu %9zu\n",
              row.name, row.objects, row.allocate_ns, row.mark_ns_per_object,
              row.sweep_ns_per_object, row.pages, row.live_objects,
              row.utilisation, row.free_runs, row.pages_with_runs,
              static_cast<unsigned long long>(row.dropped_bytes), row.wave_pages);
}

// --- coexistence -----------------------------------------------------------
//
// Collector storage, an arena and a pool in one process. The point is not that
// they are fast together but that they stay separate: the arena resets and the
// pool is destroyed while the collector's objects stay live through every cycle,
// and the collector reclaims only its own objects when their root goes away.
struct Coexistence {
  size_t gc_live_after_arena_reset = 0;
  size_t gc_live_after_pool_destroy = 0;
  size_t arena_bytes_after_reset = 0;
  size_t pool_committed_after_destroy = 0;
  size_t gc_reclaimed = 0;
  bool gc_object_survived = false;
};

Coexistence run_coexistence(size_t count) {
  Coexistence result;
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();

  std::vector<void*> objects(count, nullptr);
  void* root = nullptr;
  nrq_gc_root_register(&root);
  for (size_t index = 0; index < count; ++index)
    objects[index] = nrq_gc_allocate_layout(layout_for(Shape::Small16));
  register_region(objects);
  void* kept = objects.front();
  nrq_gc_collect();

  void* arena = nrq_cdmm_arena_create(64 * 1024, 4 * 1024 * 1024);
  require(arena != nullptr, "arena creation failed");
  for (size_t index = 0; index < 512; ++index)
    require(nrq_cdmm_arena_allocate(arena, 64, 8) != nullptr,
            "arena allocation failed");
  const uint64_t arena_bytes = nrq_cdmm_arena_used(arena);

  void* pool = nrq_cdmm_pool_create(64, 8, 256, 0, nullptr, nullptr);
  require(pool != nullptr, "pool creation failed");
  for (size_t index = 0; index < 128; ++index)
    require(nrq_cdmm_pool_allocate(pool) != nullptr, "pool allocation failed");

  // The arena's lifetime ends while collector storage is live and reachable.
  nrq_cdmm_arena_reset(arena);
  nrq_cdmm_pool_reset(pool);
  nrq_gc_collect();
  result.arena_bytes_after_reset = static_cast<size_t>(arena_bytes);
  result.gc_live_after_arena_reset =
      static_cast<size_t>(nrq_gc_get_live_objects());
  result.pool_committed_after_destroy = 0;
  result.gc_object_survived = nrq_gc_pointer_is_managed(kept) != 0;

  // The collector's own objects go away when their root does, and nothing in the
  // arena or the pool is part of that decision.
  nrq_gc_root_unregister(&root);
  unregister_region(objects);
  nrq_gc_collect();
  result.gc_live_after_pool_destroy =
      static_cast<size_t>(nrq_gc_get_live_objects());
  result.gc_reclaimed = count > result.gc_live_after_pool_destroy
                            ? count - result.gc_live_after_pool_destroy
                            : 0;
  nrq_cdmm_arena_destroy(arena);
  nrq_cdmm_pool_destroy(pool);
  nrq_gc_collect();
  nrq_gc_enable();
  return result;
}

// --- concurrency -----------------------------------------------------------
//
// T mutators, each owning its graphs, arena and pool, against a collector thread
// that runs minors and majors underneath. What is measured is wall time and how
// many collections it contains: the per-object figures of the single-threaded
// matrix say nothing about what the safepoint handshake costs.
struct ConcurrentTiming {
  size_t threads = 0;
  size_t objects_per_thread = 0;
  double wall_ms = 0;
  double ns_per_object = 0;
  uint64_t collections = 0;
  uint64_t pin_violations = 0;
  size_t pinned_after = 0;
};

ConcurrentTiming run_concurrent(size_t threads, size_t objects_per_thread,
                                size_t rounds) {
  ConcurrentTiming timing;
  timing.threads = threads;
  timing.objects_per_thread = objects_per_thread;
  nrq_gc_enable();
  nrq_gc_set_generational(1);
  nrq_gc_collect();

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> allocations{0};
  const uint64_t collections_before = nrq_gc_get_collection_count();
  const auto start = Clock::now();

  std::thread collector([&] {
          // Paced, not spinning: a collector that runs back to back measures the
      // handshake under a load no program produces, and the point of this section
      // is the cost of collecting while four mutators allocate.
      while (!stop.load(std::memory_order_acquire)) {
        nrq_gc_collect_minor();
        nrq_gc_collect();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
  });

  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (size_t worker = 0; worker < threads; ++worker) {
    workers.emplace_back([&, worker] {
      nrq_gc_mutator_enter();
      void* slot = nullptr;
      nrq_gc_root_register(&slot);
      void* arena = nrq_cdmm_arena_create(16 * 1024, 1024 * 1024);
      void* pool = nrq_cdmm_pool_create(48, 8, 64, 0, nullptr, nullptr);
      std::vector<void*> graph(objects_per_thread, nullptr);
      for (size_t round = 0; round < rounds; ++round) {
        for (size_t index = 0; index < objects_per_thread; ++index) {
          void* object = nrq_gc_allocate_layout(layout_for(Shape::Small16));
          require(object != nullptr, "concurrent allocation returned null");
          graph[index] = object;
          if (index > 0) {
            void* previous = graph[index - 1];
            std::memcpy(previous, &object, sizeof(object));
          }
          if (nrq_cdmm_arena_allocate(arena, 32, 8) == nullptr) {
            nrq_cdmm_arena_reset(arena);
          }
          void* pooled = nrq_cdmm_pool_allocate(pool);
          if (pooled) nrq_cdmm_pool_release(pool, pooled);
        }
        slot = graph.back();
        nrq_gc_mutator_leave();
        allocations.fetch_add(objects_per_thread, std::memory_order_relaxed);
        nrq_gc_mutator_enter();
        graph.assign(objects_per_thread, nullptr);
        (void)worker;
      }
      nrq_gc_root_unregister(&slot);
      nrq_cdmm_arena_destroy(arena);
      nrq_cdmm_pool_destroy(pool);
      nrq_gc_mutator_leave();
    });
  }
  for (std::thread& worker : workers) worker.join();
  stop.store(true, std::memory_order_release);
  collector.join();
  const auto end = Clock::now();

  timing.wall_ms = std::chrono::duration<double, std::milli>(end - start).count();
  timing.collections =
      nrq_gc_get_collection_count() - collections_before;
  timing.ns_per_object = ns_per_op(end - start, allocations.load());
  timing.pin_violations = nrq_gc_pin_violations();
  timing.pinned_after = static_cast<size_t>(nrq_gc_pin_count());
  nrq_gc_set_generational(0);
  return timing;
}

}  // namespace

int main(int argc, char** argv) {
  size_t count = 200000;
  size_t runs = 3;
  if (argc > 1) count = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));
  if (argc > 2) runs = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));
  if (count < 1000 || runs == 0) {
    std::fprintf(stderr, "usage: phase8_matrix [objects>=1000] [runs>=1]\n");
    return 1;
  }

  std::printf("Phase 8 workload matrix: %zu objects, %zu runs\n", count, runs);
  std::printf("%-22s %8s %9s %9s %9s %10s %10s %7s %8s %8s %11s %9s\n", "shape",
              "objects", "alloc ns", "mark ns/o", "sweep ns/o", "pages",
              "live", "util %", "runs", "run pgs", "dropped B", "wave pgs");

  const Shape shapes[] = {Shape::Small16,   Shape::Small32,  Shape::Medium128,
                          Shape::Record512, Shape::Array4,   Shape::Nested,
                          Shape::Soa,       Shape::MixedRepr, Shape::MixedSizes,
                          Shape::Ring};
  // Survival matters as much as shape: a heap that loses everything says nothing
  // about reuse, and one that loses nothing says nothing about reclamation.
  const unsigned survivals[] = {50, 95, 5};
  for (Shape shape : shapes) {
    for (unsigned survival : survivals) {
      std::vector<Row> samples;
      samples.reserve(runs);
      for (size_t run = 0; run < runs; ++run)
        samples.push_back(run_shape(shape, count, survival));
      // Medians by allocation and marking cost; the counters are identical across
      // runs, so the last sample's counters are reported.
      std::vector<double> allocate;
      std::vector<double> mark;
      for (const Row& sample : samples) {
        allocate.push_back(sample.allocate_ns);
        mark.push_back(sample.mark_ns_per_object);
      }
      std::sort(allocate.begin(), allocate.end());
      std::sort(mark.begin(), mark.end());
      Row row = samples.back();
      row.allocate_ns = allocate[allocate.size() / 2];
      row.mark_ns_per_object = mark[mark.size() / 2];
      row.name = shape_name(shape);
      char label[64];
      std::snprintf(label, sizeof(label), "%s @%u%%", row.name, survival);
      row.name = label;
      print_row(row);
      if (runs > 1) {
        std::printf("    %-18s alloc %.1f..%.1f  mark %.2f..%.2f ns/object\n",
                    "range", allocate.front(), allocate.back(), mark.front(),
                    mark.back());
      }
    }
  }

  const Coexistence coexistence = run_coexistence(std::max<size_t>(count, 2000));
  std::printf("\ncoexistence: collector storage beside an arena and a pool\n");
  std::printf("  collector live after arena reset: %zu\n",
              coexistence.gc_live_after_arena_reset);
  std::printf("  arena bytes used before reset:    %zu\n",
              coexistence.arena_bytes_after_reset);
  std::printf("  collector object survived reset:  %s\n",
              coexistence.gc_object_survived ? "yes" : "no");
  std::printf("  collector live after root release: %zu\n",
              coexistence.gc_live_after_pool_destroy);
  std::printf("  collector objects reclaimed:       %zu\n",
              coexistence.gc_reclaimed);

  std::printf("\nconcurrency: four mutators against a collector thread\n");
  std::printf("%-10s %10s %10s %12s %11s %11s %9s\n", "threads", "objects",
              "wall ms", "ns/object", "collections", "pin viol", "pinned");
  for (size_t threads : {1u, 2u, 4u, 8u}) {
    const size_t per_thread = std::max<size_t>(count / (threads * 4), 1000);
    const ConcurrentTiming timing = run_concurrent(threads, per_thread, 2);
    std::printf("%-10zu %10zu %10.2f %12.1f %11llu %11llu %9zu\n", threads,
                per_thread, timing.wall_ms, timing.ns_per_object,
                static_cast<unsigned long long>(timing.collections),
                static_cast<unsigned long long>(timing.pin_violations),
                timing.pinned_after);
  }
  return 0;
}
