# PO-24 — Compiler-Assisted Runtime Optimization

**Task:** PO 3.15 — Compiler-Assisted Runtime Optimization
**Date:** 2026-11-06
**Status:** Completed & Verified (unit and integration suites; runtime timing and
hardware-counter measurements deferred per the project policy that this task must
not be benchmarked)
**Compiler:** Nirqoba 0.162-beta (in development; 0.168-beta tree), LLVM/MLIR
23.1.4, MSVC x64 RelWithDebInfo

---

## 1. Problem Identified

The runtime side of the GC protocol had already been made cheap per call (PO-23).
What was left was *volume*: the lowering emitted the correct protocol for every
construct in isolation, without looking at what the surrounding code, or the
layout it addressed, already guaranteed. Nine classes of work were provably
redundant but still emitted:

1. **Nested mutator regions.** Every generated function that could touch a
   managed value opened its own `nrq_gc_mutator_enter` / `nrq_gc_mutator_leave`
   pair, even when its only callers already held a region. The enter/leave bumped
   and restored a nesting counter no collection could observe.
2. **One layout descriptor per call site.** Each root registration referenced a
   `alloca [N x i64]` filled with offsets and sizes immediately before the call,
   so a function with five managed locals materialised five arrays of identical
   constant content per activation.
3. **Root store after barrier store.** A reference stored through the write
   barrier was then reloaded from the root slot when it was needed again, instead
   of reusing the value the barrier had just accepted.
4. **Copy-based root publication.** Publishing a single managed reference into its
   root registered the slot, staged the pointer in a second slot, and called
   `nrq_gc_root_copy` to move 8 bytes into the root.
5. **Copy-based parameter roots.** A managed parameter kept live across a
   collection point paid the same staging slot plus copy in the prologue.
6. **Reload before unpin.** Releasing a managed handoff loaded the reference back
   out of the root it had been written into moments earlier.
7. **Add-and-mask safepoint throttle.** The back-edge poll advanced a counter,
   masked it with `kSafepointInterval - 1` and compared the mask with zero — an
   add plus a bitwise and per loop crossing.
8. **Root metadata duplication.** Distinct call sites addressing the same
   reference layout each carried their own private constant array.
9. **No structural test coverage.** Nothing asserted *which* protocol operations a
   program emits, so a change could quietly reintroduce a redundant call.

## 2. Root Cause

Lowering was purely local. `TirToMlir` walked one operation at a time and had no
representation of "the caller already established a region", "this slot was just
written with this value", or "this layout has been described before", and the pass
pipeline had no interprocedural step that could answer the region question. Each
runtime operation was therefore correct but paid unconditionally, and the cost was
per site rather than per program.

The fix is uniform: give the lowering the small amount of state it needs (a
pipeline pass for reachability, a per-module descriptor cache, a per-function
forwarded-value map) and delete the operations that the state proves unnecessary.
No runtime ABI changed, and every transformation is a strict removal of an
operation whose effect is already guaranteed by an operation that remains.

## 3. Files Modified

| File | Change |
| --- | --- |
| `compiler/lib/Lowering/PassPipeline.cpp` | New `ElideNestedMutatorRegionsPass` (pipeline step 6, after internalization; memory-effects inference renumbered to step 7). |
| `compiler/lib/Lowering/TirToMlir.cpp` | Per-module reference-layout descriptor cache; `forwarded_root_values_` map; direct pointer-root publication; parameter roots initialised through the barrier; unpin from the forwarded value; countdown safepoint throttle. |
| `tests/run_tests.py` | `expect_ir_contains`, `expect_ir_not_contains` and `expect_ir_counts` assertions, evaluated against a second `--emit=llvm-raw` build of the case. |
| `tests/integration/positive/gc_protocol_elision/` | New fixture: three-level call chain asserting one region pair and one layout descriptor. |
| `tests/integration/positive/gc_handoff_root_direct/` | New fixture: managed return and managed parameter asserting no `nrq_gc_root_copy` remains. |
| `tests/unit/mlir_pipeline_test.cpp` | `ThrottlesGcSafepointOnLoopBackEdges` now asserts the countdown and the absence of the wrap mask. |
| `CMakeLists.txt`, `README.md`, `tests/integration/positive/memory_advanced/expect.json`, `docs/VERSION_LOG.md` | Release bookkeeping for `0.158-beta`. |

No public API, runtime function, header, or ABI changed in this task.

## 4. Transformations Implemented

### 4.1 Nested mutator regions are elided (interprocedural)

`ElideNestedMutatorRegionsPass` treats a mutator region as a property of the call
graph. A function *establishes* a region when it can be entered from outside the
module's own calls:

* it is the entry point (`main`),
* it has public linkage, or
* its address is taken (the runtime or native code may invoke it at any time).

Every other function is reached only through direct calls, so it runs inside a
region once all of its callers do. The pass therefore computes a fixed point from
the establishing set and removes `enter`/`leave` from every function it proves is
always wrapped.

