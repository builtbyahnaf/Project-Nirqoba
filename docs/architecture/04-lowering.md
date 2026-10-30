# 04 — Lowering Pipeline

## 1. Lowering Stages

The MLIR lowering pipeline translates domain-specific and high-level dialect operations down to the LLVM dialect:

1. **`nrq-to-llvm`**:
   Custom MLIR pass implemented in `compiler/lib/Lowering/NrqToLLVMPass.cpp`.
   - Replaces `nrq.print_string` with:
     - Module-level `llvm.mlir.global` constant storing the NUL-terminated UTF-8 string bytes.
     - `llvm.addressof` + `llvm.getelementptr` calculating pointer to the byte sequence.
     - `llvm.mlir.constant` for the byte length.
     - `llvm.call @nrq_print_string(%ptr, %len)`.
2. **`convert-func-to-llvm`**:
   Standard MLIR pass converting `func.func` and `func.return` to `llvm.func` and `llvm.return`.
3. **`reconcile-unrealized-casts`**:
   Standard MLIR pass eliminating intermediate unrealized cast operations across dialect conversions.

## 2. Preservation of High-Level Semantics

Operations with domain semantics (such as `nrq.print_string`) are deliberately preserved in the `nrq` dialect until the dedicated lowering pass. This ensures `--emit=mlir` can inspect high-level operations before lower-level ABI and memory materialization occurs.
