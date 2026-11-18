// Phase 7 unified-collector benchmark.
//
// The stress-test corpus times each phase with `time.now()`, which reports whole
// milliseconds. That is enough for a 100 ms phase and useless for a collection
// that now finishes in microseconds: the corpus prints `COLLECT_MS 0` for a
// million reclaimed objects. This harness measures the same collector entry
// points with `steady_clock`, so a sub-millisecond pause is a number instead of a
// zero, and it splits a cycle into the parts the phase report quotes:
//
//   allocate   ns per object, per representation
//   mark       µs for one full collection of a reachable graph
//   sweep      µs for one full collection of the same graph once it is garbage
//   barrier    ns per armed old-to-young store
//
// Inputs: [objects] [runs]. Large objects are scaled down because one 200 KiB
// object costs as much as 12 800 sixteen-byte records.
//
// Every timed cycle asserts that a cycle actually ran, that the mark phase kept
// the graph alive, and that the sweep phase reclaimed it, so a small number can
// never come from a collection that did nothing.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nirqoba/runtime/alloc_hint.h"
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
  std::fprintf(stderr, "benchmark invariant failed: %s\n", message);
  std::exit(2);
}

// --- root regions -----------------------------------------------------------
//
// An externally owned array of managed pointers is a root region: the collector
// re-reads every element through the (offset, stride) table it is given. It is
// registered and released by the storage address, not by a slot address.

const size_t kWordOffset[] = {0};

void register_word_region(std::vector<void*>& region) {
  nrq_gc_root_register_array(region.data(), region.size(), sizeof(void*),
                             kWordOffset, 1);
}

void unregister_word_region(std::vector<void*>& region) {
  nrq_gc_root_unregister(reinterpret_cast<void**>(region.data()));
}

struct Sample {
  double allocate_ns = 0;
  double mark_us = 0;
  double mark_ns_per_object = 0;
  double sweep_us = 0;
  double sweep_ns_per_object = 0;
  uint64_t live_after_mark = 0;
  uint64_t live_after_sweep = 0;
  uint64_t cycles = 0;
  size_t header_bytes = 0;
  int32_t representation = 0;
  size_t objects = 0;
};

enum class Shape {
  Chain16,      // H0 fixed record, one reference, linked into a chain
  Rooted16,     // the same record through allocate + a separate root store
  Planned16,    // the same record through the planned, rooted entry point
  Record512,    // H8 fixed record
  Array4x16,    // H16 four-element array
  LargeObject,  // H16 in a large-object page
  SoaPointers,  // H16 structure-of-arrays payload with one pointer column
};

const char* shape_name(Shape shape) {
  switch (shape) {
    case Shape::Chain16: return "h0-chain-16B";
    case Shape::Rooted16: return "h0-rooted-manual";
    case Shape::Planned16: return "h0-planned-16B";
    case Shape::Record512: return "h8-record-512B";
    case Shape::Array4x16: return "h16-array-4x16B";
    case Shape::LargeObject: return "h16-large-200K";
    case Shape::SoaPointers: return "h16-soa-ptr-column";
  }
  return "unknown";
}

// The descriptor every 16-byte chain node uses. Interning it once is what a
// compiler emits as a read-only table; the collector then shares one description
// across every instance.
const nrq_object_layout* chain_layout() {
  static const size_t kOffsets[] = {0};
  static const nrq_object_layout* layout =
      nrq_gc_layout_intern(16, 0, nullptr, kOffsets, 1);
  return layout;
}

size_t shape_objects(Shape shape, size_t requested) {
  switch (shape) {
    case Shape::Record512:
      return std::max<size_t>(requested / 24, 1000);
    case Shape::LargeObject:
      // 256 * 200 KiB is 50 MiB: enough pages to measure, small enough to run.
      return 256;
    default:
      return std::max<size_t>(requested, 1000);
  }
}

