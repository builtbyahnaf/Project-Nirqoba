# Stress Test 2 - GC Cost Investigation

**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)  
**Date:** 2026-11-04  
**Scope:** Why managed allocation costs ~1.1 us/node and managed traversal
~215 ns/node (chain) / ~954 ns/node (tree) in Nirqoba 0.105-beta  
**Status:** Diagnosis only; no compiler or runtime code was modified

---

## 1. Summary

The cost is **not** in LLVM optimization, pointer representation, or a missed
optimization pass. The optimized IR contains plain pointer loads and the
traversal loops are already minimal. The cost is in the **GC protocol emitted
around every allocation, every managed reference store, and every function
activation**, plus the fact that a managed return **defeats TLAB batching** by
publishing the pending allocation on every call.

Three root causes account for essentially all of the measured time:

1. **A fixed GC prologue/epilogue runs on every GC-capable function call.**
   `make_node` and each recursive `sum_tree` activation call
   `nrq_gc_mutator_enter`, `nrq_gc_root_frame_begin`, two
   `nrq_gc_root_register_typed` calls, `nrq_gc_root_copy`/`nrq_gc_root_store`,
   `nrq_gc_root_frame_end`, and `nrq_gc_mutator_leave`. Most of these take a
   global mutex, and root registration inserts/erases a node in a global
   `std::unordered_map`, causing heap allocation churn per call.
2. **Every managed reference store pays a generational remembered-set barrier
   that is enabled by default and is conservative.** Even a young-to-young
   store performs `object_index.find(value)` plus
   `remembered_young.insert(header)`. Over 1M distinct young objects this costs
   ~113 ns per `root_store` and ~142 ns per `root_copy`. In these benchmarks no
   object is ever promoted, so the barrier records information that a minor
   collection does not need.
3. **A managed return publishes the TLAB on every call.** `nrq_gc_pin_transfer`
   calls `publish_tlab_locked`, which hash-inserts every pending object into
   `object_index`. Because `make_node` returns a managed pointer, the pending
   list contains exactly one object at each return, so the TLAB never batches:
   each node pays a publication insert and a pin/unpin round trip on top of the
   allocation.

The previously-added TLAB and open-addressing object-index optimizations made
the *constant factors* of allocation and lookup better, but they did not change
the *number* of lock acquisitions, hash insertions, and remembered-set updates
per node. Those counts, not the container implementation, dominate.

Direct probes linked against the same `nrqoba_runtime.lib` reproduce both
benchmark results: a faithful `sum_tree` IR replica costs **924 ns/node**
(benchmark 954 ns) and a faithful `build_chain` replica costs ~**870 ns/node**
(benchmark 1,147 ns, which has high run-to-run variance).

---

## 2. Method

The investigation used three independent sources of evidence:

1. **Compiler output.** `nirqoba build ... -O3 --emit=llvm` for tests 01 and
   02. The optimized module was inspected for the exact call sequence, pointer
   loads, and declarations. `-O3` does run the LLVM O3 pipeline; the GC calls
   survive because they are opaque external declarations.
2. **Runtime call microbenchmarks.** Small C++ probes compiled with MSVC
   `/O2 /MD /std:c++20` and linked against the same
   `build/msvc-dev/lib/nrqoba_runtime.lib` and `runtime/include`. Each probe
   times one runtime operation, or a faithful reconstruction of one emitted
   sequence, over one million operations.
3. **Layout probes.** Consecutive allocations were used to measure the real
   object stride, and `sizeof` confirmed the payload sizes.

Probe sources are in `benchmarks/stress_test_2/investigation/`. They are
diagnostic programs, not part of the measured suite, and they do not alter
`benchmark_results.json`.

Build command (from a Developer Command Prompt):

```bat
cl /O2 /MD /nologo /std:c++20 /EHsc ^
   /I "runtime\include" ^
   "benchmarks\stress_test_2\investigation\traversal_probe.cpp" ^
   /Fe:"traversal_probe.exe" ^
   /link "build\msvc-dev\lib\nrqoba_runtime.lib"
```

Absolute numbers vary with host state and heap reuse. The benchmark's own
allocation samples range from 1,117 ms to 2,371 ms (median 1,147 ms), so the
attribution below relies on differences measured within the same probe, not on
cross-run absolute values.

---

## 3. What the generated code actually does

### 3.1 Allocation: `make_node` (test 01)

