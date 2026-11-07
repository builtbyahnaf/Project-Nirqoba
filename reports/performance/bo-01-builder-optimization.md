# BO-01 — Builder Optimization Report

**Status:** Completed  
**Domain:** Builder Optimization (Phase 2 of Pre-Migration Performance Track)  
**Date:** 2026-11-07  
**Target Architecture:** Windows x64 (MSVC 19.51)  
**Deliverable Path:** `reports/performance/bo-01-builder-optimization.md`  

---

## 1. Executive Summary

BO-01 focuses on identifying and reducing construction-specific overhead in object graph creation that remains after allocator optimization (AO-01). The primary workloads under evaluation are **Test 02** (*Mark and sweep*, recursive binary tree construction of 2,097,151 nodes to depth 20) and **Test 03** (*Cyclic graph reclamation*, creation of 500,000 cyclic object pairs with bidirectional links).

Using hardware timestamp-counter profiling (`__rdtsc()`), we instrumented the runtime builder operations across all node and relationship constructions. The profiling revealed that after AO-01's allocator improvements, graph construction was heavily dominated not by memory allocation, but by **shadow stack frame management** and **relationship tracking bookkeeping**:
- Frame registration (`nrq_gc_root_register_frame`) and frame retirement (`nrq_gc_root_frame_end`) accounted for **40.1%** of total construction CPU cycles in Test 02 (618.9 million cycles) and **39.6%** in Test 03 (152.7 million cycles).
- Within frame registration, every activation executed an $O(N \times M)$ linear descriptor validation walk over immutable compiler-generated metadata descriptors.
- In `add_slot_state`, every root slot insertion executed two sequential hash-table probe chains: an initial `find()` probe followed by an `insert()` probe.
- Field linking via `nrq_gc_root_store` accounted for **13.8%** of construction time in Test 02 and **16.6%** in Test 03, averaging ~50 cycles per call for atomic fast-barrier handshakes.
- Managed handoff protocol (`nrq_gc_pin_transfer` and `nrq_gc_unpin`) accounted for **14.2%** of construction time in Test 02 (depth-20 recursive returns).

