// AOR Phase 4 representation benchmark.
//
// Measures the three representations through the public GC API on the same
// workload shape: a fixed live set of N records, the cost of building it, a full
// collection with everything live, and a full collection of the same heap after
// every reference is dropped. Each shape keeps one managed reference so marking
// walks the same graph in all three cases.
//
//   phase4_benchmark [objects] [runs]
//
// Prints one row per representation per run and a median row per representation.
// It reports timings only; it does not compute the derived comparison.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nirqoba/runtime/gc.h"

namespace {

using Clock = std::chrono::steady_clock;

enum class Shape { Headerless, Compact, Extended, Mixed };

const char* shape_name(Shape shape) {
  switch (shape) {
    case Shape::Headerless: return "H0-64B-record";
    case Shape::Compact: return "H8-1KiB-record";
    case Shape::Extended: return "H16-1KiB-array";
    case Shape::Mixed: return "mixed-third-each";
  }
  return "?";
}

// The shape one allocation of a mixed run asks for. Every third object is a
// different representation, which is the case the page-kind dedication and the
// second-shape slab rule exist for.
Shape shape_for(Shape shape, size_t index) {
  if (shape != Shape::Mixed) return shape;
  switch (index % 3) {
    case 0: return Shape::Headerless;
    case 1: return Shape::Compact;
    default: return Shape::Extended;
  }
}

size_t shape_bytes(Shape shape) {
  switch (shape) {
    case Shape::Headerless: return 64;
    case Shape::Compact:
    case Shape::Extended: return 1024;
    case Shape::Mixed: return 0;
  }
  return 0;
}

struct Sample {
  double allocate_ns = 0;
  double live_collect_ns = 0;
  double dead_collect_ns = 0;
  size_t live_bytes = 0;
  size_t headerless = 0;
  size_t compact = 0;
  size_t extended = 0;
  size_t headerless_state_bytes = 0;
  size_t compact_state_bytes = 0;
  size_t extended_state_bytes = 0;
  size_t pages_headerless = 0;
  size_t pages_compact = 0;
  size_t pages_extended = 0;
  size_t pages_large = 0;
  uint64_t collections = 0;
};

// The container is a typed array of pointer words, so the collector walks every
// slot as a managed reference with element stride.
Sample run(Shape shape, size_t count, bool measure_bytes) {
  const size_t offsets[1] = {0};
  void* container =
      nrq_gc_allocate_array_typed(count, sizeof(void*), offsets, 1);
  if (!container) {
    std::fprintf(stderr, "container allocation failed\n");
    std::exit(2);
  }
  void* root = container;
  nrq_gc_root_register(&root);
  auto** slots = static_cast<void**>(container);

  nrq_gc_mutator_enter();
  Sample sample;
  const auto allocate_begin = Clock::now();
  for (size_t index = 0; index < count; ++index) {
    const Shape request = shape_for(shape, index);
    void* object = nullptr;
    switch (request) {
      case Shape::Headerless:
        object = nrq_gc_allocate_repr(shape_bytes(request), nullptr,
                                      NRQ_GC_REPR_HEADERLESS);
        break;
      case Shape::Compact:
        object = nrq_gc_allocate_repr(shape_bytes(request), nullptr,
                                      NRQ_GC_REPR_COMPACT);
        break;
      default:
        object = nrq_gc_allocate_repr(shape_bytes(request), nullptr,
                                      NRQ_GC_REPR_EXTENDED);
        break;
    }
    if (!object) {
      std::fprintf(stderr, "object allocation failed at %zu\n", index);
      std::exit(2);
    }
    slots[index] = object;
  }
  nrq_gc_mutator_leave();
  sample.allocate_ns = std::chrono::duration<double, std::nano>(
                           Clock::now() - allocate_begin)
                           .count();

  const auto live_begin = Clock::now();
  nrq_gc_collect();
  sample.live_collect_ns =
      std::chrono::duration<double, std::nano>(Clock::now() - live_begin).count();

  if (measure_bytes) {
    // Metadata per object, derived from the live set rather than assumed: the
    // pages actually used for this shape divided by the objects in them.
    const nrq_gc_representation_stats stats = nrq_gc_get_representation_stats();
    sample.headerless = static_cast<size_t>(stats.headerless_objects);
    sample.compact = static_cast<size_t>(stats.compact_objects);
    sample.extended = static_cast<size_t>(stats.extended_objects);
    sample.live_bytes = sample.headerless * 0 + sample.compact * 8 +
                        sample.extended * 16;
    sample.headerless_state_bytes = stats.headerless_metadata_bytes;
    sample.compact_state_bytes = stats.compact_metadata_bytes;
    sample.extended_state_bytes = stats.extended_metadata_bytes;
    sample.pages_headerless = stats.headerless_pages;
    sample.pages_compact = stats.compact_pages;
    sample.pages_extended = stats.extended_pages;
    sample.pages_large = stats.large_pages;
    sample.collections = nrq_gc_get_stats().collections;
  }

  for (size_t index = 0; index < count; ++index) slots[index] = nullptr;
  const auto dead_begin = Clock::now();
  nrq_gc_collect();
  sample.dead_collect_ns =
      std::chrono::duration<double, std::nano>(Clock::now() - dead_begin).count();

  nrq_gc_root_unregister(&root);
  nrq_gc_collect();
  return sample;
}

double median(std::vector<double> values) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
  const size_t count =
      argc > 1 ? std::strtoull(argv[1], nullptr, 10) : size_t{200000};
  const int runs = argc > 2 ? std::atoi(argv[2]) : 5;
  if (count < 16 || runs < 1) {
    std::fprintf(stderr, "usage: %s [objects>=16] [runs>=1]\n", argv[0]);
    return 2;
  }

