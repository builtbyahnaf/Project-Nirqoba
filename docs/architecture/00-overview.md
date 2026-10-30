# 00 — Architecture Overview

## 1. Compiler Pipeline

Project Nirqoba implements a staged compiler architecture built on top of C++, MLIR, and LLVM:

```text
Source (.nrqba)
      |
SourceFile & SourceLocation
      |
Maximally-Munching Lexer
      |
Recursive-Descent Parser (AST)
      |
Semantic & Type Analysis (TypeChecker, SymbolTable)
      |
Typed Nirqoba Representation (TIR)
      |
TirToMlir Translation
      |
Nirqoba MLIR Dialect (`nrq`)
      |
Pass Pipeline (`nrq-to-llvm`, `convert-func-to-llvm`, `reconcile-unrealized-casts`)
      |
LLVM IR Translation
      |
ObjectEmitter (TargetMachine: x86_64-pc-windows-msvc)
      |
Native COFF Object (.obj)
      |
Linker (lld-link / link.exe) + nrqoba_runtime.lib
      |
Native Executable (.exe)
```

## 2. Stage Boundaries & Invariants

Each compiler stage has strict ownership and knowledge boundaries:
- **Lexer**: Tokenizes UTF-8 source buffer into a flat `std::vector<Token>`. Never throws or exits; emits diagnostic records and produces `Invalid` tokens when scanning fails.
- **Parser**: Builds an Abstract Syntax Tree (AST). Does not resolve types or symbols. Recognizes Phase 2 operators and keywords, rejecting them with explicit `NRQ2100` phase diagnostic codes.
- **Sema**: Resolves symbols lexically and validates types. Populates type annotations on AST expressions. Does not lower or emit code.
- **TIR**: Target-independent typed intermediate representation. Guarantees SSA-style value numbering, explicit stack allocation slots, and verification invariants before lowering to MLIR.
- **MLIR (`nrq` dialect)**: Encapsulates high-level Nirqoba semantics (such as `nrq.print_string`) before lowering to generic LLVM dialect representations.
- **Backend & Linker**: Generates native machine code via LLVM TargetMachine, emits COFF objects, and links with system CRT and Nirqoba runtime libraries.
