# PO-13 — SSA Promotion, Scalar Replacement and Aggregate Optimization

**Task:** P0 3.5 — SSA Promotion, Scalar Replacement and Aggregate Optimization
**Date:** 2026-11-04
**Status:** Completed & Verified (correctness and execution tests passed; benchmarks deferred per policy)
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, MSVC x64 RelWithDebInfo

---

## 1. Problem Identified

The task asks whether the generated IR *allows* LLVM to perform mem2reg, SROA,
redundant-load elimination, store-to-load forwarding, dead-store elimination,
aggregate copy elimination, lifetime-based slot reuse and removal of unnecessary
temporary materialisation — and to fix the IR quality where it does not.

Two of those questions had a definite answer.

1. **Temporary stack slots were materialised at their use site, not in the entry
   block.** Four lowering paths created an `llvm.alloca` with the builder's
   current insertion point: the aggregate-store temporary that feeds
   `nrq_gc_root_copy`, the reference-offset array of `array.allocate`, the
   element temporary of `memory.mapped_read`/`memory.mapped_write`, and the
   record slot of `memory.profiler.capture`. A use site inside a loop body means
   the slot is allocated — dynamically, since LLVM only folds an alloca in the
   entry block into the fixed frame — on every iteration. A program that
   allocates and profiles in a loop therefore grows its stack without bound, and
   `alloca_at_entry` already documented exactly this hazard for the ordinary
   locals while these four paths bypassed it.
2. **Those same slots were invisible to mem2reg and SROA.** Both require the
   alloca to be in the entry block, so a temporary created inside a loop kept
   going through memory at every optimization level, and the value it carried
   could not be forwarded.

## 2. Root Cause

`TirToMlir.cpp` has two ways to obtain a stack slot: the `alloca_at_entry`
helper, which every `tir.alloca` and every root slot uses, and a direct
`builder_.create<mlir::LLVM::AllocaOp>` call at the current insertion point.
The direct calls were introduced for the "materialise one value for a runtime
call" pattern, where the alloca reads naturally as part of the call sequence.
Nothing in the code required the slot to be per-iteration — the surrounding TIR
operation is emitted exactly once regardless of how many times the generated
code runs it — so the placement was accidental, not semantic.

## 3. Files Modified

- `compiler/lib/Lowering/TirToMlir.cpp`
  - `Store` lowering: the aggregate temporary that feeds `nrq_gc_root_copy` is
    now an entry-block slot.
  - `array.allocate` lowering: the reference-offset array is now an entry-block
    slot (the offsets describe the element type, so one slot per allocation site
    is enough).
  - `memory.mapped_read` / `memory.mapped_write`: the element temporary is now an
    entry-block slot.
  - `memory.profiler.capture`: the record slot is now an entry-block slot.
  - Each of the four paths reports a new internal diagnostic `NRQ4006` if the
    translator has no entry block (an invariant the constructor already
    guarantees), matching the existing `NRQ4005` handling for root slots.
  - There is now exactly one `mlir::LLVM::AllocaOp` creation in the lowering, and
    it lives inside `alloca_at_entry`.
- `tests/unit/mlir_pipeline_test.cpp`: `kStackSlotSource`,
  `lower_source_to_mlir` and the `AllocatesEveryStackSlotInTheEntryBlock` test
  (front end → TIR → MLIR, then a block-placement assertion on every alloca).
- `tests/integration/mlir/ssa_promotion_aggregates/` (new) and
  `tests/integration/positive/loop_stack_slot_reuse/` (new).
- This report.

## 4. Algorithm or Transformation Implemented

### 4.1 Slot placement

Every stack slot the lowering creates now comes from `alloca_at_entry`, which
sets the insertion point to the start of the function's entry block for the
duration of the allocation:

- an alloca in the entry block with a constant count is folded into the fixed
  frame, so the frame is adjusted once per activation instead of once per
  iteration;
- mem2reg promotes it (it needs a store in the entry block and every load/store
  dominated by it), and SROA can split it when the accesses are statically
  indexable;
- because the TIR operation is translated once, the number of allocas is
  unchanged — only their placement moves, so frame size does not grow.

### 4.2 What was deliberately *not* added

The task excludes duplicating existing LLVM passes, so no TIR-level SSA
construction, no custom SROA and no custom DSE pass was written. The lowering
keeps materialising locals in memory: `mem2reg` and `SROA` are what turn them
into SSA values, and the investigation below establishes that they now do.

### 4.3 Investigation results

Each item of the required list was checked by inspecting `--emit=llvm` output at
`-O0` and `-O3` for the representative programs:

