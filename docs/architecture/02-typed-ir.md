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