```llvm
define ptr @make_node(i64 %0) {
  tail call void @nrq_gc_mutator_enter()
  %6 = tail call i64 @nrq_gc_root_frame_begin()
  call void @nrq_gc_root_register_typed(ptr %5, i64 8, ptr %3, i64 1)
  call void @nrq_gc_root_register_typed(ptr %4, i64 8, ptr %2, i64 1)
  %8 = call ptr @nrq_gc_allocate_array_typed_rooted(ptr %5, i64 1, i64 16, ptr %7, i64 1)
  call void @nrq_gc_root_store(ptr %4, ptr %8)
  %11 = load ptr, ptr %4
  %12 = call ptr @nrq_gc_pin_transfer(ptr %11)
  call void @nrq_gc_root_frame_end(i64 %6)
  call void @nrq_gc_mutator_leave()
  ret ptr %11
}
```

The caller (`build_chain`) then adds, per iteration:

```llvm
%17 = call ptr @make_node(i64 %.07)
call void @nrq_gc_root_copy(ptr %7, ptr %6, i64 8)
%18 = load ptr, ptr %7
call void @nrq_gc_unpin(ptr %18)
%19 = load ptr, ptr %8
call void @nrq_gc_root_store(ptr %19, ptr %17)   ; tail->next = new node
%20 = load ptr, ptr %8
%.unpack = load ptr, ptr %20
call void @nrq_gc_root_store(ptr nonnull %8, ptr %.unpack)
call void @nrq_gc_safepoint()
```

Per allocated node this is **11 mutex-protected runtime calls** (two root
registrations, two rooted-allocation lock sections, a local `root_store`, a
`pin_transfer`, `root_frame_end` unregistering two roots, a caller
`root_copy`, an `unpin`, and two field/tail `root_store`s), plus
`mutator_enter`/`mutator_leave` and `safepoint` on a second mutex. Four of those
calls run the generational barrier, and one call inserts the object into the
global object index.

### 3.2 Traversal: `sum_chain` (test 01)

```llvm
.lr.ph:
  %13 = load ptr, ptr %7          ; load cursor from root slot
  %.unpack = load ptr, ptr %13     ; cursor->next
  %.elt6 = getelementptr ..., i64 8
  %.unpack7 = load i64, ptr %.elt6 ; cursor->value
  %14 = add i64 %.unpack7, %.058
  call void @nrq_gc_root_store(ptr nonnull %7, ptr %.unpack)
  %15 = add nuw nsw i64 %.09, 1
  call void @nrq_gc_safepoint()
  %exitcond.not = icmp eq i64 %15, %1
```

The loads are plain: there is **no read barrier** and no runtime lookup on a
managed-reference load. The instrumentation is one `root_store` (write barrier
+ generational remembered set) and one `safepoint` per iteration.

### 3.3 Traversal: `sum_tree` (test 02)

```llvm
define i64 @sum_tree(ptr %0, i64 %1) {
  tail call void @nrq_gc_mutator_enter()
  %8 = tail call i64 @nrq_gc_root_frame_begin()
  call void @nrq_gc_root_register_typed(ptr %7, i64 8, ptr %5, i64 1)
  call void @nrq_gc_root_copy(ptr %7, ptr %4, i64 8)      ; copy parameter
  call void @nrq_gc_root_register_typed(ptr %6, i64 8, ptr %3, i64 1)
  call void @nrq_gc_root_store(ptr %6, ptr %0)            ; second root
  %10 = load ptr, ptr %6
  ...
  %14 = call i64 @sum_tree(ptr %.unpack, i64 %13)
  %16 = load ptr, ptr %6                                 ; reload after call
  %17 = call i64 @sum_tree(ptr %.unpack9, i64 %13)
  call void @nrq_gc_root_frame_end(i64 %8)
  call void @nrq_gc_mutator_leave()
}
```

Unlike `sum_chain`, `sum_tree` has **no safepoint**, but every recursive call
pays the full GC prologue/epilogue: two root registrations, a `root_copy`, a
`root_store`, a frame begin/end, and mutator enter/leave. It also registers a
second root slot (`%6`) that duplicates the parameter already copied into `%7`.
That second root is written through `root_store` and then read on every
dereference.

### 3.4 Are the LLVM passes running?

Yes. `Compiler/lib/Backend/Optimization.cpp` builds an LLVM `PassBuilder` O3
pipeline, and `--emit=llvm` output is the optimized module. The GC calls remain
because all of them are ordinary external declarations with **no attributes**:

