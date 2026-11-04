# PO-12 — Runtime Function Attributes and Inferred Function Memory Effects

**Task:** P0 3.4 — Runtime Function Attributes (GC family) + Function Memory Effects
**Date:** 2026-11-04
**Status:** Completed & Verified (correctness and execution tests passed; benchmarks deferred per policy)
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, MSVC x64 RelWithDebInfo
**Predecessor:** `reports/performance/po-08-runtime-attributes.md` (the allocator,
resource, math and bounds families). That report closed with the GC root
functions "addressed in subsequent roadmap tasks"; this report is that follow-up.

---

## 1. Problem Identified

Two separate gaps left the optimizer blind at every call boundary in generated
code.

1. **The GC protocol had no attributes at all.** `RuntimeAttributes.h` described
   the math, allocator, resource, error and bounds families, but not a single
   `nrq_gc_*` entry point. `nrq_gc_mutator_enter`, `nrq_gc_safepoint`,
   `nrq_gc_root_register_typed`, `nrq_gc_root_store`, `nrq_gc_root_copy`,
   `nrq_gc_collect` and friends were emitted as bare declarations with no
   `nounwind`, no `willreturn` and no `nocapture`. Every one of them therefore
   looked like a call that could throw a C++ exception across the generated
   frame, and their pointer arguments looked like they could be captured
   anywhere.
2. **Generated functions described none of their own memory access.**
   `llvm.func` bodies were exported without any `memory(...)` attribute, so from
   the optimizer's point of view every call to a generated helper — including a
   helper that only multiplies two integers — read and wrote all of memory.
   Loads could not be hoisted, merged or sunk across such a call, calls could not
   be reordered with stores, and a call whose result was unused could not be
   deleted.

The cost is structural rather than incidental. The Nirqoba ownership model makes
small, single-purpose generated functions the normal shape of the program
(`owned`/value paths, borrowed accessors, view helpers), so these call boundaries
are everywhere in lowered code.

## 2. Root Cause

- `get_runtime_function_policy` is a symbol-name table. Any symbol absent from
  the table silently receives no attributes (`apply_runtime_function_attributes`
  returns early on `std::nullopt`), and the GC family had simply never been
  added after the root machinery was introduced.
- The ownership and escape analyses (`po-09`, `po-10`) annotate *parameters*
  (`nirqoba.ownership`, `llvm.noalias`, `llvm.nocapture`, `llvm.readonly`) but
  nothing ever summarised what a function *body* does. MLIR's own
  `memory_effects` attribute exists only on the LLVM dialect, so it has to be
  derived after `convert-func-to-llvm`, not attached in the nrq/TIR stages — and
  no pass did that derivation.

## 3. Files Modified

- `compiler/include/nirqoba/Lowering/RuntimeAttributes.h`
  - New `MemoryEffectKind::OtherReadWrite` and
    `MemoryEffectKind::OtherReadWriteArgRead` values plus their lowering in
    `apply_runtime_function_attributes` (`other: readwrite`, no inaccessible or
    argument memory; the second form also reads argument memory).
  - New policy section **7. GC mutator protocol and root management**: `nounwind`
    for every emitted `nrq_gc_*` symbol, `willreturn` for the ones that cannot
    wait on another thread, `nocapture` for the reference-offset arrays and slot
    addresses that are only compared, `noalias` for
    `nrq_gc_allocate_array_typed_rooted`.
  - New policy section **8. Synchronization primitives**: `nounwind` for
    `nrq_mutex_create/lock/unlock/destroy`, `willreturn` for all but `lock`.
  - The diagnostic and print family (`nrq_memory_array_bounds_fail`,
    `nrq_fatal_error`, `nrq_runtime_fatal`, `nrq_system_exit`, `nrq_print_*`)
    now states that it reads/writes global state and not the caller's stack.
    Without this, a function containing a single array access would have fallen
    back to "touches everything" and could never be summarised.
- `compiler/lib/Lowering/PassPipeline.cpp`
  - `AddressOrigin`, `MemoryEffectSummary`, `charge_access`,
    `charge_unknown_effects`, `llvm_op_accesses_no_memory`,
    `effects_from_attribute`, `scan_body`, `analyze_body`, `mod_ref_info`,
    `attach_memory_effects` and the new `InferFunctionMemoryEffectsPass`.
  - `build_lowering_pipeline` gains **step 4**: the pass runs last, at every
    optimization level, after `convert-func-to-llvm` and the reconcile pass.
