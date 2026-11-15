# Memory Model Migration — Phase 2 Report

## 1. Phase Overview

- **Phase identifier:** MMM Phase 2
- **Phase title:** Deterministic Destruction and Move Semantics
- **Status:** Complete
- **Starting version:** `0.531-beta`
- **Final version:** `0.539-beta`
- **Objective:** Make the Phase 1 ownership representation operational: moves
  transfer ownership without copying, owned values are destroyed exactly once by
  compiler-generated cleanup, and every supported control-flow exit preserves
  that guarantee.

## 2. Scope Delivered

1. Executable move semantics for locals, arguments, returns and aggregate fields.
2. Drop-generated deterministic destruction.
3. Fresh obligations for re-owned slots.
4. Obligation completeness and use-after-destruction verification.
5. Overwrite destruction for conditionally released or moved owners.
6. Explicit release and `defer` participation in obligation bookkeeping.
7. Partial-initialization cleanup through the zeroed `uninit` aggregate.
8. Control-flow cleanup for nested scopes, returns, multiple returns, loops and
   recursion.
9. A dedicated `tests/memory_model` suite included in the full run.
10. Documentation and version records.

Excluded as required: Phase 3 borrow enforcement and lifetime inference, Phase 4
ordinary allocation migration, Phase 5 arenas, Phase 6 weak/shared extensions,
Phase 7 GC selection, Phase 8 FFI and all later phases. No syntax was added.

## 3. Move Semantics

`tir.move` remains a distinct result number that aliases the source during
lowering, so there is no physical copy. The builder clears the source slot and
suppresses its automatic cleanup only when the move dominates that scope exit.
Conditional moves leave the fallback cleanup armed; the null slot makes the
already-moved path release null, which every supported release accepts.

The new owner becomes responsible for the single destruction. A moved-out slot
that later receives a new owner now registers a fresh obligation; previously the
transferred obligation was reused and the replacement's destruction lowered as an
unrecorded call. Sema and TIR verification both reject use after move and double
move.

Destructuring is not used as a move path: tuples already reject owned/shared/
borrowed pointer elements, and resource-bearing aggregates are rejected by
`check_destructuring`. Whole-value and field moves are the supported forms.

## 4. Destruction Strategy

Automatic cleanup is a single `tir.drop` carrying its release:

```text
%20 = tir.load %4
tir.drop %20 obligation=0 release=memory.deallocate
```

Translation expands the drop into the named release on a copy of the function, so
TIR keeps one destruction record and MLIR receives the ordinary runtime call. A
drop is not followed by a second generated call. Explicit user releases remain
calls and discharge their obligation instead of transferring it.

Cleanup order is LIFO within a scope, body owners before parameters, and nested
aggregate fields in reverse declaration order. Each `return` closes all scopes;
`break` and `continue` close the loop-body scopes they leave.

## 5. Control Flow and Overwrites

Assignments over a slot whose automatic cleanup is still armed emit the pending
destruction before storing the replacement. This covers the path-sensitive case:

- the release or move happened on one branch, so the slot is null there;
- the other branch still owns the value, which the overwrite would otherwise leak.

The drop releases the live value on the owning path and null on the moved path.
The replacement then receives a fresh obligation at scope exit. A direct
assignment over a live owner without an armed cleanup path remains rejected by
Sema. A deferred call captures its argument when registered, so rebinding that
owner is safe and the earlier defer still releases the captured value.

## 6. Partial Initialization

`uninit` produces a zero aggregate. Field initialization writes only the selected
fields; recursive destruction releases initialized fields, and zero fields are
no-ops. No per-object initialization bitmap or registry is introduced. This
covers nested aggregates and cleared owned fields. A failure path after a
successful allocation is a release-null object; recoverable allocation failure and
constructors that can fail are not language features.

## 7. Exceptions, Panics and Exit

Nirqoba has no exception, panic or error-propagation control flow, so there is no
unwind cleanup to implement. `system.exit` does not run lexical cleanup; runtime
teardown retires process-owned resources on a best-effort basis. These are
documented limitations, not silently claimed support.

## 8. Compiler and Lowering Integration

- Sema: move/release/defer conflict rules and owner overwrite rules.
- TIR: `Drop` owns its release; obligations are per live owner; a re-owned slot
  gets a new obligation.
- Verifier: missing/duplicate release, unresolved obligation, use after
  destruction, transfer reason/consumer, borrow/lifetime rules retained.
- Lowering: drop expansion on a function copy; move aliasing; lifetime and
  obligation attributes preserved; stack-promotion escape scan follows moves.
- Runtime: no new destruction registry, GC root, safepoint or write barrier. The
  same release calls are used. Ordinary allocation remains GC-backed until
  Phase 4.

## 9. Tests and Evidence

- Unit: `299` tests, `299` passed.
- Full runner (integration + memory model): `519` cases, `519` passed, 0 failed,
  0 skipped.
- `tests/memory_model` has 17 cases covering local/argument/return/field/chained
  and shared moves, conditional moves, early and multiple returns, loop exits,
  recursion, partial initialization, nested aggregates, re-owned slots, owner
  overwrite, defer rebinding and reverse destruction order.
- TIR evidence: `reverse_destruction_order` asserts the shared release appears
  before the raw deallocation.
- MLIR evidence: `MlirPipelineTest.DropLowersToItsReleaseCall` observes
  `nrq_memory_deallocate_checked` from a drop.
- Existing GC, allocator, secure-storage, stack-promotion and integration suites
  passed in the same run.

No benchmark was run. Phase 2 does not add runtime instructions: moves alias,
borrow/end-borrow disappear, and a drop lowers to the release that Phase 1
already emitted. No performance superiority is claimed.

## 10. Known Limitations and Risks

- The verifier is structural and straight-line. It does not prove discharge
  completeness for arbitrary branch graphs; the builder covers the supported
  control flow, but a future path-sensitive pass is still needed for a full proof.
- Direct reassignment of a live owner remains rejected rather than implementing
  move assignment. Releasing first, or conditional overwrite, is the supported
  form.
- No user-defined destructors, recoverable allocation failure, exceptions or
  panics exist, so their cleanup ordering cannot be tested.
- `system.exit` and process teardown do not run lexical cleanup.
- Dynamic owned arrays still reject elements that themselves own allocations.
- Ordinary allocation and its GC interaction remain Phase 4 work.

## 11. Phase Boundary

Phase 3 and later phases were not started. Full borrow/lifetime enforcement,
ordinary non-GC allocation, arenas, weak references, GC selection, FFI and later
work remain unimplemented.
