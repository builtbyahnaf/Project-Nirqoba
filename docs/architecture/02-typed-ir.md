# 02 — Typed Intermediate Representation (TIR)

## 1. Role of TIR

TIR sits between the AST and the MLIR dialect translation layer. It provides:
1. Complete type information on all values and operations (so MLIR translation never has to deduce types).
2. Clean separation between language syntax concepts and MLIR dialect representations.
3. Stable textual representation (`--emit=tir`) for verification, debugging, and golden regression testing.

## 2. Operation Set (Phase 1)

- `ConstantInt`: Produces an `i64` or `i1` immediate value.
- `ConstantString`: Reference to a string literal.
- `Alloca`: Allocates a typed stack slot for a named local variable.
- `Store`: Stores a value into an allocated slot.
- `Load`: Loads a value from an allocated slot.
- `Binary`: Arithmetic operation on two operands (`add`, `sub`, `mul`, `div`, `rem`).
- `PrintString`: Language-level print operation carrying literal string text.
- `Return`: Function terminator returning an optional value.

## 3. Verification Invariants

The `Verifier` (`verify_tir_module`) enforces structural invariants:
- Every function must contain at least one operation.
- Every function must terminate with a `Return` operation.
- Every referenced `tir::Value` ID must be in bounds for the containing function.
- Load/Store slot types must match the loaded/stored value types.
- Return operand types must match the declared function return type.

## 4. Ownership Representation and Destruction (MMM Phases 1–3)

Every value, parameter and return carries a `ValueOwnership` category. Four
ownership operations make the semantic relationships verifiable without changing
generated code:

- `tir.move` records a transfer, its source and its reason (local, return,
  argument, field or element). The source becomes unusable; the result aliases it
  during lowering.
- `tir.borrow` records a non-owning value, its owner and the selected storage path,
  plus immutable/mutable access.
- `tir.end_borrow` ends a lexical borrow before the owner's cleanup. Returned
  borrows stay open and are described by a function-level lifetime relation.
- `tir.drop` discharges a `DestructionObligation` and owns the release call it
  names. Lowering expands a drop into that call on a copy of the function, so TIR
  has one destruction record and MLIR gets a normal runtime call. Transfer marks
  the obligation as owed by the new owner instead.

Functions store `LifetimeRecord` and `DestructionObligation` tables next to the
operation stream. Translation to MLIR publishes them as `nirqoba.ownership`,
`nirqoba.lifetime_relations` and `nirqoba.destruction_obligations` attributes.

The verifier rejects use after move, double moves, a transfer reason that does not
match its consumer, mutability/origin/lifetime mismatches on borrows, double-ended
borrows, unknown or twice-discharged obligations, an obligation whose owner is not
a storage location, a drop without its release or with a duplicate release, a
release family mismatch, an obligation that is never discharged or transferred,
use of a value after its destruction, and a reserved (`Weak`/`ArenaBound`) return
category. The structural checks run over the operation stream; Phase 3 adds a
separate control-flow borrow analysis for the rules that are not local to one
operation.

## 5. Borrow Flow over the CFG (MMM Phase 3)

`verify_borrow_flow` builds basic blocks from `label`, `branch` and
`branch_cond`, then computes a fixed point of active borrows and invalid owner
storage roots at each block entry. A merge unions every restriction that may
still be active on an incoming path, and loop back-edges participate in the same
fixed point. This is what rejects a borrow after an owner was moved or destroyed
on only one branch.

A backward liveness pass over the same CFG decides whether a borrow still
restricts later code. Aliases include the address-of value, copies, loads from
local bindings that hold the pointer, and borrowed call results forwarded from a
declared source parameter. A borrow whose aliases are all dead stops conflicting,
which avoids treating a scope-end marker as a borrow that is live until the
lexical boundary.

The pass also checks the persistent lifetime table: source, path and mutability
must match the `tir.borrow` operation; a borrow has one lexical record; a record
without an operation is stale. Writes overlapping a live borrow, moves or drops
of a borrowed owner, aggregate borrow escapes through returns or calls, and
mutable/immutable borrowed-return mismatches are rejected with `NRQ4007`.

`tir.borrow` and `tir.end_borrow` still lower to nothing. The pass adds
compile-time analysis only; it does not add a runtime borrow flag, allocation,
GC root, safepoint or write barrier.

## 6. Allocation Strategy (MMM Phase 4)

Allocation calls carry a strategy contract in TIR. The verifier derives the
strategy from the callee and checks both the value's `ValueOwnership` and its
storage shape:

- `memory.allocate`, `memory.reallocate` and `memory.secure_allocate` require
  `owned` ownership and a plain pointer;
- `memory.owned_array` and `memory.owned_array_aligned` require `owned` ownership
  and an owned-array descriptor;
- `array.allocate` requires `gc_managed` ownership and a managed pointer;
- `memory.shared_allocate` and `memory.shared_retain` require `shared` ownership
  and a pointer.

Ordinary release calls (`memory.deallocate`, `memory.secure_release` and
`array.owned_release`) reject a `gc_managed` value, and an owned-array release
requires an owned array. A `tir.drop` now accepts only a genuinely owning
category; collector-managed values have no lexical destruction obligation.

The contract changes no generated code. It prevents a later transformation from
rewriting an allocation name or ownership record into a different memory
strategy without a verifier failure. The ordinary path remains
`nrq_memory_allocate` / `nrq_memory_deallocate_checked`; only explicit
`array.allocate`, managed-reference payloads and SoA remain collector-backed.

