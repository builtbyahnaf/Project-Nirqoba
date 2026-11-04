# PO-11 — GC Root-Liveness Optimization

**Task:** P0 3.3 — GC Root-Liveness Optimization
**Date:** 2026-11-04
**Status:** Completed & Verified (correctness and execution tests passed; benchmarks deferred per policy)
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, MSVC x64 RelWithDebInfo

---

## 1. Problem Identified

Generated code published every GC-managed stack slot as a root for the whole
duration of a function activation:

- Every `Alloca` whose type could contain a managed reference was zeroed and
  registered with `nrq_gc_root_register_typed` immediately after
  `nrq_gc_root_frame_begin`, no matter where — or whether — the slot was used.
- Every parameter able to hold a managed reference received an extra stack slot,
  an `nrq_gc_root_copy` and its own registration.
- Every store of a managed pointer went through the `nrq_gc_root_store` write
  barrier, including stores into slots the collector can never observe.
- Every function activation that touched a managed value opened a root frame,
  even when the plan ended up containing no registrations at all.

Registration is not a cheap operation. `nrq_gc_root_register_typed` validates the
root descriptor, allocates a `std::vector<size_t>` for the reference offsets,
takes the shadow-stack mutex, pushes a `ShadowRoot`, updates the per-slot index,
and enters the existing collector handshake. `nrq_gc_root_frame_end` then walks
every root the frame leaves behind to undo the bookkeeping, and marking traces
every registered root on each collection. A slot that is overwritten or dead
long before the next collection paid all of that for nothing.

## 2. Root Cause

`FunctionTranslator` made root decisions per *function*, not per *slot live
range*. `needs_gc_root_frame_` was simply `uses_gc_mutator_`, and
`register_gc_root` was called unconditionally for every managed-typed `Alloca`
and every managed-typed parameter. There was no liveness information at all, so
the compiler could not distinguish a reference that must survive an allocation
from one that is already dead at that point.

## 3. Files Modified

- `compiler/lib/Lowering/TirToMlir.cpp`:
  - `CollectionSummary` / `summarize_collections`: interprocedural greatest
    fixpoint deciding which functions can run a collection.
  - `callee_is_collection_free`: the runtime routines proven never to allocate a
    managed object or start a cycle (verified against `runtime/src/gc.cpp`,
    where only `nrq_gc_allocate*` calls `maybe_collect_automatically` and only
    `nrq_gc_collect*` runs `run_collection`).
  - `ValueLiveness`: backward dataflow liveness over TIR values with an implicit
    CFG (labels split blocks, branches carry edges, sequential ops fall through).
  - `GcRootPlan` / `analyze_gc_root_plan`: per-slot and per-value root decisions,
    plus the set of stack addresses no collection can observe.
  - `FunctionTranslator`: registers only planned roots, opens a root frame only
    when the plan contains one, skips the write barrier and the root copy for
    unobserved destinations, and releases a pinned call-result handoff without
    publishing it when the result is dead at every future collection point.
- `tests/integration/mlir/gc_return_root_handoff/main.nrqba`: the buffer is now
  read after the collection so the test actually exercises a rooted handoff
  (before this change the buffer was dead and the old test only passed because
  every slot was registered unconditionally).
- New tests (see §6) and this report.

## 4. Algorithm or Transformation Implemented

### 4.1 Collection points

A collection can observe this thread's roots at exactly two kinds of program
point:

1. **A call that may collect.** Derived by greatest fixpoint: a module function
   may collect when any of its operations may, and a call to a module function
   inherits its callee's answer. Calls to runtime routines are collection points
   unless the routine is on the proven collection-free list.
2. **A loop back edge**, where the generated code polls `nrq_gc_safepoint`. A
   collection requested by another thread snapshots this thread's roots while it
   is parked, so every back edge is a collection point regardless of what the
   loop body does.

### 4.2 Backward liveness

`ValueLiveness` computes, for every operation index, the set of TIR values that
are live-in and live-out, using the standard backward equations

```
live_out(i) = union over successors of live_in(successor)
live_in(i)  = use(i) union (live_out(i) minus def(i))
```