- `tests/unit/mlir_pipeline_test.cpp`: `kMemoryEffectProbe` and the
  `InfersFunctionMemoryEffectsFromProvenance` test.
- `tests/integration/mlir/function_memory_effects/` (new test).
- This report.

## 4. Algorithm or Transformation Implemented

### 4.1 Runtime attribute policy additions

Every `nrq_gc_*` symbol the lowering emits was enumerated first
(`grep -o '"nrq_gc_[a-z_]*"' compiler/lib/Lowering/TirToMlir.cpp`) so the table
covers the actual ABI and nothing invented:

| Symbol family | `nounwind` | `willreturn` | Memory / capture |
|---|---|---|---|
| `nrq_gc_mutator_enter/leave`, `nrq_gc_safepoint` | yes | no | (bookkeeping) |
| `nrq_gc_root_frame_begin/end` | yes | yes | (bookkeeping) |
| `nrq_gc_root_register_typed` | yes | no | `nocapture` on the offsets array |
| `nrq_gc_root_register_array`, `nrq_gc_root_clear_array_references` | yes | no | `nocapture` on the offsets array |
| `nrq_gc_root_store`, `nrq_gc_root_copy` | yes | no | (write barrier) |
| `nrq_gc_root_unregister`, `nrq_gc_unpin` | yes | yes | `nocapture` on the slot address |
| `nrq_gc_pin`, `nrq_gc_pin_transfer` | yes | no | — |
| `nrq_gc_allocate_array_typed_rooted` | yes | no | `noalias` result |
| `nrq_gc_collect` | yes | no (parks other mutators) | — |
| `nrq_gc_init`, `nrq_gc_shutdown` | yes | no | — |
| `nrq_gc_enable/disable/set_threshold/is_enabled`, `nrq_gc_get_*`, `nrq_gc_pin_count`, `nrq_gc_pin_violations`, `nrq_gc_ref_count_*` | yes | yes | — |

`willreturn` is withheld wherever the routine can block: a collection waits for
every other mutator to park, `nrq_mutex_lock` waits for the lock, and the write
barriers enter the collector handshake. `nounwind` is safe for all of them
because the runtime reports failures through `nrq_runtime_fatal` /
`nrq_memory_array_bounds_fail`, both of which terminate the process, and because
the platform layers already swallow their own exceptions
(`runtime/src/sync_windows.cpp`, `file_windows.cpp`, `socket_windows.cpp` wrap
the OS calls in `try`/`catch`).

### 4.2 Inferred function memory effects

`InferFunctionMemoryEffectsPass` is a `ModuleOp` pass that derives
`llvm.memory_effects` from each generated body.

**Address provenance.** Each pointer value is classified while walking the body:

- a result of `llvm.alloca`, or of a `getelementptr`/cast chain rooted at one, is
  `Local` — *inaccessible* memory, the caller's frame;
- a parameter, or a pointer extracted or cast from one, is `Argument` — argument
  memory, including memory reachable through pointers loaded out of it;
- everything else (a pointer returned by a call, a global's address, a pointer
  loaded from an unknown location) is `Unknown` and is charged to **both** the
  `other` and the `argmem` locations, because it may be reachable from either.

**Stack slots carry provenance.** The toolchain gives every local — including
every parameter — an `llvm.alloca`. A reader of a borrowed view therefore loads
the view's data pointer out of its own stack slot, and a naive rule ("a pointer
loaded from stack memory could address anything") would mark the function as
touching all of memory. The pass therefore runs each body twice: the first scan
records, per `llvm.alloca`, the joined origin of every pointer value stored into
it (through the slot itself or any GEP of it); the second scan charges accesses
using those origins. Joining two different origins yields `Unknown`, so mixing
an argument pointer and a stack pointer in one slot stays conservative.

**Calls.** A call to a module function inherits that function's summary. Module
functions are analysed to a **greatest fixpoint** starting from "no effects", so
recursion is handled: a property can only be added as evidence accumulates, and
a self-recursive arithmetic function (the `fib` shape) keeps `memory(none)`.
A call to a declaration uses the effects the runtime policy already attached;
a declaration with no attribute, an unknown callee (indirect call), or one that
touches `errnoMem`/`targetMem` makes the call conservative, because the caller's
three-location attribute cannot describe those.

