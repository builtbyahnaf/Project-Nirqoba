# PO-16 - Function and Call Optimization

**Task:** P1 3.7 - Function and Call Optimization
**Date:** 2026-11-05
**Status:** Completed & Verified (IR/assembly inspection and execution tests
passed; runtime benchmarks deferred per Section 5 policy)
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, MSVC x64 Debug (`msvc-dev`)

---

## 1. Problem Identified

The task asks the compiler to expose inlining, tail calls, tail recursion,
known-call-target and calling-convention facts to LLVM, to stop materialising
temporary call frames, and to avoid unnecessary argument conversions. Before
this task the generated IR made several of those optimizations impossible or
unreliable:

1. **Every generated function was externally visible.** A helper that the
   program never calls still had to be emitted, and LLVM could not reason about
   or delete it. The object file kept all helpers even at `-O2`.
2. **Returned calls carried no `tail` marker.** LLVM's x86 backend only
   performs a sibling call when the IR says the call is in tail position. At
   `-O0`, where no optimization pipeline runs, deep recursion therefore grew the
   stack until it overflowed.
3. **Every frontend local and parameter had a stack slot.** A by-value scalar or
   aggregate parameter cost an `alloca`, a `store` and a `load` per use even when
   the slot was written exactly once. At `-O0` nothing later promoted those
   slots, so the overhead reached the object file unchanged.
4. **The IR did not distinguish argument adaptation from real work.** The only
   integer conversions attached to a call were the ones required by the narrow
   runtime ABI (for example `nrq_print_int(i64)`), never conversions around a
   Nirqoba-to-Nirqoba call.

Nirqoba has no virtual dispatch, traits, interfaces, or first-class function
values: every `tir.call` names a symbol. Devirtualization and indirect-call
elimination therefore have nothing to remove in the current language. The task
list's devirtualization item is documented as not applicable rather than
implemented as an empty pass.

## 2. Root Cause

- `compiler/lib/Lowering/TirToMlir.cpp` emitted `func.func` for every user
  function without a linkage choice. Conversion to `llvm.func` reproduced the
  default external linkage, so the module looked like an ABI boundary for every
  helper.
- The `tail` marker is part of the LLVM IR contract, not something the backend
  may invent later. The lowering knew that a call's result was returned
  unchanged but never wrote that fact down. At `-O2` LLVM's own pipelines often
  recover it; at `-O0` there is no such pipeline, so the information was simply
  lost.
- The frontend lowering intentionally gives each local an address so ownership,
  GC-root registration and definite assignment have a stable home. That is the
  right default, but a slot whose initialising store is the only write and whose
  every other use is a load carries no information the stored SSA value lacks.
  The lowering did not recognise that case.
- Runtime calls use a deliberately narrow C ABI while language calls use the
  callee's declared TIR types. The lowering already converted at the runtime
  boundary only; no change was needed for language calls, but there was no test
  pinning that property.

## 3. Files Modified

- `compiler/lib/Lowering/PassPipeline.cpp`
  - `MarkTailCallsPass`: marks a direct call in tail position with the LLVM
    `tail` kind.
  - `InternalizeFunctionsPass`: gives every generated function except `main`
    internal linkage.
  - `build_lowering_pipeline`: runs both passes at every optimization level.
- `compiler/lib/Lowering/TirToMlir.cpp`
  - `analyze_promoted_slots` / `SlotPromotion`: replaces single-assignment,
    address-free, non-managed slots with the stored SSA value.
  - The load lowering consults the promotion map; the alloca and initialising
    store are skipped for promoted slots.
- `tests/integration/positive/tail_call_marker/` (new)
- `tests/integration/positive/tail_call_deep_recursion/` (new)
- `tests/integration/positive/tail_recursion_accumulator/` (new)
- `tests/integration/positive/internal_function_linkage/` (new)
- `tests/integration/positive/internal_dead_function_removal/` (new)
- `tests/integration/positive/parameter_slot_promotion/` (new)
- `tests/integration/positive/parameter_slot_reassignment/` (new)
- `tests/integration/positive/inlined_constant_call/` (new)
- `tests/integration/mlir/direct_call_arguments/` (new)
- Existing expectations updated where internal linkage or the new `tail`
  marker changed the printed IR:
  `borrow_attribute_readonly`, `dead_store_elimination`,
  `pointer_alias_attrs`, `recursive_numeric_no_gc_frame`,
  `function_memory_effects`.
