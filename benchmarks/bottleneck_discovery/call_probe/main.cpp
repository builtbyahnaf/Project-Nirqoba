// Per-operation cost probe for the GC runtime entry points the compiler emits
// around one managed object (diagnostic only; not part of the build).
//
// Every mode enters one mutator and runs one warmup pass, then times N
// operations. Automatic collection stays disabled so the measurement isolates
// the entry-point cost rather than collection scheduling.
//
//   call_probe.exe <mode> [count]

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

constexpr size_t kLayout[] = {0, 8};
constexpr size_t kRootDescriptor[] = {2, 8, 8, 1, 0, 8, 1, 0};

void report(const char* name, int64_t count, double ms) {
  const double ns = ms * 1.0e6 / static_cast<double>(count);
  std::printf("OP %s N %lld MS %.3f NS_PER %.3f\n", name,
              static_cast<long long>(count), ms, ns);
  std::fflush(stdout);
}

// One registered two-slot frame that stays open for the whole loop.
struct LongFrame {
  uint64_t marker;
  void* slots[2]{};
  void* table[2];
  LongFrame() {
    marker = nrq_gc_root_frame_begin();
    table[0] = &slots[0];
    table[1] = &slots[1];
    nrq_gc_root_register_frame_static(table, 2, kRootDescriptor);
  }
  ~LongFrame() { nrq_gc_root_frame_end(marker); }
};

void run_allocate_rooted(int64_t count, size_t references) {
  LongFrame frame;
  const size_t* layout = references ? kLayout : nullptr;
  // Warm up one page so the first page acquisition is not in the sample.
  for (int i = 0; i < 512; ++i)
    nrq_gc_allocate_array_typed_rooted(&frame.slots[0], 1, 24, layout,
                                       references);
  const double start = now_ms();
  for (int64_t i = 0; i < count; ++i)
    nrq_gc_allocate_array_typed_rooted(&frame.slots[0], 1, 24, layout,
                                       references);
  report(references ? "allocate_rooted_ref2" : "allocate_rooted_ref0", count,
         now_ms() - start);
}

void run_allocate_unrooted(int64_t count) {
  const double start = now_ms();
  for (int64_t i = 0; i < count; ++i)
    nrq_gc_allocate_array_typed(1, 24, kLayout, 2);
  report("allocate_unrooted_ref2", count, now_ms() - start);
}

// Same allocation loop twice. The first pass grows the heap and pays every
// first-touch page fault; after two collections the retired pages are recycled,
// so the second pass allocates the same number of objects from memory the
// process already owns. The difference between the two numbers is the OS cost
// (page faults) hiding inside a bulk allocation measurement.
void run_allocate_warm(int64_t count) {
  LongFrame frame;
  double start = now_ms();
  for (int64_t i = 0; i < count; ++i)
    nrq_gc_allocate_array_typed_rooted(&frame.slots[0], 1, 24, kLayout, 2);
  report("allocate_rooted_cold_ref2", count, now_ms() - start);
  // First collection reclaims the batch; the second recycles its pages.
  nrq_gc_collect();
  nrq_gc_collect();
  start = now_ms();
  for (int64_t i = 0; i < count; ++i)
    nrq_gc_allocate_array_typed_rooted(&frame.slots[0], 1, 24, kLayout, 2);
  report("allocate_rooted_warm_ref2", count, now_ms() - start);
}

void run_frame(int64_t count) {
  for (int i = 0; i < 512; ++i) {
    const uint64_t marker = nrq_gc_root_frame_begin();
    void* slots[2] = {nullptr, nullptr};
    void* table[2] = {&slots[0], &slots[1]};
    nrq_gc_root_register_frame_static(table, 2, kRootDescriptor);
    nrq_gc_root_frame_end(marker);
  }
  const double start = now_ms();
  for (int64_t i = 0; i < count; ++i) {
    const uint64_t marker = nrq_gc_root_frame_begin();
    void* slots[2] = {nullptr, nullptr};
    void* table[2] = {&slots[0], &slots[1]};
    nrq_gc_root_register_frame_static(table, 2, kRootDescriptor);
    nrq_gc_root_frame_end(marker);
  }
  report("frame_begin_register_end_2slots", count, now_ms() - start);
}

