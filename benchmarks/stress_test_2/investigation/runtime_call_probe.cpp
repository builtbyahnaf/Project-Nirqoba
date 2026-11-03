#include "nirqoba/runtime/gc.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

volatile void* sink;

struct Node {
  void* next;
  int64_t value;
};

void report(const char* name, double elapsed_ms, size_t operations) {
  const double per_op_ns =
      elapsed_ms * 1.0e6 / static_cast<double>(operations);
  std::printf("%-34s %10.3f ms  %8.2f ns/op\n", name, elapsed_ms, per_op_ns);
}

// Mirrors the generated sequence for the test suite's make_node helper plus
// its caller's link update. The outer mutator is already entered.
__declspec(noinline) void make_node_pattern(int64_t value, void** caller_cursor) {
  nrq_gc_mutator_enter();
  const uint64_t marker = nrq_gc_root_frame_begin();

  void* node_slot = nullptr;
  void* extra_slot = nullptr;
  const size_t offset = 0;
  nrq_gc_root_register_typed(&node_slot, sizeof(void*), &offset, 1);
  nrq_gc_root_register_typed(&extra_slot, sizeof(void*), &offset, 1);

  void* object = nrq_gc_allocate_array_typed_rooted(
      &node_slot, 1, sizeof(Node), &offset, 1);
  nrq_gc_root_store(&extra_slot, object);
  static_cast<Node*>(object)->value = value;

  void* transferred = nrq_gc_pin_transfer(object);
  nrq_gc_root_frame_end(marker);
  nrq_gc_mutator_leave();

  // Caller side of the managed return handoff and the linked-field store.
  void* caller_temporary = transferred;
  nrq_gc_root_copy(caller_cursor, &caller_temporary, sizeof(void*));
  nrq_gc_unpin(*caller_cursor);
  nrq_gc_root_store(caller_cursor, transferred);
  nrq_gc_root_store(&static_cast<Node*>(*caller_cursor)->next, nullptr);
  nrq_gc_safepoint();
}

}  // namespace