| # | Mechanism | Finding |
|---|---|---|
| 1 | Mem2Reg | Works once the slot is in the entry block: scalar locals disappear entirely at `-O3` (see §7). Previously blocked for any local whose *temporary* was created in a loop. |
| 2 | SROA | Works: `struct_locals` and `ssa_promotion_aggregates` (struct, tuple, fixed-size array) are fully scalarized — 16 and 11 allocas become 0. |
| 3 | Redundant load elimination | Works: load counts drop to 0 on the same programs, so repeated field/index reads are not re-issued. |
| 4 | Store-to-load forwarding | Works together with mem2reg; no load of a promoted slot survives at `-O3`. |
| 5 | Dead store elimination | Already exercised by `tests/integration/mlir/dead_store_elimination`; unchanged by this task. |
| 6 | Aggregate copy elimination | Improved by PO-11 (a store into a slot the collector cannot observe is a plain `llvm.store`, not a temporary plus `nrq_gc_root_copy`) plus entry-block placement (the remaining copy temporaries are promotable). |
| 7 | Lifetime-based slot reuse | Achieved through entry-block placement: all slots share one fixed frame, and LLVM's stack colouring can overlap slots whose live ranges do not intersect. The lowering emits no `llvm.lifetime.start/end`; they are not required for reuse, but they would let LLVM reuse a frame region across disjoint live ranges more aggressively (see §11). |
| 8 | Unnecessary temporary materialisation | Fixed for the four paths above. No other path creates a slot that a value could avoid. |

Deliberately retained memory traffic:

- a GC root slot must stay in memory, because the collector traces the slot
  address (`nrq_gc_root_register_typed`); promoting it to SSA would hide the
  reference from marking. `gc_managed_graph` shows this honestly: 16 allocas at
  `-O0`, 13 at `-O3`.
- a local array indexed by a runtime value cannot be scalarized (correct
  behaviour, not a deficiency).

## 5. Why the Approach is Correct

- **Placement is not an observable property.** Moving an alloca earlier only
  changes when the frame space is reserved; the slot is still written before it
  is read on every path, because the surrounding lowering stores the value
  immediately after allocating for the copy/mapped/profiler paths, and the
  offsets array is filled before the call that consumes it.
- **No aliasing change.** The slots are distinct SSA values; mem2reg and SROA do
  their own alias and escape checks, so nothing is promoted that is observed
  through another pointer. In particular the GC root slots remain in memory
  because their address is passed to a capturing runtime call.
- **No new allocation count.** Each TIR operation is translated once, so the
  number of allocas per function is unchanged; the fix cannot increase frame
  size, and it removes the per-iteration dynamic adjustment that previously grew
  the stack.
- **The regression is proven, not assumed.** With the previous lowering the same
  test program exits non-zero (the generated executable aborts from stack
  exhaustion), and the block-placement unit test reports two allocas outside the
  entry block. With the change the program prints `200000` and the test passes.

## 6. Tests Added or Modified

- `tests/unit/mlir_pipeline_test.cpp` — `AllocatesEveryStackSlotInTheEntryBlock`
  compiles a program containing scalar and aggregate locals, a managed
  `array.allocate` inside a loop and a loop-local `memory.profiler.capture()`,
  then asserts that at least one `llvm.alloca` exists and that **none** of them
  is outside its function's entry block. With the previous lowering the test
  fails with `outside_entry == 2`.
- `tests/integration/positive/loop_stack_slot_reuse/` (new): 200000 iterations
  that allocate a managed array and capture a profile each time; expects
  `200000` and exit 0. Before the fix the same program exhausted its stack and
  aborted (exit 127 on this platform).
- `tests/integration/mlir/ssa_promotion_aggregates/` (new): a program with a
  struct, a tuple and a fixed-size array local, built at `-O3`; the emitted
  module must contain no `alloca` at all, which is only possible if mem2reg and
  SROA promoted every local.
- **Suite results:** 118/118 unit tests and 189/190 integration tests pass. The
  single failure (`positive/memory_advanced`) is the pre-existing version-string
  drift (`0.105-beta-phase6` expected, `0.111-beta-phase6` produced by the
  runtime), unchanged by this task.

## 7. LLVM/MLIR Evidence

Alloca, load and store counts per module (`--emit=llvm`, before/after
optimization):

| Program | `-O0` alloca / load / store | `-O3` alloca / load / store |
|---|---|---|
| `locals_in_loops` | 5 / 11 / 9 | 0 / 0 / 0 |
| `struct_locals` | 16 / 27 / 27 | 0 / 0 / 0 |
| `ssa_promotion_aggregates` (new) | 11 / 18 / 13 | 0 / 0 / 0 |
| `gc_managed_graph` | 16 / 13 / 18 | 13 / 7 / 14 |