Sample run_shape(Shape shape, size_t requested) {
  const size_t count = shape_objects(shape, requested);
  Sample sample;
  sample.objects = count;

  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();

  std::vector<void*> objects(count, nullptr);
  void* root = nullptr;
  void* site_cache = nullptr;
  const bool chain = shape == Shape::Chain16 || shape == Shape::Planned16;

  nrq_gc_root_register(&root);
  if (!chain) register_word_region(objects);

  const uint32_t kPlannedHeaderless =
      NRQ_ALLOC_PLAN_HAS_REFERENCES |
      NRQ_ALLOC_PLAN_REPR_BITS(NRQ_ALLOC_REPR_HEADERLESS);

  const auto allocate_start = Clock::now();
  for (size_t index = 0; index < count; ++index) {
    void* object = nullptr;
    switch (shape) {
      case Shape::Chain16:
        object = nrq_gc_allocate_layout(chain_layout());
        break;
      case Shape::Rooted16:
        // The same allocation and the same publication as the planned entry
        // point, but as two calls: this is the cost the planned path replaces.
        object = nrq_gc_allocate_layout(chain_layout());
        if (object) nrq_gc_root_store(&root, object);
        break;
      case Shape::Planned16:
        object = nrq_gc_allocate_planned(&root, 16, kWordOffset,
                                         /*reference_count=*/1, &site_cache,
                                         kPlannedHeaderless);
        break;
      case Shape::Record512:
        object = nrq_gc_allocate(512, nullptr);
        break;
      case Shape::Array4x16: {
        static const size_t kOffsets[] = {0};
        object = nrq_gc_allocate_array_typed(4, 16, kOffsets, 1);
        break;
      }
      case Shape::LargeObject:
        object = nrq_gc_allocate(200 * 1024, nullptr);
        break;
      case Shape::SoaPointers: {
        // A structure-of-arrays payload of four rows that are twelve bytes each:
        // a contiguous pointer column followed by a four-byte scalar column. The
        // pointer column is (offset 0, stride 8), which is exactly how the
        // lowering describes a struct that was split into columns.
        static const size_t kColumns[] = {0, 8};
        object = nrq_gc_allocate_soa_rooted(&root, 4, 12, kColumns, 2);
        break;
      }
    }
    require(object != nullptr, "allocation returned null");
    objects[index] = object;
    if (chain && index > 0) {
      // `next` is the managed field at offset 0.
      void* previous = objects[index - 1];
      std::memcpy(previous, &object, sizeof(object));
    }
  }
  const auto allocate_end = Clock::now();
  sample.allocate_ns = ns_per_op(allocate_end - allocate_start, count);

  if (chain) root = objects[0];
  sample.header_bytes = nrq_gc_object_header_size(objects[0]);
  sample.representation = nrq_gc_object_representation(objects[0]);

  const uint64_t cycles_before = nrq_gc_get_collection_count();

  // Mark phase: every object is reachable from the chain head or the root
  // region, so this cycle marks the whole graph and reclaims nothing.
  const auto mark_start = Clock::now();
  nrq_gc_collect();
  const auto mark_end = Clock::now();
  sample.mark_us = us_total(mark_end - mark_start);
  sample.mark_ns_per_object = ns_per_op(mark_end - mark_start, count);
  sample.live_after_mark = nrq_gc_get_live_objects();
  require(nrq_gc_get_collection_count() > cycles_before,
          "the mark phase did not run a cycle");
  require(sample.live_after_mark >= count,
          "the mark phase reclaimed a reachable object");

  // Sweep phase: the graph becomes garbage when its roots go away.
  if (!chain) unregister_word_region(objects);
  nrq_gc_root_unregister(&root);
  const auto sweep_start = Clock::now();
  nrq_gc_collect();
  const auto sweep_end = Clock::now();
  sample.sweep_us = us_total(sweep_end - sweep_start);
  sample.sweep_ns_per_object = ns_per_op(sweep_end - sweep_start, count);
  sample.live_after_sweep = nrq_gc_get_live_objects();
  sample.cycles = nrq_gc_get_collection_count() - cycles_before;
  require(sample.live_after_sweep < count,
          "the sweep phase did not reclaim the graph");
  nrq_gc_enable();
  return sample;
}

// Armed old-to-young store barrier: promote a generation of old objects first,
// then time stores of freshly allocated young objects into a registered slot.
Sample run_barrier(size_t count) {
  Sample sample;
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();

  std::vector<void*> old_objects(64, nullptr);
  for (void*& object : old_objects) object = nrq_gc_allocate(32, nullptr);
  register_word_region(old_objects);
  nrq_gc_collect();  // a full cycle promotes them, arming the barrier
  require(nrq_gc_store_barrier_active() != 0,
          "the store barrier is not armed after a major cycle");

  void* slot = nullptr;
  nrq_gc_root_register(&slot);
  std::vector<void*> young(count, nullptr);
  for (size_t index = 0; index < count; ++index)
    young[index] = nrq_gc_allocate(32, nullptr);
  register_word_region(young);

  const auto start = Clock::now();
  for (size_t index = 0; index < count; ++index)
    nrq_gc_root_store(&slot, young[index]);
  const auto end = Clock::now();
  sample.allocate_ns = ns_per_op(end - start, count);
  sample.objects = count;

  nrq_gc_root_unregister(&slot);
  unregister_word_region(young);
  unregister_word_region(old_objects);
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
  return sample;
}

