# 03 — MLIR Dialect Architecture

## 1. The `nrq` Dialect

Phase 1 establishes the Nirqoba-specific out-of-tree MLIR dialect: `nrq`.
- Defined using TableGen in `compiler/include/nirqoba/Lowering/NrqOps.td`.
- C++ namespace: `::nirq::nrq`.
- Phase 1 operation:
  - `nrq.print_string`: Carries a string attribute representing the literal text to write to stdout.

## 2. Dialect Registry

Dialects are registered uniformly through `register_all_dialects()` in `compiler/include/nirqoba/Lowering/RegisterDialects.h`.
In Phase 1, the registry contains:
- `mlir::BuiltinDialect`
- `mlir::func::FuncDialect`
- `mlir::LLVM::LLVMDialect`
- `nirq::nrq::NrqDialect`

## 3. Future Dialect Responsibilities

As planned in the roadmap:
- `nrq.tensor`: Multidimensional array operations, shapes, broadcasting, lowering to Linalg and TOSA (Phase 19).
- `nrq.graph`: Typed computational graph nodes (`Node<T>`) and autodiff (Phase 20–21).
- `nrq.async`: Coroutines, task scopes, suspension points, lowering to Async and LLVM coroutines (Phase 10).
- `nrq.gpu`: Device kernels, stream launches, memory transfers, lowering to GPU and NVVM dialects (Phase 23).