Two deliberate conservatisms: recursion never qualifies unless its cycle has an
outside entry point, and a function must itself contain a region
(`nrq_gc_mutator_enter`) to count as a provider of one — a function that only
calls helpers cannot cover them. Removal of a pair never drops the region to zero
depth, which is the only thing the nesting counter is observed for.

### 4.2 One read-only descriptor per reference layout

`reference_layout_descriptor()` interns the `[offsets…, sizes…]` array of a
reference layout in a per-module map keyed by the layout. The first use emits a
`private constant [N x i64]` global named `nrq_gc_layout_<o0>_<o1>…`; every later
use — in the same function or another one — references that global, and no
`alloca [1 x i64]` staging array is emitted at all for a one-reference layout.
The content is immutable and pointer-free, so sharing it changes no aliasing or
lifetime property; the array is `private` and only ever passed to
`nrq_gc_root_register_typed`.

### 4.3 Values stored through the barrier are reused

`forwarded_root_values_` records, for the slot that a barrier store just wrote,
the reference that was stored. A subsequent read of that slot reuses the value
instead of reloading it. The map is cleared on any `Store` (the slot may have been
written directly), and on `Call`, `Label`, `Branch` and `BranchCond` — after a
call the slot can have been rewritten by the callee, and at a control-flow join the
recorded value is no longer known to dominate.

### 4.4 Single references are published straight into their root

`root_managed_references_and_release_handoffs` distinguishes a layout that is
exactly one pointer at offset 0 from a general aggregate. For the single-reference
case the value is registered and then stored into the root with
`nrq_gc_root_store(root.slot, value)`; the intermediate slot, its store, and the
`nrq_gc_root_copy` call disappear. Aggregates keep the copy path, which is what
their by-value `{…}` ABI requires.

### 4.5 Parameter roots are initialised by the barrier

The same distinction applies to the prologue of a function whose managed parameter
stays live across a collection point: the registration is followed by
`nrq_gc_root_store`, not by a staging store plus `nrq_gc_root_copy`. A parameter
root therefore costs one registration and one barrier store instead of a slot, a
store and a copy.

### 4.6 Handoff release unpins the published value

The release path for a direct root had been loading the reference back out of the
slot. It now unpins the value that was published into it, removing the reload from
every handoff release.

### 4.7 Safepoint throttle counts down

The poll counter became a countdown. A fresh activation initialises it to
`kSafepointInterval`; each back edge subtracts one and compares with zero, and the
polled block reloads the interval. That replaces `add` + `and` + `icmp` with
`sub` + `icmp` on the hot path and keeps the observation bound at
`kSafepointInterval` crossings. `ThrottlesGcSafepointOnLoopBackEdges` now asserts
exactly one countdown subtract, zero `and 63` masks, one poll call, and that the
poll still sits in a block whose only predecessor is the conditional.

### 4.8 The test runner can assert emitted protocol operations

`run_tests.py` builds the case a second time with `--emit=llvm-raw` (pre-LLVM, so
internal linkage and un-inlined calls are visible) and applies three new
expectation keys: `expect_ir_contains`, `expect_ir_not_contains` and
`expect_ir_counts` (exact substring multiplicity). The assertions are available in
`build`, `build_and_run`, `assembly_check` and `target_assembly` modes and are
skipped only when the case asks for no IR build.

## 5. Correctness Argument

**Region elision.** The removed pair only ever moved the nesting depth from *n* to
*n+1* and back. A collection observes the depth at a safepoint, and a safepoint
inside the callee is reached with depth *n ≥ 1* whether or not the callee opened its
own region, because the caller's region is still open — `main` leaves through the
same call chain it entered by. Functions that can be entered without a region keep
their pair, which is why the establishing set includes `main`, public linkage and
address-taken functions; recursion is excluded until its cycle has such an entry.

**Layout descriptors.** The global is an immutable constant with the same bytes,
offset order and `object_size` count that the stack array held, and it is only read
by the registration call. Sharing is therefore observationally identical; the
descriptor is `private`, so it does not leak into the link unit's symbol set.

**Forwarded values.** The map is only consulted for a slot the lowering itself just
stored to, and is invalidated at every point where another write could intervene
(any store, any call) and at every control-flow boundary where dominance is lost
(labels, branches). Reusing the stored value is therefore equivalent to reloading
it, because no instruction between the store and the use can have changed the slot.

**Direct root publication.** `nrq_gc_root_register_typed` records the slot's
`object_size`, layout and pointer-root flag; `nrq_gc_root_store` writes the pointer
with the barrier and remembered-set handling. For a one-pointer layout the copied
bytes and the barrier store produce the same slot contents and the same barrier
effect, so a collection scanning the root sees the same referent. Aggregates keep
`root_copy`, because a multi-word layout is not a single reference and the barrier
semantics of a partial copy differ.

**Parameter roots.** Identical argument, applied to the prologue: the root is
registered with the same descriptor and then written with the barrier instead of a
byte copy. The parameter's value is loaded from the argument, and the store order
registration → store is unchanged.

**Unpin from the value.** The unpin target is the pointer value the same path
published, so the pin table's key is the same pointer a reload would have produced.