// Page utilization for a heap where half of the objects survive. A page class
// that cannot reuse the space of a dead neighbour shows up as committed pages
// well above what the surviving payload needs.
struct Fragmentation {
  size_t live_objects = 0;
  size_t live_payload_bytes = 0;
  size_t pages = 0;
  size_t committed_bytes = 0;
  size_t free_runs = 0;
  size_t pages_with_runs = 0;
  double utilization = 0;
  // Representation facts sampled while this heap is still live. The stats are
  // computed from the page set, so asking after everything is collected would
  // report zeros.
  nrq_gc_representation_stats stats{};
};

Fragmentation run_fragmentation(size_t count) {
  Fragmentation result;
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();

  {
    std::vector<void*> objects(count, nullptr);
    for (size_t index = 0; index < count; ++index)
      objects[index] = nrq_gc_allocate_layout(chain_layout());
    register_word_region(objects);

    // Retain every other object: the survivors are interleaved with garbage.
    std::vector<void*> survivors(count / 2, nullptr);
    for (size_t index = 0; index < survivors.size(); ++index)
      survivors[index] = objects[index * 2];
    register_word_region(survivors);
    unregister_word_region(objects);
    objects.clear();
    objects.shrink_to_fit();

    nrq_gc_collect();

    result.live_objects = nrq_gc_get_live_objects();
    result.live_payload_bytes = survivors.size() * 16;
    result.pages = nrq_gc_page_count();
    result.committed_bytes = result.pages * 64 * 1024;
    result.free_runs = nrq_gc_free_run_count();
    result.pages_with_runs = nrq_gc_page_with_runs_count();
    result.stats = nrq_gc_get_representation_stats();
    result.utilization =
        result.committed_bytes == 0
            ? 0
            : 100.0 * static_cast<double>(result.live_payload_bytes) /
                  static_cast<double>(result.committed_bytes);

    unregister_word_region(survivors);
  }
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
  return result;
}

void print_row(const char* name, size_t objects, const Sample& sample) {
  std::printf("%-22s %9zu %9.1f %9.1f %11.2f %9.1f %11.2f %3zu %3zu %10llu\n",
              name, objects, sample.allocate_ns, sample.mark_us,
              sample.mark_ns_per_object, sample.sweep_us,
              sample.sweep_ns_per_object, sample.header_bytes,
              static_cast<size_t>(sample.representation),
              static_cast<unsigned long long>(sample.live_after_sweep));
}

void print_heap_facts() {
  const nrq_gc_representation_facts facts = nrq_gc_get_representation_facts();
  const nrq_gc_allocation_facts allocation = nrq_gc_get_allocation_facts();
  const nrq_gc_page_metadata_facts page = nrq_gc_get_page_metadata_facts();
  std::printf("\nheap facts\n");
  std::printf("  headers bytes:        H0 %zu  H8 %zu  H16 %zu  max %zu\n",
              facts.headerless_header_bytes, facts.compact_header_bytes,
              facts.extended_header_bytes, facts.max_header_bytes);
  std::printf("  payload alignment:    H0 %zu  H8 %zu  H16 %zu\n",
              facts.headerless_payload_alignment, facts.compact_payload_alignment,
              facts.extended_payload_alignment);
  std::printf("  page metadata:        %zu bytes for %zu bitmap bytes\n",
              allocation.page_bytes, page.total_bitmap_bytes);
  std::printf("  layout descriptors:   %zu (%zu bytes), memo %llu hits / %llu misses\n",
              allocation.layout_descriptors, allocation.layout_bytes,
              static_cast<unsigned long long>(nrq_gc_layout_memo_hits()),
              static_cast<unsigned long long>(nrq_gc_layout_memo_misses()));

  std::printf("\nallocation profile\n");
  std::printf("%-10s %10s %8s %10s %8s %10s\n", "payload", "span", "per page",
              "page slack", "per refill", "page bytes");
  const size_t payloads[] = {16, 64, 256, 512, 4096, 65536, 200 * 1024};
  for (size_t payload : payloads) {
    nrq_gc_allocation_profile profile{};
    if (!nrq_gc_get_allocation_profile(payload, &profile)) continue;
    std::printf("%-10zu %10zu %8zu %10zu %8zu %10zu\n", profile.payload_bytes,
                profile.span_bytes, profile.slots_per_page,
                profile.page_slack_bytes, profile.objects_per_refill,
                profile.page_bytes);
  }
}