// Frame begin/end with no slot publication, plus one registration/unregistration
// of a single pointer root, so the fixed and per-slot parts can be separated.
void run_frame_parts(int64_t count) {
  for (int i = 0; i < 512; ++i) {
    const uint64_t marker = nrq_gc_root_frame_begin();
    nrq_gc_root_frame_end(marker);
  }
  double start = now_ms();
  for (int64_t i = 0; i < count; ++i) {
    const uint64_t marker = nrq_gc_root_frame_begin();
    nrq_gc_root_frame_end(marker);
  }
  report("frame_begin_end_only", count, now_ms() - start);

  void* slot = nullptr;
  for (int i = 0; i < 512; ++i) {
    nrq_gc_root_register(&slot);
    nrq_gc_root_unregister(&slot);
  }
  start = now_ms();
  for (int64_t i = 0; i < count; ++i) {
    nrq_gc_root_register(&slot);
    nrq_gc_root_unregister(&slot);
  }
  report("root_register_unregister_1slot", count, now_ms() - start);

  void* slots[4]{};
  void* table[4] = {&slots[0], &slots[1], &slots[2], &slots[3]};
  constexpr size_t descriptor4[] = {4, 14, 8, 1, 0, 8, 1, 0,
                                    8, 1, 0, 8, 1, 0};
  for (int i = 0; i < 512; ++i) {
    const uint64_t marker = nrq_gc_root_frame_begin();
    nrq_gc_root_register_frame_static(table, 4, descriptor4);
    nrq_gc_root_frame_end(marker);
  }
  start = now_ms();
  for (int64_t i = 0; i < count; ++i) {
    const uint64_t marker = nrq_gc_root_frame_begin();
    nrq_gc_root_register_frame_static(table, 4, descriptor4);
    nrq_gc_root_frame_end(marker);
  }
  report("frame_begin_register_end_4slots", count, now_ms() - start);
}

void run_store(int64_t count) {
  LongFrame frame;
  void* first = nrq_gc_allocate_array_typed_rooted(&frame.slots[0], 1, 24,
                                                  kLayout, 2);
  void* second = nrq_gc_allocate_array_typed_rooted(&frame.slots[1], 1, 24,
                                                   kLayout, 2);
  const double start = now_ms();
  for (int64_t i = 0; i < count; ++i)
    nrq_gc_root_store(&frame.slots[0], (i & 1) ? second : first);
  report("root_store_alternating", count, now_ms() - start);
}

void run_pin(int64_t count) {
  LongFrame frame;
  void* object = nrq_gc_allocate_array_typed_rooted(&frame.slots[0], 1, 24,
                                                   kLayout, 2);
  for (int i = 0; i < 64; ++i) {
    nrq_gc_pin_transfer(object);
    nrq_gc_unpin(object);
  }
  const double start = now_ms();
  for (int64_t i = 0; i < count; ++i) {
    nrq_gc_pin_transfer(object);
    nrq_gc_unpin(object);
  }
  report("pin_transfer_plus_unpin", count, now_ms() - start);
}

// The exact per-pair sequence of test 03: frame, two rooted allocations, four
// root stores, frame end. Two objects per iteration.
void run_pair(int64_t count) {
  for (int i = 0; i < 512; ++i) {
    const uint64_t marker = nrq_gc_root_frame_begin();
    void* slots[2] = {nullptr, nullptr};
    void* table[2] = {&slots[0], &slots[1]};
    nrq_gc_root_register_frame_static(table, 2, kRootDescriptor);
    nrq_gc_root_frame_end(marker);
  }
  const double start = now_ms();
  for (int64_t i = 0; i < count; ++i) {
    const uint64_t marker = nrq_gc_root_frame_begin();
    void* slots[2] = {nullptr, nullptr};
    void* table[2] = {&slots[0], &slots[1]};
    nrq_gc_root_register_frame_static(table, 2, kRootDescriptor);
    void* first = nrq_gc_allocate_array_typed_rooted(&slots[0], 1, 16,
                                                    kLayout, 1);
    nrq_gc_root_store(&slots[0], first);
    void* second = nrq_gc_allocate_array_typed_rooted(&slots[1], 1, 16,
                                                     kLayout, 1);
    nrq_gc_root_store(&slots[1], second);
    nrq_gc_root_store(reinterpret_cast<void**>(first), second);
    nrq_gc_root_store(reinterpret_cast<void**>(second), first);
    nrq_gc_root_frame_end(marker);
  }
  report("test03_pair_2objects", count * 2, now_ms() - start);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "pair";
  const int64_t count = argc > 2 ? std::atoll(argv[2]) : 200000;
  nrq_gc_mutator_enter();
  nrq_gc_disable();
  nrq_gc_collect();
  {
    const uint64_t marker = nrq_gc_root_frame_begin();
    if (mode == "alloc-rooted")
      run_allocate_rooted(count, 2);
    else if (mode == "alloc-rooted0")
      run_allocate_rooted(count, 0);
    else if (mode == "alloc-warm")
      run_allocate_warm(count);
    else if (mode == "alloc-unrooted")
      run_allocate_unrooted(count);
    else if (mode == "frame")
      run_frame(count);
    else if (mode == "frame-parts")
      run_frame_parts(count);
    else if (mode == "store")
      run_store(count);
    else if (mode == "pin")
      run_pin(count);
    else if (mode == "pair")
      run_pair(count);
    else
      std::printf("unknown mode\n");
    nrq_gc_root_frame_end(marker);
  }
  nrq_gc_mutator_leave();
  return 0;
}