The first three show complete scalarization of scalars, structs, tuples and
fixed-size arrays. `gc_managed_graph` keeps the slots that must stay in memory:
those are the registered GC roots, whose address the collector traces, plus the
reference-offset arrays passed to `nrq_gc_allocate_array_typed_rooted`.

Slot placement for the loop program that allocates and profiles in the same
iteration (`--emit=llvm -O0`, `main`), before:

```llvm
define i64 @main() #3 {
  %1 = alloca [1 x i64], i64 1, align 8          ; entry block
  %2 = alloca { i64, i64, ... }, i64 1, align 8  ; entry block (profiler record, 28 words)
  %3 = alloca ptr, i64 1, align 8                ; entry block
  %4 = alloca ptr, i64 1, align 8                ; entry block
  %5 = alloca i64, i64 1, align 8                ; entry block
  %6 = alloca i64, i64 1, align 8                ; entry block
  ...
12:                                              ; loop body
  %13 = alloca [1 x i64], i64 1, align 8         ; reference offsets, per iteration
  %15 = call ptr @nrq_gc_allocate_array_typed_rooted(ptr %4, i64 1, i64 16, ptr %13, i64 1)
  ...
37:                                              ; loop body
  %24 = alloca { i64, i64, ... }, i64 1, align 8 ; profiler record, per iteration
```

and after:

```llvm
define i64 @main() #3 {
  %1 = alloca [1 x i64], i64 1, align 8              ; reference offsets
  %2 = alloca { i64, i64, ... }, i64 1, align 8      ; profiler record (28 words)
  %3 = alloca { i64, i64, ... }, i64 1, align 8      ; profiling region copy
  %4 = alloca ptr, i64 1, align 8                    ; root slot
  %5 = alloca ptr, i64 1, align 8                    ; node
  %6 = alloca [1 x i64], i64 1, align 8              ; array count
  %7 = alloca i64, i64 1, align 8                    ; i
  %8 = alloca i64, i64 1, align 8                    ; total
  ...                                                ; no slot anywhere else
```

(The slot-to-name mapping in the comments is indicative; the property under test
is that all of them precede the first branch, which is what
`AllocatesEveryStackSlotInTheEntryBlock` asserts.)

The same program at runtime, before → after:

```
before: exit 127 (generated executable aborts; the frame grows per iteration)
after:  prints 200000, exit 0
```

## 8. Before-and-After Benchmark Results

Section 5 of the roadmap is explicitly gated on user approval, so no runtime
benchmark was executed. The acceptance criterion asking for performance
comparisons is therefore answered with static IR counts (§7) plus a behavioural
result (crash vs. completion), not with wall-clock numbers.

## 9. Compilation-Time and Memory Impact

The change replaces one builder call with another; it adds no analysis and no
pass, so compile time is unchanged. Frame size is unchanged: the same number of
slots is allocated, in a different block. The generated programs can now be
smaller in practice because frame adjustment no longer happens per iteration,
and because the slots became promotable (see the `-O3` counts above).

## 10. Known Limitations

- The lowering emits no `llvm.lifetime.start`/`llvm.lifetime.end`. Stack slot
  reuse therefore relies on LLVM's stack colouring seeing non-overlapping live
  ranges, not on explicit lifetime regions.
- GC root slots and reference-offset arrays must stay in memory, so a function
  that manipulates managed values keeps a real frame. This is a requirement of
  the collector, not an IR-quality deficiency.
- Locals whose address is taken through a dynamic index (array locals read with a
  runtime subscript) cannot be scalarized; the slot stays.
- The aggregate-store path still materialises the value in memory to hand it to
  `nrq_gc_root_copy`. The copy is only emitted for destinations whose provenance
  the collector can observe; removing it entirely would require a runtime
  barrier ABI that takes the value in registers.
- The investigation covered mem2reg, SROA, RLE, forwarding, DSE, copy
  elimination, slot reuse and temporary materialisation. Loop-structure quality
  (2 blocks per bounds check, vectorization hints) is the subject of 3.6 and was
  not addressed here.

## 11. Remaining Optimization Opportunities

1. Emit `llvm.lifetime.start/end` around stack slots so LLVM can overlap frame
   regions across disjoint live ranges and prove a slot dead before a call.
2. Let the GC root plan (PO-11) also decide *when* a root slot starts and stops
   being a root, which would let the slot itself be promoted to SSA outside that
   window.
3. Teach the copy barrier to accept a by-value aggregate, removing the temporary
   from the aggregate-store path.
4. Fold the parameter slot away at the TIR level: a parameter that is never
   assigned does not need an `alloca` + store + load, even though mem2reg removes
   them later.