```llvm
declare void @nrq_gc_safepoint() local_unnamed_addr
declare void @nrq_gc_root_store(ptr, ptr) local_unnamed_addr
declare void @nrq_gc_root_register_typed(ptr, i64, ptr, i64) local_unnamed_addr
```

No `nounwind`, no `memory(...)` effects. LLVM must treat each call as a full
memory clobber, which is why the IR reloads root slots after every call. This is
correct but prevents the optimizer from proving that any of the bookkeeping is
redundant.

---

## 4. Allocation measurements

One million `Node` objects (16-byte payload, one managed reference) through the
real runtime, in fresh processes, all values in ns per object:

| Operation | Time | Notes |
|---|---:|---|
| Raw allocation: TLAB bump + 112-byte header construction + 16-byte zero + page commit | **68-73** | No publication |
| Bulk publication at thread leave: `object_index.insert` + object list + statistics | **67-71** | One lock for the whole batch |
| Unrooted allocation + bulk publication | **142** | 70 + 71 |
| Rooted allocation (`allocate_array_typed_rooted`) + bulk publication | **176-182** | Adds two `gc_mutex` root-validation sections (~35) |
| Rooted allocation + per-return `pin_transfer` + `unpin` | **285-313** | The benchmark's actual pattern |

Interpretation:

- **Raw allocation is cheap.** The TLAB bump, header initialization, zeroing,
  and page commit together are under 75 ns. The TLAB is working as a bump
  allocator.
- **Publication is the expensive part.** Inserting one million objects into the
  open-addressing index costs ~70 ns per object even when done in one batch.
- **Per-return publication doubles that.** `pin_transfer` publishes the pending
  TLAB on every managed return. Since `make_node` returns one object, the batch
  is always one object, and the per-object publish/pin/unpin path adds
  ~110-135 ns over the deferred case. Test 03 corroborates this: its
  `make_one_cycle` returns `void`, so it publishes in bulk and allocates one
  million objects in **803 ms** (~803 ns/object) versus test 01's
  **1,147 ms** (~1,147 ns/object), even though test 03 amortizes the function
  prologue over two allocations.

The benchmark's 1,147 ns/object is the sum of raw allocation (~70), per-return
publication and pin/unpin (~130), the generational write barriers on the local
root store, caller `root_copy`, and two field stores (~4 x 113-142), the fixed
per-call root/frame/mutator bookkeeping (~200-250), and the loop safepoint
(~13).

### 4.1 Metadata and zeroing

`ObjectHeader` is **112 bytes**:

```cpp
ObjectHeader* next, *mark_next, *tlab_next;   // 24
GcPage* page; void* object;                   // 16
size_t size, element_size;                    // 16
nrq_gc_trace_fn trace;                        //  8
bool marked, young; uint8_t age;              //  3 + padding
uint64_t birth_epoch;                         //  8
ReferenceLayout reference_offsets;            // 32
```

Measured allocation strides:

| Payload | `sizeof` | Allocation stride | Useful fraction |
|---|---:|---:|---:|
| `Node` (one managed ref + int) | 16 | **128** | 12.5% |
| `TreeNode` (two managed refs + int) | 24 | **136** | 17.6% |

`ReferenceLayout` keeps up to two offsets inline, so a one- or two-reference
object does **not** allocate a separate metadata vector. That optimization is
working. The remaining 112-byte header is still large: it forces a 128-byte
stride for a 16-byte payload and makes allocator and cache behavior worse than
the payload requires. Zeroing (`std::memset(object, 0, bytes)`) is only 16-24
bytes and is not a dominant cost.

The header is also **constructed field by field on every allocation**
(`new (header_storage) ObjectHeader()`), which writes roughly 112 bytes before
the payload is zeroed. A large part of the ~70 ns raw allocation is this
metadata traffic and the page/TLAB refill, not the bump pointer itself.

---

## 5. Traversal measurements

### 5.1 Chain (test 01)

A one-million-node chain allocated through the real runtime, so the layout and
population match the benchmark. Values are ns per node:

| Sequence | Time | Delta |
|---|---:|---:|
| Raw managed-layout pointer chase | **12.0** | baseline |
| Chase + `root_store` | **131.6** | +119.6 |
| Chase + `safepoint` | **15.7** | +3.7 |
| Chase + `root_store` + `safepoint` | **176.0** | +164.0 |
| Benchmark `sum_chain` | **215** | includes call, layout, run variance |

