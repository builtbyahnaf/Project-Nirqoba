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

## 4. Ownership Representation and Destruction (MMM Phases 1–2)

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
category. The analysis is structural and straight-line; full path-sensitive
discharge proofs for arbitrary branch graphs remain future work, as does Phase 3
lifetime inference.

