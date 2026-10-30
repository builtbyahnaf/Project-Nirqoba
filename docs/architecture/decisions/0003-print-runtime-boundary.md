# ADR 0003: Print and Runtime ABI Boundary

## Status
Accepted

## Context
A simple `printf` shim could be generated directly in LLVM IR to display output for early programs. However, Nirqoba requires a dedicated runtime library boundary to support precise garbage collection tracing, thread-aware I/O, and platform-independent abstractions in later phases.

## Decision
Lower the language builtin `print` through a dedicated MLIR operation (`nrq.print_string`), which lowers to a C ABI call into `nrqoba_runtime.lib` (`nrq_print_string`).
The runtime implementation on Windows uses Win32 `WriteFile` directly rather than CRT `stdio` buffers.