The raw chase over GC objects is 12.0 ns/node, about 4.6x a native 16-byte
pointer chase (2.6 ns/node) because each node sits 128 bytes apart. That layout
effect is real but small compared with the write barrier: **one
`root_store` costs about 120 ns more than the loads it accompanies.**

Direct barrier measurements over one million distinct, published young objects:

| Operation | Generational ON | Generational OFF | Barrier cost |
|---|---:|---:|---:|
| `nrq_gc_root_store` | **130.8** | **18.1** | **~113** |
| `nrq_gc_root_copy` (8 bytes) | **169.4** | **27.8** | **~142** |
| `nrq_gc_pin_transfer` + `unpin` | **167.1** | - | - |
| `nrq_gc_unpin` alone | **21.2** | - | - |

With generational collection off, `root_store` is only 18 ns (a lock, a phase
check, and a store). With it on, the runtime performs
`object_index.find(value)` and `remembered_young.insert(header)` on every
non-null store. The object index has one million entries and the remembered set
grows to one million entries, so each barrier is a cache miss plus a hash
probe/insert.

Note that in this workload the source and the destination are both young. A
young-to-young store cannot require a remembered entry under a standard
generational scheme, but the runtime does not know the container's generation,
so it records the value unconditionally.

### 5.2 Tree (test 02)

A depth-20 complete tree (2,097,151 nodes) built in preorder through the real
runtime, matching the benchmark's physical layout. Values are ns per node:

| Variant | Time |
|---|---:|
| Raw recursive checksum | **7.1-11.6** |
| Faithful `sum_tree` IR replica, generational ON | **924** |
| Faithful `sum_tree` IR replica, generational OFF | **470** |
| Faithful minus the second root registration and its `root_store`, generational ON | **667** |
| Faithful minus the second root registration and its `root_store`, generational OFF | **279** |
| Benchmark `sum_tree` | **954** |

This isolates the tree gap completely:

- **Raw traversal:** ~10 ns/node.
- **GC prologue/epilogue alone** (enter/leave, frame, one root registration,
  `root_copy`, no generational barrier): ~280 ns/node.
- **The second root registration/unregistration plus its `root_store`:**
  ~190 ns/node with generational off, ~260 ns/node with it on.
- **Generational barriers:** ~450 ns/node (924 - 470).

The second root slot is redundant at the IR level: the parameter is already
copied into `%7`, but the compiler also registers `%6` and stores the same
parameter there for every dereference. Each registration and unregistration
mutates a global `std::unordered_map<void*, RootInfo>`, so the call performs a
heap allocation and a free. At recursion depth 20 that map is being inserted
and erased continuously, which is why a root registration costs more inside the
recursion than in a flat loop.

### 5.3 Object layout and pointer representation

Pointer fields are plain 8-byte pointers; there is no compressed or tagged
representation and no indirection. The layout penalty is a secondary effect:
`TreeNode` is 136 bytes with `left` at offset 112, `right` at 120, and `value`
at 128, so a node's hot fields span two 64-byte cache lines, and the 136-byte
stride makes consecutive nodes straddle lines irregularly. This contributes to
the 7-12 ns raw traversal, not the remaining ~900 ns.

---

## 6. Why the TLAB and object-index optimizations did not fix this

Both optimizations improved the wrong axis:

- **TLABs** removed the lock and index insertion from the *bump* path, but the
  publication path is still executed once per object because
  `nrq_gc_pin_transfer` calls `publish_tlab_locked`. For code that returns
  managed values, the TLAB batch size is effectively one. The benchmark's
  helper-return shape is exactly that case.
- **The open-addressing object index** removed one heap node per object and
  improved lookup locality, but the workload still performs one insert to
  publish plus several lookup/insert operations for barriers *per node*. The
  number of operations dominates the container constant.
- **The generational remembered set** is enabled by default
  (`generational_collection_enabled{true}`) and its barrier is unconditional
  for every non-null store. These benchmarks disable automatic collection and
  never run a minor collection before traversal, so no object is ever promoted
  and no remembered entry is ever needed. The barrier is pure overhead for the
  measured phases.

---

## 7. Prioritized remediation options

These are options, not changes made during this investigation. The first three
address the measured bottlenecks; the rest reduce fixed overhead and improve
future headroom.