- This report.

## 4. Algorithm or Transformation Implemented

### 4.1 Tail-position marker

`MarkTailCallsPass` walks `llvm.call` operations and marks one with
`tailcallkind::Tail` only when all of the following hold:

- the call has no marker yet;
- the immediately following operation is `llvm.return`;
- for a value-returning call, the return operand is exactly the call result;
  for a void call, the return has no operands;
- calling and called functions are both definitions in this module (external
  declarations such as `nrq_print_int` are left alone);
- their function types and calling conventions are identical and the callee is
  not variadic.

The pass runs after the final cleanup round and before internalization. It runs
at `-O0` as well as optimized levels because the marker describes the program,
not an optimization decision.

The adjacency requirement is also the GC safety condition: a function with a
root frame emits `nrq_gc_root_frame_end` and `nrq_gc_mutator_leave` before its
return, so any call that still has GC teardown after it is not adjacent to the
return and is never marked.

### 4.2 Internal linkage

`InternalizeFunctionsPass` sets `llvm.linkage = #llvm.linkage<internal>` on
every `llvm.func` that has a body, except `main`. Declarations are untouched
because they refer to the runtime; `main` stays external because the C runtime
starts there. With internal linkage LLVM's inliner, global optimizer, IPSCCP
and dead-function elimination can see the helpers as module-local code. On x64
LLVM is then free to choose an internal calling convention (`fastcc`).

### 4.3 Single-assignment slot promotion

`analyze_promoted_slots` considers an `alloca` eligible when:

- the next TIR operation stores into that slot;
- the slot's type contains no managed-reference offset (such a slot may be a
  registered GC root and must keep its address);
- no later operation writes the slot, takes its address, or passes it as an
  operand/aggregate element - every later use must be a plain `load`.

For an eligible slot, the load lowering returns the stored SSA value and the
alloca/store are not emitted. The analysis is conservative: assigned
parameters, reassigned locals, address-taken slots and aggregate slots that
hold managed references all keep their storage.

### 4.4 Deliberately not added

No custom inliner, tail-recursion eliminator, function specializer or
devirtualizer was written. LLVM already owns those transformations; the change
here is to stop hiding the necessary facts (linkage, tail position, slot
identity) from it. Function specialization for known constant arguments is
likewise left to LLVM's interprocedural passes (and, for cross-module cases,
the future LTO work in 3.9).

## 5. Why the Approach Is Correct

- **Tail marker.** `tail` is an LLVM hint with defined semantics: the callee may
  reuse the caller's frame, and the backend must still verify ABI eligibility
  before emitting a jump. Marking an actually-returned call is therefore safe,
  and the pass refuses every case it cannot prove (different signature,
  variadic callee, external declaration, GC teardown after the call). The
  `llvm.return` adjacency check means no operation can observe caller state
  between the call and the return.
- **Internal linkage.** The compiler emits one module per compilation and the
  language has no FFI/export attribute. Only the C entry point is named by the
  platform. Making the remaining definitions local is therefore a statement of
  fact, not a new assumption. If an export mechanism is added later, it must
  exempt the exported symbols from this pass.
- **Slot promotion.** The four conditions are exactly the definition of a
  write-once, read-only slot. The store dominates every load (it immediately
  follows the alloca, and TIR is in program order), so replacing a load with
  the stored value cannot change a value. Excluding managed-reference
  containing types preserves the address identity GC root registration
  requires. Excluding any other use preserves address-taken and aliasing
  semantics.
- **Argument conversions.** The lowering already converts only at the runtime
  boundary, where the C ABI genuinely differs from the language type. The new
  test pins the direct-call case so a future change cannot silently add a
  truncate/extend pair.

## 6. Tests Added or Modified

