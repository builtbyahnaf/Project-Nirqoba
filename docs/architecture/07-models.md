# 07 — Language and Execution Models

## 1. Type Model

Phase 1 establishes a statically checked scalar type model:
- `int`: Fixed 64-bit signed integer (`i64`). No platform-dependent width variations.
- `bool`: 1-bit boolean (`i1`), literals `true` and `false`.
- `string`: Immutable UTF-8 string literal.
- `void`: Empty return type.
- `<error>`: Sentinel type representing failed analysis to suppress cascading errors.

## 2. Memory Model

- Phase 1 memory is stack-only: local variables are allocated with `Alloca` slots.
- Automatic Garbage Collection by default is planned for dynamic objects in Phase 6.5.
- Deterministic ownership and borrowing are planned for Phase 9.

## 3. Execution Target Model

- CPU-first execution: all code compiles directly to native x86-64 machine instructions.
- No GPU dependency: executables run without requiring a GPU or CUDA runtime.
- Native interoperability: uses the standard C/MSVC ABI.