1. **Make the write-barrier and safepoint fast paths lock-free when the
   collector is idle.** `nrq_gc_root_store` and `nrq_gc_root_copy` take
   `gc_mutex` on every call even when `collection_phase == Idle`, and
   `nrq_gc_safepoint` takes `mutator_mutex` on every loop back edge even when
   no collection has been requested. A flag check before locking (with a safe
   handoff to the slow path when a cycle starts) removes most of the ~11 lock
   acquisitions per allocated node and the two per traversed node. This
   requires a careful barrier/epoch design so a store racing with the start of
   a cycle is still observed.
2. **Skip the generational barrier until an old generation exists.**
   Track whether any object has ever been promoted (or whether a minor
   collection has run). While every live object is young, a young-to-young
   store needs no remembered entry, so `remember_young_locked` can return
   immediately without `object_index.find` or `remembered_young.insert`. This
   alone removes ~113 ns per `root_store` and ~142 ns per `root_copy` in tests
   01 and 02.
3. **Decouple managed-return handoff from TLAB publication.**
   `pin_transfer` should make the returned object visible without flushing the
   whole pending list, or the collector should treat pinned-but-unpublished
   objects as roots so publication can stay lazy until the TLAB fills or the
   mutator leaves. This restores real batching for helper functions that return
   managed pointers. It is the difference between ~71 ns of bulk publication
   and ~110-135 ns of per-return publication per object. Note also that
   `nrq_gc_pin_transfer` publishes the object and then immediately performs
   `object_index.find(object)`; the insert already knows the header, so that
   lookup is redundant in the publish path.
4. **Remove the redundant root registration.** `sum_tree` registers two roots
   per activation; the second duplicates the parameter. A peephole over the GC
   root operations, or a lowering that reuses one slot for the parameter and
   the local, removes ~190 ns/node from the tree traversal. More generally,
   collapse all roots of a frame into one registration record so a frame pays
   one lock instead of N.
5. **Replace the global root map with a per-thread flat stack.**
   `std::unordered_map<void*, RootInfo> roots` plus
   `std::vector<void*> thread_roots` causes a heap allocation and free on every
   register/unregister and a global lock on every operation. A per-thread
   arena/stack of root descriptors makes registration a pointer bump and
   unregistration a stack pop, and removes the global contention point. Frame
   begin/end already has a thread-local frame stack, so this fits the existing
   design.
6. **Use a thread-local frame marker instead of a global atomic.**
   `nrq_gc_root_frame_begin` performs a global compare-exchange on
   `next_root_frame_marker` for every activation. A thread-local monotonic
   counter (with a per-thread tag if markers must be unique) removes that
   atomic from the hot path.
7. **Give the GC runtime functions LLVM attributes and consider LTO.**
   Adding `nounwind` and accurate `memory(...)` effects to the declarations
   would let LLVM stop treating every GC call as a full clobber and remove the
   root-slot reloads it currently emits after each call. Inlining the fast paths
   (through LTO or by emitting the idle-path checks inline) would let the
   optimizer coalesce the bookkeeping. This is low risk but worth less than
   items 1-3.
8. **Shrink the object header.** At 112 bytes it is 87.5% of a 128-byte `Node`
   allocation. Packing the flags, removing fields that are only needed during a
   cycle, and separating cold metadata would improve allocation density, cache
   behavior, and marking. Zeroing is already cheap; the win is layout, not
   memset.

---

## 8. What is not the cause

- **LLVM optimization is not being skipped.** `-O3` runs the O3 pipeline; the
  optimized IR is what was inspected.
- **Managed-reference loads are not instrumented.** They compile to plain
  `load ptr`. There is no read barrier, card lookup, or runtime call on a load.
- **Pointer representation is not inefficient.** Fields are ordinary 8-byte
  pointers.
- **The collector itself is not the traversal cost.** Automatic collection is
  disabled during the measured phases; the traversal cost is the per-store and
  per-call protocol, not marking or sweeping.
- **The object index implementation is not the main problem.** Its constant
  factors are reasonable; the problem is that it is touched once per allocation
  and several times per node because of publication and barriers.

---

## 9. Bottom line

Nirqoba's managed traversal spends roughly **95% of its time in GC
bookkeeping** and only a few percent in the pointer chase itself. The tree case
is worse than the chain case because every recursive activation pays the full
root-frame protocol and two root registrations, and because the generational
barrier runs twice per activation. Allocation is similarly dominated by
publication and barriers: the bump allocator is fast, but the runtime publishes
one object per managed return and performs a conservative remembered-set update
on every store.

The highest-leverage fixes are, in order: lock-free idle-path barriers, gating
the generational barrier until an old generation exists, and stopping
`pin_transfer` from flushing the TLAB on every managed return.