int main() {
  nrq_gc_disable();
  nrq_gc_mutator_enter();

  constexpr size_t kSmall = 1'000'000;
  constexpr size_t kLarge = 10'000'000;

  // Baseline loop so the harness overhead is visible next to runtime calls.
  {
    volatile size_t counter = 0;
    const double start = now_ms();
    for (size_t i = 0; i < kLarge; ++i) counter += i;
    const double elapsed = now_ms() - start;
    report("empty loop (baseline)", elapsed, kLarge);
    sink = const_cast<void*>(static_cast<const volatile void*>(&counter));
  }

  // Loop-backedge safepoint, as emitted on every benchmark loop iteration.
  {
    const double start = now_ms();
    for (size_t i = 0; i < kLarge; ++i) nrq_gc_safepoint();
    const double elapsed = now_ms() - start;
    report("nrq_gc_safepoint", elapsed, kLarge);
  }

  // Nested mutator enter/leave, the pattern used by helper-function calls.
  {
    nrq_gc_mutator_enter();  // outer function activation
    const double start = now_ms();
    for (size_t i = 0; i < kSmall; ++i) {
      nrq_gc_mutator_enter();
      nrq_gc_mutator_leave();
    }
    const double elapsed = now_ms() - start;
    report("nested mutator enter+leave", elapsed, kSmall);
    nrq_gc_mutator_leave();
  }

  // Top-level enter/leave publishes a TLAB and registers the thread.
  {
    const double start = now_ms();
    for (size_t i = 0; i < kSmall; ++i) {
      nrq_gc_mutator_enter();
      nrq_gc_mutator_leave();
    }
    const double elapsed = now_ms() - start;
    report("top-level mutator enter+leave", elapsed, kSmall);
  }

  // Root frame begin/end, once per GC-capable function activation.
  {
    const double start = now_ms();
    for (size_t i = 0; i < kSmall; ++i) {
      const uint64_t marker = nrq_gc_root_frame_begin();
      nrq_gc_root_frame_end(marker);
    }
    const double elapsed = now_ms() - start;
    report("root frame begin+end", elapsed, kSmall);
  }

  // Root slot registration and unregistration, once per managed local.
  {
    void* slot = nullptr;
    const size_t offset = 0;
    const double start = now_ms();
    for (size_t i = 0; i < kSmall; ++i) {
      nrq_gc_root_register_typed(&slot, sizeof(void*), &offset, 1);
      nrq_gc_root_unregister(&slot);
    }
    const double elapsed = now_ms() - start;
    report("root register+unregister", elapsed, kSmall);
  }

  // Root write barrier, as emitted when a managed local/field is updated.
  {
    void* slot = nullptr;
    const size_t offset = 0;
    nrq_gc_root_register_typed(&slot, sizeof(void*), &offset, 1);
    const double start = now_ms();
    for (size_t i = 0; i < kLarge; ++i) nrq_gc_root_store(&slot, nullptr);
    const double elapsed = now_ms() - start;
    report("root_store(null)", elapsed, kLarge);
    nrq_gc_root_unregister(&slot);
  }

  // Allocation exactly as benchmark test 01: one 16-byte Node per call,
  // through the rooted allocation entry point.
  {
    void* root = nullptr;
    const size_t offset = 0;
    nrq_gc_root_register_typed(&root, sizeof(void*), &offset, 1);
    const double start = now_ms();
    for (size_t i = 0; i < kSmall; ++i) {
      void* object = nrq_gc_allocate_array_typed_rooted(
          &root, 1, sizeof(Node), &offset, 1);
      static_cast<Node*>(object)->value = static_cast<int64_t>(i);
    }
    const double elapsed = now_ms() - start;
    report("rooted Node allocation", elapsed, kSmall);
    root = nullptr;
    nrq_gc_root_unregister(&root);
    nrq_gc_collect();
  }

  // Unrooted typed allocation, to separate the TLAB bump path from the
  // rooted wrapper's root validation/update locks.
  {
    const size_t offset = 0;
    const double start = now_ms();
    for (size_t i = 0; i < kSmall; ++i) {
      void* object = nrq_gc_allocate_array_typed(1, sizeof(Node), &offset, 1);
      static_cast<Node*>(object)->value = static_cast<int64_t>(i);
      if ((i & 1023) == 0) sink = object;
    }
    const double elapsed = now_ms() - start;
    report("unrooted Node allocation", elapsed, kSmall);
    nrq_gc_collect();
  }

  // Root store with a live young object, which performs the object-index
  // lookup used by the generational remembered-set barrier.
  {
    void* root = nullptr;
    const size_t offset = 0;
    nrq_gc_root_register_typed(&root, sizeof(void*), &offset, 1);
    void* object = nrq_gc_allocate_array_typed_rooted(
        &root, 1, sizeof(Node), &offset, 1);
    nrq_gc_root_store(&root, object);
    const double start = now_ms();
    for (size_t i = 0; i < kLarge; ++i) nrq_gc_root_store(&root, object);
    const double elapsed = now_ms() - start;
    report("root_store(live young object)", elapsed, kLarge);
    root = nullptr;
    nrq_gc_root_unregister(&root);
    nrq_gc_collect();
  }

  // Managed return handoff: pin transfer plus the caller's unpin.
  {
    void* root = nullptr;
    const size_t offset = 0;
    nrq_gc_root_register_typed(&root, sizeof(void*), &offset, 1);
    void* object = nrq_gc_allocate_array_typed_rooted(
        &root, 1, sizeof(Node), &offset, 1);
    const double start = now_ms();
    for (size_t i = 0; i < kSmall; ++i) {
      void* transferred = nrq_gc_pin_transfer(object);
      nrq_gc_unpin(transferred);
    }
    const double elapsed = now_ms() - start;
    report("pin_transfer+unpin", elapsed, kSmall);
    root = nullptr;
    nrq_gc_root_unregister(&root);
    nrq_gc_collect();
  }

  // Aggregate root copy, used for managed return values and aggregate stores.
  {
    void* root = nullptr;
    const size_t offset = 0;
    nrq_gc_root_register_typed(&root, sizeof(void*), &offset, 1);
    void* source = nullptr;
    const double start = now_ms();
    for (size_t i = 0; i < kLarge; ++i)
      nrq_gc_root_copy(&root, &source, sizeof(void*));
    const double elapsed = now_ms() - start;
    report("root_copy(8 bytes)", elapsed, kLarge);
    root = nullptr;
    nrq_gc_root_unregister(&root);
  }

  // Reconstruct the complete per-node allocation sequence used by test 01.
  // Start and end at depth 0 so the final leave includes TLAB publication,
  // just as build_chain's return does in the benchmark.
  {
    nrq_gc_mutator_leave();  // drop the probe's long-lived activation
    void* cursor = nullptr;
    const size_t offset = 0;
    nrq_gc_root_register_typed(&cursor, sizeof(void*), &offset, 1);
    nrq_gc_mutator_enter();
    const double start = now_ms();
    for (size_t i = 0; i < kSmall; ++i)
      make_node_pattern(static_cast<int64_t>(i), &cursor);
    const double elapsed = now_ms() - start;
    report("full make_node sequence", elapsed, kSmall);

    const double publish_start = now_ms();
    nrq_gc_mutator_leave();
    const double publish_elapsed = now_ms() - publish_start;
    report("publish 1M deferred objects", publish_elapsed, kSmall);

    cursor = nullptr;
    nrq_gc_root_unregister(&cursor);
    nrq_gc_collect();
    nrq_gc_mutator_enter();  // restore the probe activation for later phases
  }

  // Read-only pointer chase through the same Node shape used by the suite.
  {
    const size_t count = 1'000'000;
    Node* nodes = static_cast<Node*>(std::calloc(count, sizeof(Node)));
    for (size_t i = 0; i + 1 < count; ++i) nodes[i].next = &nodes[i + 1];
    int64_t total = 0;
    const double start = now_ms();
    Node* cursor = nodes;
    for (size_t i = 0; i < count; ++i) {
      total += cursor->value;
      cursor = static_cast<Node*>(cursor->next);
    }
    const double elapsed = now_ms() - start;
    report("raw pointer chase (baseline)", elapsed, count);
    std::printf("checksum=%lld\n", static_cast<long long>(total));
    std::free(nodes);
  }

  nrq_gc_mutator_leave();
  return 0;
}