| Test | Mode | What it pins |
|---|---|---|
| `positive/tail_call_marker` | build, `-O0` | Both directions of mutual recursion print `tail call i64 @...` at `-O0`. |
| `positive/tail_call_deep_recursion` | build+run, `-O0` | 250,000 mutually recursive frames finish and print `42`; without the marker the run overflows the stack. |
| `positive/tail_recursion_accumulator` | build+run, `-O2` | Accumulator-style recursion finishes at 250,000 frames and prints `31250375001`; LLVM eliminates the recursion into a `tailrecurse` cycle (and, at `-O2`, into the closed form). |
| `positive/internal_function_linkage` | build, `-O0` | Helpers are `define internal`; `main` remains external; symbols stay inspectable at `-O0`. |
| `positive/internal_dead_function_removal` | build, `-O2` | Uncalled internal helper disappears from optimized IR. |
| `positive/parameter_slot_promotion` | build, `-O0` | Promoted helpers contain no `alloca`; parameters are used as SSA values. |
| `positive/parameter_slot_reassignment` | build+run | Reassigned parameters keep their slot and still produce the correct result. |
| `positive/inlined_constant_call` | build, `-O2` | Small internal helpers are inlined, constants folded, and helper definitions removed (`@nrq_print_int(i64 5150)` only). |
| `mlir/direct_call_arguments` | build, `-O0` | A Nirqoba call passes `i32`/`i8`/`i64` arguments directly; no conversion pair surrounds the call. |
| `mlir/recursive_numeric_no_gc_frame` | build, `-O3` | Numeric recursion has no GC root frame and uses `define internal fastcc` plus `tail call fastcc`. |

Existing tests whose expectations changed because of this task were updated to
the new, correct IR (internal linkage, `tail call`, no parameter slots).

## 7. IR and Assembly Evidence

### 7.1 Tail marker at `-O0`

`nirqoba build tail_call_marker/main.nrqba --emit=llvm -O0`:

```llvm
define internal i64 @is_even(i64 %0) #1 {
  %2 = icmp eq i64 %0, 0
  br i1 %2, label %3, label %4
3:
  ret i64 1
4:
  %5 = sub i64 %0, 1
  %6 = tail call i64 @is_odd(i64 %5)
  ret i64 %6
}
```

Both directions are marked. The functions are internal and have no parameter
slot after promotion.

### 7.2 Tail recursion and internal calling convention at `-O3`

`recursive_numeric_no_gc_frame` at `-O3`:

```llvm
define internal fastcc i64 @fib(i64 %0) unnamed_addr #1 {
  %2 = icmp slt i64 %0, 2
  br i1 %2, label %common.ret, label %tailrecurse
...
tailrecurse:
  %3 = add nsw i64 %.tr2, -1
  %4 = tail call fastcc i64 @fib(i64 %3)
  ...
}
```

The recursive call is in a `tailrecurse` cycle, and LLVM selected `fastcc` for
the internal helper. The same function contains no `nrq_gc_root_frame_begin` or
`nrq_gc_root_frame_end`.

### 7.3 Inlining of small helpers at `-O2`

`inlined_constant_call` after optimization:

```llvm
define noundef i64 @main() local_unnamed_addr #0 {
  %1 = tail call i32 @nrq_print_int(i64 5150)
  ret i64 0
}
```

Neither `add_one` nor `twice` survives. `--remarks=inline` on the call-heavy
corpus reports the decisions directly:

```text
remark: <unknown>:0:0: 'add_one' inlined into 'twice' with (cost=-30, threshold=337)
remark: <unknown>:0:0: 'twice' inlined into 'main' with (cost=-15030, threshold=337)
remark: <unknown>:0:0: 'sum_to' not inlined into 'sum_to' because it should never be inlined (cost=never): recursive
```

Ordinary inlining is therefore LLVM's decision, made from accurate linkage and
cost information.

### 7.4 Argument conversions

`mlir/direct_call_arguments` at `-O0`:

```llvm
define internal i32 @add32(i32 %0, i32 %1) #1 {
  %3 = add i32 %0, %1
  ret i32 %3
}

define internal i8 @low_byte(i64 %0) #1 {
  %2 = trunc i64 %0 to i8
  ret i8 %2
}

define i64 @main() #2 {
  %1 = call i32 @add32(i32 10, i32 20)
  %2 = sext i32 %1 to i64
  %3 = call i32 @nrq_print_int(i64 %2)
  %4 = call i8 @low_byte(i64 300)
  %5 = zext i8 %4 to i64
  %6 = call i32 @nrq_print_int(i64 %5)
  ret i64 0
}
```