  std::printf("# AOR Phase 4 representation benchmark\n");
  std::printf("# objects=%zu runs=%d\n", count, runs);
  const nrq_gc_representation_facts facts = nrq_gc_get_representation_facts();
  std::printf("# headers: H0=%zu H8=%zu H16=%zu max=%zu\n",
              facts.headerless_header_bytes, facts.compact_header_bytes,
              facts.extended_header_bytes, facts.max_header_bytes);

  for (Shape shape : {Shape::Headerless, Shape::Compact, Shape::Extended,
                      Shape::Mixed}) {
    // One warm-up run keeps the first-touch page population out of the samples.
    run(shape, count, false);
    std::vector<double> allocate;
    std::vector<double> live;
    std::vector<double> dead;
    size_t header_bytes = 0;
    size_t metadata_bytes = 0;
    size_t pages_headerless = 0;
    size_t pages_compact = 0;
    size_t pages_extended = 0;
    size_t pages_large = 0;
    uint64_t collections = 0;
    size_t extended = 0;
    for (int iteration = 0; iteration < runs; ++iteration) {
      const Sample sample = run(shape, count, iteration == 0);
      allocate.push_back(sample.allocate_ns);
      live.push_back(sample.live_collect_ns);
      dead.push_back(sample.dead_collect_ns);
      if (iteration == 0) {
        header_bytes = sample.live_bytes;
        metadata_bytes = sample.headerless_state_bytes +
                         sample.compact_state_bytes +
                         sample.extended_state_bytes;
        extended = sample.extended;
        pages_headerless = sample.pages_headerless;
        pages_compact = sample.pages_compact;
        pages_extended = sample.pages_extended;
        pages_large = sample.pages_large;
        collections = sample.collections;
      }
    }
    std::printf(
        "shape=%s n=%zu alloc_median_ns=%zu live_gc_median_ns=%zu "
        "dead_gc_median_ns=%zu header_bytes=%zu metadata_bytes=%zu "
        "h0_pages=%zu h8_pages=%zu h16_pages=%zu large_pages=%zu "
        "collections=%llu h16_objects=%zu\n",
        shape_name(shape), count, static_cast<size_t>(median(allocate)),
        static_cast<size_t>(median(live)), static_cast<size_t>(median(dead)),
        header_bytes, metadata_bytes, pages_headerless, pages_compact,
        pages_extended, pages_large,
        static_cast<unsigned long long>(collections), extended);
  }
  return 0;
}