double median_of(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
  size_t count = 1000000;
  size_t runs = 5;
  if (argc > 1) count = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));
  if (argc > 2) runs = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));
  if (count < 1000 || runs == 0) {
    std::fprintf(stderr, "usage: phase7_benchmark [objects>=1000] [runs>=1]\n");
    return 1;
  }

  std::printf("Phase 7 collector benchmark: %zu objects, %zu runs\n", count, runs);
  std::printf("%-22s %9s %9s %9s %11s %9s %11s %3s %3s %10s\n", "shape",
              "objects", "alloc ns", "mark us", "mark ns/obj", "sweep us",
              "sweep ns/obj", "hdr", "rep", "live after");

  const Shape shapes[] = {Shape::Chain16,     Shape::Rooted16,
                          Shape::Planned16,
                          Shape::Record512,   Shape::Array4x16,
                          Shape::LargeObject, Shape::SoaPointers};
  for (Shape shape : shapes) {
    std::vector<Sample> samples;
    samples.reserve(runs);
    for (size_t run = 0; run < runs; ++run)
      samples.push_back(run_shape(shape, count));
    for (size_t index = 0; index < samples.size(); ++index) {
      char label[64];
      std::snprintf(label, sizeof(label), "%s#%zu", shape_name(shape), index + 1);
      print_row(label, samples[index].objects, samples[index]);
    }
    const Sample& last = samples.back();
    Sample combined;
    combined.allocate_ns =
        median_of({samples[0].allocate_ns, samples[samples.size() / 2].allocate_ns,
                   samples.back().allocate_ns});
    combined.mark_us = median_of({samples[0].mark_us,
                                  samples[samples.size() / 2].mark_us,
                                  samples.back().mark_us});
    combined.mark_ns_per_object =
        median_of({samples[0].mark_ns_per_object,
                   samples[samples.size() / 2].mark_ns_per_object,
                   samples.back().mark_ns_per_object});
    combined.sweep_us = median_of({samples[0].sweep_us,
                                   samples[samples.size() / 2].sweep_us,
                                   samples.back().sweep_us});
    combined.sweep_ns_per_object =
        median_of({samples[0].sweep_ns_per_object,
                   samples[samples.size() / 2].sweep_ns_per_object,
                   samples.back().sweep_ns_per_object});
    combined.header_bytes = last.header_bytes;
    combined.representation = last.representation;
    combined.live_after_sweep = last.live_after_sweep;
    combined.objects = last.objects;
    std::string label = std::string(shape_name(shape)) + "-median";
    print_row(label.c_str(), combined.objects, combined);
  }

  std::printf("\n%-22s %9s %11s\n", "workload", "objects", "ns/store");
  std::vector<double> barrier_samples;
  for (size_t run = 0; run < runs; ++run) {
    const Sample sample = run_barrier(std::max<size_t>(count / 4, 1000));
    barrier_samples.push_back(sample.allocate_ns);
    print_row("barrier-old-to-young", sample.objects, sample);
  }
  std::printf("barrier-old-to-young median %9.2f ns/store\n",
              median_of(barrier_samples));

  const Fragmentation fragmentation =
      run_fragmentation(std::max<size_t>(count, 2000));
  std::printf("\nfragmentation: half of %zu objects survive\n", count);
  std::printf("  live objects:      %zu\n", fragmentation.live_objects);
  std::printf("  live payload:      %zu bytes\n", fragmentation.live_payload_bytes);
  std::printf("  committed pages:   %zu (%zu bytes)\n", fragmentation.pages,
              fragmentation.committed_bytes);
  std::printf("  utilization:       %.1f%%\n", fragmentation.utilization);
  std::printf("  free runs:         %zu over %zu pages\n", fragmentation.free_runs,
              fragmentation.pages_with_runs);
  const nrq_gc_representation_stats& live = fragmentation.stats;
  std::printf("  live by header:    H0 %llu  H8 %llu  H16 %llu\n",
              static_cast<unsigned long long>(live.headerless_objects),
              static_cast<unsigned long long>(live.compact_objects),
              static_cast<unsigned long long>(live.extended_objects));
  std::printf("  pages by header:   H0 %zu  H8 %zu  H16 %zu  large %zu  fallback %zu\n",
              live.headerless_pages, live.compact_pages, live.extended_pages,
              live.large_pages, live.fallback_pages);
  std::printf("  page metadata:     H0 %zu  H8 %zu  H16 %zu bytes\n",
              live.headerless_metadata_bytes, live.compact_metadata_bytes,
              live.extended_metadata_bytes);
  std::printf("  header bytes saved: %llu against an H16 header per object\n",
              static_cast<unsigned long long>(live.header_bytes_saved_vs_extended));

  print_heap_facts();
  return 0;
}