with `use` covering the operand, lhs, rhs, call arguments and aggregate
elements. Because slot addresses are ordinary TIR values, "slot `s` is live at
`i`" means a load from `s` still happens at or after `i` on some path — exactly
the condition under which the slot's contents must survive a collection.

The fixpoint is a worklist over predecessors, so it converges in a handful of
passes even for irreducible control flow.

### 4.3 Root plan

For each `Alloca` whose type contains managed references, the analysis builds:

- `addresses(s)` = `s` plus every `GetElementPtr` derived from `s` (so struct
  fields and array elements are attributed to their slot), and
- `candidates(s)` = `addresses(s)` plus every value loaded through them.

`s` is registered as a root **iff some candidate is live-in at some collection
point**. Using live-in rather than live-out is deliberate: a value passed as an
argument to a collecting call is live *during* that call, and the callee may
collect while holding it, so a live-in test is the conservative and correct
condition.

Managed parameters and managed call results are treated the same way. A
parameter that is never live at a collection point keeps its SSA value and never
gets a root slot, and a pinned call-result handoff is released without being
published when the result is dead at every future collection point.

### 4.4 Barrier and copy elimination

Slots that fail the root test are recorded in `unobserved_addresses`. The
collector only ever reads registered slot roots, external array roots and its
own heap metadata, so memory reached through those addresses is invisible to it.
A store whose destination is such an address becomes a plain `llvm.store`
instead of `nrq_gc_root_store`, and an aggregate store becomes a plain store
instead of a temporary plus `nrq_gc_root_copy`.

The barrier is *not* removed for destinations of unknown provenance: field
stores into managed heap objects keep it, because those are exactly the
old-to-young references the generational remembered set exists to record.

### 4.5 Root frame

`needs_gc_root_frame_` is now `uses_gc_mutator_ && plan.needs_root_frame`. A
function that reads a managed reference but cannot collect no longer opens a
root frame at all.

## 5. Why the Approach is Correct

- **Every live reference stays visible.** A managed object stays reachable in
  one of three ways: a registered slot root, an external array root, or the heap
  graph reachable from them. The plan keeps a slot registered whenever a load
  from it is still needed at any point where a collection can observe it, so the
  collector's snapshot always contains every reference that is used afterwards.
- **Objects allocated after the last snapshot are safe.** The collector can only
  reclaim objects that were unreachable in its own snapshot; a slot that is not
  live at any collection point is by definition not needed after one, so
  reclaiming its contents cannot change observable behaviour.
- **Write barriers are only dropped where the collector cannot look.** The set
  of memory the collector reads is fully enumerated in `runtime/src/gc.cpp`:
  `stack.roots` (registered slots), the external `roots` table, the heap object
  index and the pinned set. `unobserved_addresses` contains only plain stack
  slots that are not registered as either kind of root, so the barrier they skip
  could never have recorded anything that marking or sweeping would read.
- **Safepoints remain.** Back-edge polls are untouched, so a long-running
  GC-using loop still parks for a collector started by another thread.
- **No new unsafe transformation.** The analysis never removes a live root, never
  moves an object between memory-management domains, and never changes what the
  runtime sees for a slot that still holds a reachable reference.

## 6. Tests Added or Modified

- `tests/integration/mlir/gc_root_liveness_dead_local/` (new): a loop whose
  per-iteration object is dead before the next allocation. Asserts that
  allocation roots and the loop safepoint are still emitted, and that
  `nrq_gc_root_store` is not.
- `tests/integration/mlir/gc_root_liveness_reader_only/` (new): a function that
  only reads a managed parameter. Asserts the module contains
  `nrq_gc_mutator_enter`/`nrq_gc_mutator_leave` but no
  `nrq_gc_root_register_typed`, no `nrq_gc_root_frame_begin` and no
  `nrq_gc_root_store`.
- `tests/integration/positive/gc_root_liveness_stress/` (new): 4096 iterations
  that each allocate a managed node while one object stays live across every
  allocation and every collection. The chain is verified after an explicit
  `gc.collect()`, and the program exits 0 only when no live object was reclaimed.
- `tests/integration/mlir/gc_return_root_handoff/main.nrqba` (modified): the
  buffer is read after `gc.collect()` so the rooted handoff path is exercised.