**Unmodelled operations.** Anything not on the explicit memory-free list
(`llvm.add`, `llvm.icmp`, `llvm.br`, `llvm.lifetime.*`, …) charges all three
locations. An operation the pass does not understand can therefore never produce
a false promise.

**Attachment.** An attribute is only written when it narrows the default: a
summary that already touches everything produces no attribute at all, which is
exactly the previous behaviour.

The pass does not depend on the optimization level. It runs at `-O0` too, since
the attribute describes the program rather than the result of an optimization.

## 5. Why the Approach is Correct

- **The added `nounwind` flags cannot change error behaviour.** Every GC and
  synchronization entry point reports failure by terminating the process
  (`nrq_runtime_fatal` is `noreturn`) rather than by throwing, and the platform
  layer contains its own `try`/`catch`. Marking a call `nounwind` therefore only
  removes the possibility of an unwind edge that the callee never had.
- **`willreturn` is only used where the call cannot wait for another thread.**
  Collection and lock acquisition are excluded, so the flag cannot be used to
  justify removing a blocking call that another thread depends on.
- **`nocapture` is only attached to pointers the runtime does not retain.**
  `nrq_gc_root_register_typed` copies the offsets array into root metadata and
  keeps only the slot address, which is why the slot address itself is *not*
  marked `nocapture`; `nrq_gc_root_unregister`/`nrq_gc_unpin` only compare the
  slot address, so it is.
- **Memory effects are an over-approximation by construction.** Every location a
  construct could touch is charged; the only path that ever *narrows* a summary
  is the "known callee" path, and that path exists only for module functions
  (whose bodies are analysed the same way) and declarations whose own attribute
  the runtime policy set. Unknown callees, unmodelled ops and `errno`-touching
  declarations widen to "everything".
- **Cheating the location classification is impossible in the unsound
  direction.** Claiming less than emitted memory would require a pointer of
  argument or local origin that the analysis mis-tracks. Provenance is only ever
  propagated structurally (alloca → GEP → load → cast), and every step that
  loses the structure falls back to `Unknown`, which charges both `other` and
  `argmem`.
- **No transformation is performed.** The pass adds an attribute; it never
  rewrites, removes or reorders an operation.

## 6. Tests Added or Modified

- `tests/unit/mlir_pipeline_test.cpp` — `InfersFunctionMemoryEffectsFromProvenance`
  runs the real pipeline on a probe module and asserts, per function:
  - `pure` (arithmetic only) — `other`, `argMem` and `inaccessibleMem` all
    `NoModRef`;
  - `reader` (loads through its pointer argument) — `argMem: Ref`,
    `other: NoModRef`;
  - `spilled` (spills the argument into a stack slot, reads it back and loads
    through the reloaded pointer) — `argMem: Ref`, `other: NoModRef`,
    `inaccessibleMem: ModRef`, i.e. parameter provenance survives the spill;
  - `opaque_caller` (calls a declaration with no known effects) — **no**
    attribute, i.e. the conservative fallback survives.
- `tests/integration/mlir/function_memory_effects/` (new) builds a real program
  at `-O2` and requires the emitted module to contain
  `other = none, argMem = none, inaccessibleMem = readwrite` (the arithmetic
  helper), `argMem = read` (the borrowed-view reader) and
  `other = readwrite, argMem = none, inaccessibleMem = none` (the diagnostic and
  print declarations), and exits 0 so the program still runs.
- **Suite results:** 117/117 unit tests and 187/188 integration tests pass. The
  single failure (`positive/memory_advanced`) is the pre-existing version-string
  drift (`0.105-beta-phase6` expected, `0.111-beta-phase6` produced by the
  runtime), unchanged from before this task.

## 7. LLVM/MLIR Evidence

Declaration attributes for a GC-using program
(`tests/integration/positive/gc_managed_graph`, `--emit=llvm -O2`), all of which
previously carried no attribute group:

```llvm
declare void @nrq_gc_root_register_typed(ptr, i64, ptr captures(none), i64)  #2
declare void @nrq_gc_root_store(ptr, ptr)                                    #2
declare void @nrq_gc_mutator_enter()                                         #2
declare void @nrq_gc_collect()                                               #2
declare noalias ptr @nrq_gc_allocate_array_typed_rooted(ptr, i64, i64, ptr, i64) #2
declare i64 @nrq_gc_root_frame_begin()                                       #1
declare void @nrq_gc_root_frame_end(i64)                                     #1
declare void @nrq_gc_unpin(ptr captures(none))                               #1

attributes #1 = { mustprogress nounwind willreturn }
attributes #2 = { nounwind }
```