**Countdown throttle.** The countdown visits zero exactly once per
`kSafepointInterval` decrements, from any starting value in `[1, kSafepointInterval]`,
and is reloaded immediately on the polled path, so the polling cadence is identical
to the mask scheme. The slot is entry-block state, so LLVM promotes it to a
register and the loop keeps its shape; a collection request raised by another
thread is still observed within one interval.

## 6. Tests Added or Modified

| Test | What it locks down | Result |
| --- | --- | --- |
| `positive/gc_protocol_elision` (integration, new) | A three-level chain (`main → middle → inner`) emits exactly one `nrq_gc_mutator_enter`/`leave` pair, exactly one layout descriptor global, no `alloca [1 x i64]`, and prints `6` | pass |
| `positive/gc_handoff_root_direct` (integration, new) | A managed return and a managed parameter across `gc.collect()` emit zero `nrq_gc_root_copy` calls and two barrier root stores, and print `13` | pass |
| `MlirPipelineTest.ThrottlesGcSafepointOnLoopBackEdges` (updated) | One poll call in a conditionally guarded block, one countdown subtract, no wrap mask | pass |

## 7. Verification

- `./build/msvc-dev/tests/nirqoba_unit_tests.exe` — **154/154**.
- `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe` —
  **243/243**.
- Structural evidence only: operation counts in the emitted IR and in the
  integration assertions, not wall-clock timings or hardware counters, per the
  project policy for this task.

## 8. Structural Evidence (before → after, per site)

| Site | Before | After |
| --- | --- | --- |
| Function reached only from a region-holding caller | `nrq_gc_mutator_enter` + body + `nrq_gc_mutator_leave` | body only (pair removed) |
| Three-level chain, regions in IR | 3 enter / 3 leave | 1 enter / 1 leave |
| Root registration for one layout | `alloca [N x i64]` + N stores + registration | `private constant [N x i64]` global + registration |
| Same layout at *k* call sites | *k* private arrays, *k* × N stores per activation | 1 private global, 0 stores |
| Single-reference root publication | register + staging `alloca` + store + `nrq_gc_root_copy` | register + `nrq_gc_root_store` |
| Managed parameter live across a collection | register + staging `alloca` + store + `nrq_gc_root_copy` | register + `nrq_gc_root_store` |
| Re-read of a slot just stored through the barrier | `load ptr` from the slot | reuse of the stored SSA value |
| Handoff release | `load ptr` from the root + `nrq_gc_unpin` | `nrq_gc_unpin` of the published value |
| Back-edge throttle | `add`, `and 63`, `icmp eq 0`, `store` | `sub 1`, `icmp eq 0`, `store` (+ interval reload on the polled path only) |

Observed in `--emit=llvm-raw` for the `gc_protocol_elision` fixture: one
`@nrq_gc_layout_0` constant, one `nrq_gc_mutator_enter`/`leave` pair, and no
`alloca [1 x i64]`. For `gc_handoff_root_direct`: `take(ptr %0)` registers `%2`
and calls `nrq_gc_root_store(%2, %0)` before `nrq_gc_collect()`, with zero
`nrq_gc_root_copy` in the module.

Compilation time: the elision pass is one fixed-point walk over the call graph and
the descriptor cache is a hash lookup per registration; no measurable compilation
cost was introduced, and none is claimed without measurement.

## 9. Known Limitations

1. **Region elision is intra-module and conservative.** It considers direct calls
   only, treats address-taken functions as entries, and never elides a recursive
   cycle without an outside entry point, so a program that passes managed helpers
   as callbacks keeps their pairs. Devirtualising such calls is out of scope here.
2. **Aggregate roots still copy.** A layout with more than one reference, or one
   reference at a non-zero offset, keeps the staging slot and `nrq_gc_root_copy`,
   because a copy is not a single barrier store.
3. **Forwarded values do not survive calls or joins.** A callee that could rewrite
   the slot, and any control-flow boundary, discard the record, so the reload only
   disappears within a straight-line block.
4. **Safepoint polling is still per back edge.** The countdown removed the mask,
   not the poll site; thinning poll sites by loop liveness is a different analysis.
5. **The IR assertions are substring counts.** They are precise for the fixtures
   they name but tied to emitted spelling; a renamed runtime function silently
   stops matching, which shows up as a failed count rather than a false pass.

## 10. Remaining Opportunities

1. **Safepoint thinning from root liveness.** A loop that cannot reach a
   collection point after its last allocation could skip polling entirely; the
   countdown makes the remaining polls cheaper but not rarer.
2. **Devirtualised region elision.** Recording address-taken functions that are
   only ever passed as known runtime callbacks would let their pair go too.
3. **Aggregate root stores through the barrier per word**, replacing
   `nrq_gc_root_copy` for multi-reference layouts while preserving the barrier's
   per-word semantics.
4. **Stack maps instead of per-frame registration** — the compiler already knows
   the layout and liveness of every root slot it registers, so a compact map plus
   one frame descriptor would replace the per-slot registration sequence.
5. **Publish the elision statistics** (regions removed, descriptors shared, copies
   avoided) through a compiler diagnostics channel so the effect is observable
   without reading IR.