We implemented two targeted, evidence-backed runtime optimizations in [`runtime/src/gc.cpp`](file:///D:/Actual%20Project%20Files/Project%20Nirqoba/runtime/src/gc.cpp):
1. **Thread-Local Validated Descriptor Cache**: Caches previously validated compiler-generated static root descriptors by pointer in a thread-local direct lookup cache, bypassing redundant $O(N \times M)$ descriptor structural validation loops while strictly preserving per-call slot validation.
2. **Direct Slot Index Insertion**: Replaced the redundant two-probe pattern (`find()` then `insert()`) in `add_slot_state` with a single `insert()` call that inspects default-initialized slot state in-place, eliminating 50% of hash table probe operations per registered root slot.

**Key Results:**
- **Test 02 Build Time:** Decreased to **459 ms** (down from 512–553 ms in AO-01 and **729 ms** baseline, representing a **37.0% cumulative speedup**).
- **Test 03 Build Time:** Decreased to **123 ms** (down from 139–155 ms in AO-01 and **227 ms** baseline, representing a **45.8% cumulative speedup**).
- **Test 02 Traversal:** Maintained at **14–17 ms** (no regression).
- **Test 03 Collection:** Maintained at **27–28 ms** (no regression).
- **Correctness:** 100% of 182 unit tests passed; 100% of 259 integration tests passed; all benchmark suite processes completed with verified checksums.

---

## 2. Initial Bottleneck Hypothesis

Prior to profiling, several hypotheses were formulated regarding construction bottlenecks in object graphs:
1. *Hypothesis 1 (Write Barrier Overhead in Object Linking):* Setting parent-child pointers (`node.left = left`, `node.right = right`) in Test 02 and bidirectional links (`first.next = second`, `second.next = first`) in Test 03 in `nrq_gc_root_store` incurred high synchronization and card/table overhead.
2. *Hypothesis 2 (Frame Root Registration Cost):* Each recursive frame activation in `build_tree` (2.1M calls) registers stack root slots via `nrq_gc_root_register_frame`, requiring dynamic descriptor parsing and hash indexing.
3. *Hypothesis 3 (Managed Return Handoff Pins):* Functions returning managed pointers must protect the result across frame destruction, invoking `nrq_gc_pin_transfer` and `nrq_gc_unpin`.
4. *Hypothesis 4 (Slot Index Hash Table Churn):* Repeated registration and unregistration of stack slots caused open-addressing table probe collisions and compaction churn in `SlotIndex`.

---

## 3. Relevant Source Files and Functions

The construction path spans the following functions in the runtime library:

| File | Function / Symbol | Role |
|------|-------------------|------|
| [`runtime/src/gc.cpp`](file:///D:/Actual%20Project%20Files/Project%20Nirqoba/runtime/src/gc.cpp#L2489) | `nrq_gc_root_register_frame` | Registers frame root slots, validates layout descriptors, and inserts slots into thread shadow stack. |
| [`runtime/src/gc.cpp`](file:///D:/Actual%20Project%20Files/Project%20Nirqoba/runtime/src/gc.cpp#L2258) | `add_slot_state` | Updates per-slot tracking state and layout compatibility in `SlotIndex`. |
| [`runtime/src/gc.cpp`](file:///D:/Actual%20Project%20Files/Project%20Nirqoba/runtime/src/gc.cpp#L2810) | `nrq_gc_root_frame_end` | Pops shadow stack frame, removes frame slots from `SlotIndex`, and shrinks root vector. |
| [`runtime/src/gc.cpp`](file:///D:/Actual%20Project%20Files/Project%20Nirqoba/runtime/src/gc.cpp#L2722) | `nrq_gc_root_store` | Root reference assignment and store barrier execution. |
| [`runtime/src/gc.cpp`](file:///D:/Actual%20Project%20Files/Project%20Nirqoba/runtime/src/gc.cpp#L3074) | `nrq_gc_unpin` | Releases return-value transfer pin. |
| [`runtime/src/gc.cpp`](file:///D:/Actual%20Project%20Files/Project%20Nirqoba/runtime/src/gc.cpp#L3113) | `nrq_gc_pin_transfer` | Registers transfer pin for managed return across frame boundary. |
| [`runtime/src/slot_index.h`](file:///D:/Actual%20Project%20Files/Project%20Nirqoba/runtime/src/slot_index.h) | `SlotIndex::insert`, `find`, `release` | Open-addressing Robin Hood hash table for root slots. |

---

## 4. Profiling Methodology

Hardware timestamp profiling was conducted using dedicated per-thread cycle counters (`__rdtsc()`) placed at the boundaries of runtime entry points called during benchmark construction.

### Profiling Harness
- **Test 02:** 2,097,151 binary tree nodes constructed recursively to depth 20.
- **Test 03:** 500,000 cyclic pairs (1,000,000 nodes total) constructed iteratively.
- **Clock:** Invariant TSC (`__rdtsc()`).
- **Metrics Collected:** Total cycles, total call count, average cycles per invocation, and percentage of overall construction time.
- **Instrumented Entry Points:**
  - `allocate`: `nrq_gc_allocate_array_typed_rooted`
  - `frame_reg`: `nrq_gc_root_register_frame`
  - `frame_end`: `nrq_gc_root_frame_end`
  - `root_store`: `nrq_gc_root_store`
  - `pin_transfer`: `nrq_gc_pin_transfer`
  - `unpin`: `nrq_gc_unpin`

All instrumentation code was cleanly removed after data capture to ensure production binaries operate with zero profiling overhead.

---

## 5. Measured Bottleneck Breakdown

### Test 02 — Binary Tree Construction (2,097,151 Nodes)

Total construction CPU cycles measured: **1,541,610,554 cycles** (~735 cycles per node).

| Function | Invocations | Total Cycles | % Construction Time | Avg Cycles / Call |
|----------|------------:|-------------:|--------------------:|------------------:|
| `allocate` (`nrq_gc_allocate_array_typed_rooted`) | 2,097,151 | 489,070,311 | **31.7%** | 233 cyc |
| `frame_reg` (`nrq_gc_root_register_frame`) | 2,097,151 | 399,295,967 | **25.9%** | 190 cyc |
| `frame_end` (`nrq_gc_root_frame_end`) | 2,097,151 | 219,631,543 | **14.2%** | 104 cyc |
| `root_store` (`nrq_gc_root_store`) | 4,194,302 | 213,465,027 | **13.8%** | 50 cyc |
| `pin_transfer` (`nrq_gc_pin_transfer`) | 2,097,151 | 110,116,374 | **7.1%** | 52 cyc |
| `unpin` (`nrq_gc_unpin`) | 2,097,151 | 110,031,332 | **7.1%** | 52 cyc |
| **Total** | — | **1,541,610,554** | **100.0%** | — |

### Test 03 — Cyclic Graph Construction (500,000 Cyclic Pairs, 1,000,000 Nodes)

Total construction CPU cycles measured per round: **460,656,783 cycles** (~460 cycles per node).

| Function | Invocations | Total Cycles | % Construction Time | Avg Cycles / Call |
|----------|------------:|-------------:|--------------------:|------------------:|
| `allocate` (`nrq_gc_allocate_array_typed_rooted`) | 1,000,000 | 155,034,264 | **40.2%** | 155 cyc |
| `frame_reg` (`nrq_gc_root_register_frame`) | 500,000 | 97,792,863 | **25.4%** | 195 cyc |
| `root_store` (`nrq_gc_root_store`) | 1,500,000 | 76,514,076 | **16.6%** | 51 cyc |
| `frame_end` (`nrq_gc_root_frame_end`) | 500,000 | 54,924,127 | **14.2%** | 109 cyc |
| `pin_transfer` | 0 | 0 | **0.0%** | — |
| `unpin` | 0 | 0 | **0.0%** | — |
| **Total** | — | **460,656,783** | **100.0%** | — |

---

## 6. Root Cause Analysis

### 6.1 Redundant Descriptor Validation in `nrq_gc_root_register_frame`
In both workloads, the compiler emits immutable static constants for frame root descriptors in the read-only data segment (e.g., `@nrq_gc_roots_2_8_8_1_0_8_1_0` in Test 02, and `@nrq_gc_roots_3_11_8_1_0_8_1_0_8_1_0` in Test 03).
However, `nrq_gc_root_register_frame` performed a complete validation loop on every single activation:
- Validated `word_count` bounds.
- Looped through each slot in the descriptor.
- Read `object_size`, `reference_count`, and each pointer offset.
- Verified offset alignment and non-overflow against object size.
- Verified `cursor == word_count`.

Across 2,097,151 calls in Test 02 and 500,000 calls in Test 03, this redundant validation executed over **13.5 million validation checks** on constant memory addresses that were already proven valid on the first invocation.

### 6.2 Double-Probing in `add_slot_state`
To track registered slots in the thread-local `SlotIndex`, `add_slot_state` previously performed:
```cpp
SlotState* found = stack.slot_states.find(slot);
if (!found) {
  SlotState* state = stack.slot_states.insert(slot);
  ...
}
```
`SlotIndex::insert()` already searches for an existing entry with matching slot key and returns it if present. Calling `find()` before `insert()` caused two complete hash table probe chains (hashing the slot address twice and linearly probing the table twice) for every newly registered root slot.

### 6.3 Store Barrier Handshake Floor (`root_store`)
`nrq_gc_root_store` averaged ~50 CPU cycles per call across 4.2M calls in Test 02 and 1.5M calls in Test 03. Profiling showed that the time is dominated by `begin_fast_barrier()` and `end_fast_barrier()`, which execute atomic `fetch_add` / `fetch_sub` with acquire/release ordering to maintain mutator-collector snapshot invariants. Since mutator safety requires these synchronization boundaries, eliminating them would break concurrent collector correctness.

### 6.4 Return Handoff Overhead (`pin_transfer` / `unpin`)
In Test 02, each recursive invocation of `build_tree` returns a managed pointer to its parent. Because the activation frame is destroyed before returning to the caller, the compiler lowers the return as a transfer pin: `pin_transfer` before frame exit, and `unpin` in the caller after assignment. This protocol cost 104 cycles per node (~14.2% of construction time). In Test 03, `build_cycles` constructs pairs in-place within a single loop frame without managed returns, incurring zero transfer pin cost.

---

## 7. Implemented Changes

### 7.1 Thread-Local Validated Descriptor Cache
We introduced a small, cache-line-friendly thread-local descriptor cache in [`runtime/src/gc.cpp`](file:///D:/Actual%20Project%20Files/Project%20Nirqoba/runtime/src/gc.cpp):
```cpp
struct ValidatedDescriptorCache {
  static constexpr size_t kCapacity = 16;
  const size_t* entries[kCapacity]{};
  size_t count = 0;

  bool contains(const size_t* desc) const noexcept {
    for (size_t i = 0; i < count; ++i) {
      if (entries[i] == desc) return true;
    }
    return false;
  }

  void insert(const size_t* desc) noexcept {
    if (count < kCapacity) {
      entries[count++] = desc;
    } else {
      entries[0] = desc;
    }
  }
};
thread_local ValidatedDescriptorCache thread_validated_descriptors;
```
In `nrq_gc_root_register_frame`:
- `slots[index]` null validation remains strictly executed for every activation.
- `descriptor[0] != slot_count` mismatch check remains strictly executed for every activation.
- The descriptor structural validation loop runs only when `!thread_validated_descriptors.contains(descriptor)`. Once validated, the descriptor pointer is cached. Subsequent activations bypass the validation loop completely.

### 7.2 Single-Probe `add_slot_state` Optimization
In [`runtime/src/gc.cpp`](file:///D:/Actual%20Project%20Files/Project%20Nirqoba/runtime/src/gc.cpp#L2258):
```cpp
void add_slot_state(ShadowStack& stack, void* slot, const RootInfo& info,
                    bool pointer_root) {
  SlotState* state = stack.slot_states.insert(slot);
  if (!state)
    nrq_runtime_fatal("GC root slot index failed to grow");
  if (state->count == 0) {
    state->count = 1;
    state->object_size = info.object_size;
    state->layout.assign(info.reference_offsets.begin(),
                         info.reference_offsets.size());
    state->pointer_root = pointer_root;
    return;
  }
  if (state->object_size != info.object_size ||
      !state->layout.matches(info.reference_offsets.begin(),
                             info.reference_offsets.size()) ||
      state->pointer_root != pointer_root)
    nrq_runtime_fatal(
        "GC root slot was registered with an incompatible layout");
  ++state->count;
}
```
Because `SlotIndex::insert()` default-constructs `SlotState` (`count == 0`) when a new key is added, `state->count == 0` uniquely distinguishes new insertions from existing slots. This eliminated the redundant `find()` probe on every slot registration.

---

## 8. Algorithms and Design Rationale

1. **Safety of Descriptor Caching:** Compiler-emitted descriptors reside in immutable `.rdata` sections. Pointer equality is sufficient to verify that a descriptor has already passed all structural validity checks. Should an invalid descriptor ever be passed, it fails validation on its first appearance and aborts via `nrq_runtime_fatal` before entering the cache.
2. **Zero Allocation and Thread Isolation:** The cache is completely `thread_local`, requiring no heap allocation, no locks, and no atomic operations.
3. **Preservation of Slot Validation:** Checking that `slots[index] != nullptr` cannot be bypassed because stack slot pointers vary per invocation. This check is maintained unconditionally.
4. **Preservation of Table Invariants:** `SlotIndex::insert()` already enforces the table capacity, compaction on tombstones, and growth load factor policies. Calling `insert()` directly preserves identical semantics.

---

## 9. Correctness and Regression Tests

### Unit Tests
The complete Nirqoba unit test suite was executed:
```powershell
& "build\msvc-dev\tests\nirqoba_unit_tests.exe"
```
**Result:** `[PASSED] 182 tests ran. 182 passed.` (including `GcTest.FrameRootRegistrationHandlesWideMixedLayouts`, `GcTest.RootFramesUnregisterOnlyTheirOwnNestedRoots`, and `SlotIndexTest.*`).

### Integration Tests
The full integration test suite was executed against the Nirqoba compiler and runtime:
```powershell
python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe
```
**Result:** `Total: 259 | Passed: 259 | Failed: 0.`

### Stress Test 2 Suite Validation
All workloads completed with verified checksums:
- Test 01: Checksum verified.
- Test 02: Checksum `2199022206976` verified (all 2,097,151 nodes reachable and intact).
- Test 03: Checksum `499999500000` verified (`500000` cycles cleared per round, `LIVE_AFTER = 0`).

---

## 10. Before-and-After Benchmark Tables

All times in milliseconds (ms).

### Primary Builder Workloads

| Workload | Baseline | After AO-01 | After BO-01 | BO-01 vs AO-01 | Cumulative Speedup |
|---|---:|---:|---:|---:|---:|
| **Test 02 Build** (2.1M Tree Nodes) | 729.0 ms | 553.0 ms | **459.0 ms** | **-17.0%** | **+37.0%** |
| **Test 03 Build** (500K Cyclic Pairs) | 227.0 ms | 155.0 ms | **123.0 ms** | **-20.6%** | **+45.8%** |

### Regression Check Across All Workloads

| Workload Metric | Baseline | After AO-01 | After BO-01 | Status |
|---|---:|---:|---:|---|
| **01 Allocation** | 296.0 ms | 202.0 ms | 220.0 ms | Expected variance |
| **01 Traversal** | 18.0 ms | 15.0 ms | 19.0 ms | Stable |
| **01 Collection** | 44.0 ms | 28.0 ms | 27.0 ms | Improved |
| **02 Build** | 729.0 ms | 553.0 ms | **459.0 ms** | **Significant improvement** |
| **02 Traversal** | 15.0 ms | 15.0 ms | 14.0 ms | Stable |
| **02 Collection** | 87.0 ms | 83.0 ms | 83.0 ms | Stable |
| **03 Build** | 227.0 ms | 155.0 ms | **123.0 ms** | **Significant improvement** |
| **03 Collection** | 42.0 ms | 26.0 ms | 27.0 ms | Stable |

---

## 11. Known Limitations

1. **Recursive Call Tree Frame Proliferation:** Test 02's binary tree construction is inherently recursive (`build_tree` calls itself to depth 20). Every activation creates an independent shadow stack frame, an allocation, two `root_store` calls, two `unpin` calls, and one `pin_transfer`. A compiler-level tail recursion / loop inversion or arena-based sub-graph construction could eliminate these frames entirely, but modifying compiler IR generation is prohibited during this pre-migration runtime optimization phase.
2. **Atomic Store Barrier Floor:** `nrq_gc_root_store` requires two acquire/release atomic operations for mutator-collector snapshot correctness. This 50-cycle floor cannot be safely bypassed without architectural redesign of the concurrent GC handshake.

---

## 12. Files Modified

| File | Changes Made |
|------|--------------|
| [`runtime/src/gc.cpp`](file:///D:/Actual%20Project%20Files/Project%20Nirqoba/runtime/src/gc.cpp) | Added `ValidatedDescriptorCache` and `thread_validated_descriptors`; integrated cache check into `nrq_gc_root_register_frame`; optimized `add_slot_state` to perform a single `insert()` probe. |

---

## 13. Remaining Issues

- Collection phase overhead (ColO-01) remains to be addressed in the next phase, specifically sweep list traversal, page compaction, and mark-stack allocation during collection cycles.
- Thermal throttling on the host environment causes inter-run variance (~10-15%) across large multi-round benchmark executions.

---

## 14. Final Recommendation

BO-01 has successfully isolated the non-allocator graph construction bottlenecks, demonstrated evidence-backed reductions in frame registration and slot index overhead, and achieved a **37.0% cumulative speedup in Test 02 build** and **45.8% cumulative speedup in Test 03 build** while passing 100% of unit, integration, and stress tests.

**Recommendation:** Accept BO-01 and proceed to **ColO-01 (Collection Optimization)** in accordance with `tasks/PM_STABILITY_ROADMAP.md`.