- **Suite results:** 116/116 unit tests and 186/187 integration tests pass; the
  single failure (`positive/memory_advanced`) is a pre-existing version-string
  drift (`0.105-beta-phase6` expected, `0.111-beta-phase6` produced by the
  runtime) unrelated to this change.

## 7. LLVM/MLIR Evidence

Root instrumentation per module (unlowered MLIR), measured with
`nirqoba build <file> --emit=mlir`:

| Program | `root_register_typed` | `root_store` | `root_copy` | `root_frame_begin` |
|---|---|---|---|---|
| `gc_managed_graph` | 6 → 4 | 5 → 4 | 1 → 0 | 2 → 2 |
| `owned_array_gc_roots` | 3 → 3 | 3 → 3 | 0 → 0 | 1 → 1 |
| `gc_aggregate_return` | 4 → 2 | 1 → 0 | 2 → 1 | 2 → 2 |
| loop with per-iteration allocation | 4 → 1 | 2 → 0 | 1 → 0 | 2 → 1 |
| `keep` live across an allocating loop | 8 → 5 | 4 → 2 | 1 → 0 | 2 → 2 |

`owned_array_gc_roots` is intentionally unchanged: every managed slot in it is
live across the explicit `gc.collect()`, so every root is required.

A function that only reads a managed parameter now lowers to plain loads and
stores around its parameter slot:

```mlir
func.func @read_only(%arg0: !llvm.ptr) -> i64 attributes {passthrough = ["nounwind"]} {
  %0 = llvm.mlir.constant(1 : i64) : i64
  %1 = llvm.alloca %0 x !llvm.ptr : (i64) -> !llvm.ptr
  llvm.call @nrq_gc_mutator_enter() : () -> ()
  %2 = llvm.mlir.zero : !llvm.ptr
  llvm.store %2, %1 : !llvm.ptr, !llvm.ptr
  llvm.store %arg0, %1 : !llvm.ptr, !llvm.ptr
  %3 = llvm.load %1 {nirqoba.ownership = "gc_managed"} : !llvm.ptr -> !llvm.ptr
  ...
}
```

## 8. Before-and-After Benchmark Results

Section 5 of the roadmap is explicitly gated on user approval, so no runtime
benchmark was executed. The measurements above are static counts of emitted root
operations and are not a claim about wall-clock performance.

## 9. Compilation-Time and Memory Impact

The liveness pass is only run for functions that actually contain a managed
slot, a managed parameter or a managed call result; every other function returns
from `analyze_gc_root_plan` after a single scan of its operations. For the
functions that do run it the cost is `O(operations x values / 64)` per iteration
with a worklist-driven fixpoint. Compilation of the full integration suite shows
no observable change.

## 10. Known Limitations

- Values that live only in SSA registers are still not traceable by the
  collector. The analysis preserves the existing guarantee: such a value is kept
  alive by the registered slot it was loaded from, which is why a loaded value's
  liveness keeps its *source slot* registered rather than tracking the value
  itself.
- Roots are registered for the whole activation once a slot needs to be visible,
  so a slot that is live across one collection point stays registered for the
  rest of the function even if it dies immediately afterwards. Shrinking the
  registered window needs an unregister point that dominates every later
  collection point, which the current placement does not attempt.
- Calls to unlisted runtime routines are treated as collection points. Adding a
  routine to the proven list is a correctness commitment and requires auditing
  the runtime for `maybe_collect_automatically` and `run_collection` calls.
- Stack arrays created through `tir.ArrayAlloca` are still not root-registered;
  this predates the task and is unchanged by it.

## 11. Remaining Optimization Opportunities

1. Shrink a root to the program region where it is actually needed, using the
   same liveness information plus a dominator check for the unregister point.
2. Derive precise stack maps (per-safepoint root sets) and pass them to the
   collector, which would remove the "registered for the whole activation"
   restriction entirely.
3. Feed the same liveness result into `analyze_stack_allocations` so managed
   aggregates with short live ranges can be scalarized instead of promoted.
4. Extend the collection-point summary to `nrq_gc_allocate_pinned` and any
   future managed allocation entry point so they participate automatically.