The `sext`/`zext` pairs belong to the `nrq_print_int` ABI, not to the Nirqoba
calls. `add32` receives its `i32` arguments directly; `low_byte` receives the
`i64` language `int` and performs its declared narrowing once inside the callee.

### 7.5 Leaf and sibling-call code generation at `-O0`

Disassembly of the call-heavy corpus at `-O0` (`llvm-objdump -d`):

```text
<add_one>:
  movq %rcx, %rax
  addq $0x1, %rax
  retq                      ; leaf: no frame, no prologue

<twice>:
  subq $0x28, %rsp
  callq <add_one>
  movq %rax, %rcx
  addq $0x28, %rsp
  jmp   <add_one>           ; returned call reused the frame
```

`count_down` likewise ends in `jmp` to itself, which is what lets the 250,000
frame `-O0` test finish. The leaf helper has no stack frame at all.

## 8. Before-and-After Results

Runtime benchmarks remain deferred under Section 5 of the roadmap (not executed
without explicit permission). The following static, reproducible measurements
were taken from the emitted IR/objects.

### 8.1 IR quality on the test corpus at `-O0`

| Metric | Before PO 3.7 | After PO 3.7 |
|---|---:|---:|
| Entry-block stack slots | 818 | 646 |
| Instructions in generated functions | 5882 | 5297 |

(The current suite, now including the two new regression tests, reports 646
slots and 5332 instructions across 238 generated functions.)

### 8.2 Object size

| Program | `-O0` before | `-O0` after | `-O2` before | `-O2` after |
|---|---:|---:|---:|---:|
| Mutual recursion (`mutual.nrqba`) | 925 B | 875 B | 567 B | 387 B |
| Call-heavy corpus (`calls.nrqba`) | 1256 B | 1206 B | 638 B | 427 B |

At `-O2` only `main` remains in either object: the internal helpers are inlined
and then deleted. At `-O0` the symbols remain for debugging, but they are local
and the parameter slots are gone.

### 8.3 Execution correctness

- `tail_call_deep_recursion` at `-O0`: prints `42` (250,000 mutual frames).
- `tail_recursion_accumulator` at `-O2`: prints `31250375001`.
- Full integration suite: **212 / 212 passed**.
- Unit suite (`ctest --preset windows-msvc-dev`): **1 / 1 passed** (all unit
  tests inside the harness).

## 9. Compilation-Time and Memory Impact

No new analysis framework was introduced. Slot promotion is one linear scan per
function; the two module passes each walk the module once. Both are dwarfed by
LLVM's own pipelines, and no extra module or global data structure is retained.
The `-O2` measurements show a code-size decrease, not an increase: dead internal
helpers disappear and the surviving `main` is smaller. Compile-time benchmarking
is part of the deferred Section 5 verification, so no runtime or compile-time
claim beyond the static measurements above is made here.

## 10. Known Limitations

- The `tail` marker is a hint, not `musttail`. A target or a callee with an
  incompatible ABI may decline the sibling call. This is intentional: `musttail`
  would turn an ineligible call into a hard error.
- Tail marking requires an immediately following `return` and identical,
  non-variadic function types. Calls separated from the return by a conversion
  or by GC teardown stay unmarked.
- Slot promotion handles only write-once, read-only, address-free slots that
  contain no managed reference. Reassigned variables, address-taken slots and
  GC-root-bearing aggregates keep their storage.
- Devirtualization has no current target in the language because there is no
  indirect call form. If function values or trait objects are added, this item
  must be revisited.
- Internal linkage assumes no FFI exports. The compiler currently has no
  export attribute; adding one requires exempting those symbols.
- Runtime benchmarks, CPU counters and cross-language comparisons are deferred
  under the Section 5 policy.

## 11. Remaining Optimization Opportunities

- Interprocedural constant propagation and specialization across modules belong
  to the LTO work in 3.9; within a module LLVM's IPO passes already own them.
- Profile-guided inlining and hot/cold splitting belong to 3.10.
- Scalar replacement of aggregates inside call arguments is covered by LLVM
  SROA once the slots are gone; larger aggregate ABI experiments should be
  measured before adding a custom rule.
- If FFI exports or indirect calls are added to the language, an escape/export
  analysis must feed both the internalization pass and a future devirtualizer.