Inferred function effects (`tests/integration/mlir/function_memory_effects`,
`--emit=mlir -O2`):

| Function | Inferred effects |
|---|---|
| `pure_helper` (arithmetic only) | `other = none, argMem = none, inaccessibleMem = readwrite` |
| `sum_borrowed` (reads a borrowed view, spilled to a stack slot) | `other = readwrite, argMem = read, inaccessibleMem = readwrite` |
| `main` (allocates, prints) | *no attribute* — conservative default |

The same program after LLVM optimization (`--emit=llvm -O2`):

```llvm
define i64 @pure_helper(i64 %0, i64 %1) local_unnamed_addr #3
define i64 @sum_borrowed({ ptr, i64 } %0) local_unnamed_addr #4
; attributes #3 = { mustprogress nofree norecurse nosync nounwind willreturn memory(none) }
; attributes #4 = { nofree norecurse nosync nounwind memory(read, inaccessiblemem: none, ...) }
```

Two observations belong in the record. First, `nrq_print_int` and
`nrq_memory_array_bounds_fail` now describe their global-state access instead of
leaving the caller's summary empty-handed
(`memory(readwrite, argmem: none, inaccessiblemem: none, ...)`). Second, LLVM's
own function-attribute inference re-derives the attribute later in its pipeline
and, for `sum_borrowed`, generalises our `argmem: read` to `other: read`
afterwards — it cannot see through `extractvalue` of a by-value aggregate to the
argument. The compiler's promise is what the LLVM optimizer consumes during the
function pipelines (LICM, GVN, DSE, inlining); LLVM's later re-derivation is not
a regression, but it does mean the surviving printed attribute can be weaker than
the one the compiler inferred.

## 8. Before-and-After Benchmark Results

Section 5 of the roadmap is explicitly gated on user approval, so no runtime
benchmark was executed. The evidence above is static attribute and IR inspection
and is not a claim about wall-clock performance.

## 9. Compilation-Time and Memory Impact

Per module the pass performs one walk over the declarations, then a fixpoint over
the defined functions. Each round is `O(operations)` with a handful of hash
lookups per operation, and the loop terminates when no summary changes; in
practice it converges in two to three rounds (the call graph of generated code is
shallow and recursion is rare). Each function body is additionally walked twice
(slot provenance, then charging), and both slot maps are `DenseMap`s over the
handful of allocas a body contains. Attribute attachment happens once per
function. The whole integration suite compiles in the same wall-clock time as
before to the resolution the build measures.

## 10. Known Limitations

- A pointer loaded from a stack slot that two different kinds of value flow into
  joins to `Unknown`, which gives up the argument-memory precision for that
  function. Flow-sensitive slot tracking (or plain mem2reg before this pass) would
  recover it.
- Declarations that touch `errno` (`nrq_sin`, `nrq_cos`, `nrq_pow_f64`) still
  widen their callers to "touches everything", because the three-location
  `memory(...)` form the pass emits does not carry `errnoMem`. Carrying it would
  need a fourth field in the summary.
- Global variables are treated as `Unknown` (both `other` and `argmem`), which is
  conservative: an access to a global is charged to argument memory even though
  it usually is not.
- `nrq_memory_array_bounds_check`/`_range_check` keep the `memory(none)` promise
  recorded in `po-08`; they are declared, never emitted as bodies, so this pass
  cannot verify that promise. A future task should either lower them or make the
  policy conservative.
- Induction of `willreturn` for runtime routines is per-symbol policy, not
  derived. A new blocking runtime routine defaults to no `willreturn`, which is
  safe but loses the annotation.

## 11. Remaining Optimization Opportunities

1. Consult the pass result during lowering so a function that is provably
   `memory(none)` and tiny is emitted with `alwaysinline`, removing the call
   boundary for helpers the optimizer cannot see through.
2. Run the same provenance machinery over `nrq.` operations before they are
   converted, so an allocation result keeps `Local`/fresh provenance instead of
   collapsing to `Unknown`.
3. Extend the summary with `errnoMem` and `targetMem0/1` so the math family and
   any future target-specific intrinsic stop widening their callers.
4. Combine this pass with the root-liveness result of `po-11`: a function that
   never allocates and never blocks cannot observe a collection, so its GC state
   reads are `inaccessibleMem` only — a refinement the current location
   classification does not express.
