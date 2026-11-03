# PO-1 — Build and baseline validation

**Date:** 2026-11-03  
**Status:** VERIFIED  
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, RelWithDebInfo  
**Build tree:** `build/msvc-dev`

---

## 1. Environment and commands

Built with the MSVC x64 developer environment and Ninja:

```text
call "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
ninja -C build\msvc-dev
```

The installed `vcvars64.bat` reports a non-fatal warning because `vswhere.exe`
is not on `PATH`; the compiler and linker environment still resolves correctly.

Unit suite:

```text
build\msvc-dev\tests\nirqoba_unit_tests.exe
```

Integration suite:

```text
python tests\run_tests.py --compiler build\msvc-dev\bin\nirqoba.exe
```

## 2. Baseline result

| Check | Result |
|---|---|
| Compiler build | PASS |
| Unit tests | 100 / 100 PASS |
| Integration tests | 170 / 170 PASS |
| Existing optimization scaffolding | Present but not independently validated before this pass |

Baseline performance evidence from the checked-in stress benchmark:

| Workload | Before |
|---|---:|
| `fib(40)`, 102M recursive calls | 9,739.06 ms |
| 1M × 64-byte allocate/free | 175–233 ms across local runs |

## 3. Milestone 1 — recursion and heap fast paths

This report covers the first code-generation milestone completed after the
baseline.

### 3.1 GC root frames only where roots exist

Every function previously executed:

- `nrq_gc_root_frame_begin()` — an atomic marker allocation plus a thread-local
  vector push;
- `nrq_gc_root_frame_end(marker)` — a mutex lock, frame validation, and root
  cleanup.

Those calls were emitted even for functions with no GC-managed parameters,
locals, return values, or GC operations. They prevented LLVM from applying tail
recursion elimination to numeric recursive functions.

`FunctionTranslator` now emits a root frame only when
`function_uses_gc()` is true. Functions without roots still keep their normal
mutator/safepoint behavior when applicable.

The optimized `fib` now lowers to a tail-recursive loop with a single recursive
call, and the generated LLVM IR contains no GC root-frame calls.

### 3.2 Thread-local default allocation recycling

The default `GeneralAllocator` now recycles small freed blocks on the allocating
thread:

- cache limit: 64 blocks per thread;
- maximum cached request: 1024 bytes;
- exact size and alignment matching;
- only the process default general allocator is eligible;
- thread-selected allocators, explicit allocator handles, and a custom
  process-wide default keep the existing registry and pinning path;
- registry ownership is removed before a block enters the cache and reinserted
  on reuse, so double-free, shared-release, profiler, and cross-thread ownership
  checks remain intact;
- the cache is drained through `heap_free_aligned` at thread exit.

## 4. Validation after milestone 1

| Check | Result |
|---|---|
| Unit tests | 94 / 94 PASS |
| Integration tests | 171 / 171 PASS |
| New recursion/GC regression | PASS |
| `fib(40)` correctness | `102334155` |
| Alloc/free correctness | `1000000` |

The new integration test is
`tests/integration/mlir/recursive_numeric_no_gc_frame`. It checks that optimized
LLVM IR contains the tail-recursive `fib` call and does **not** contain
`nrq_gc_root_frame_begin` or `nrq_gc_root_frame_end`.

`tests/run_tests.py` now supports `expect_stdout_not_contains` so absence-based
code-generation guarantees can be tested directly.

## 5. Focused measurement

Measurement used 2 warmups and 7–9 measured runs of the existing Nirqoba stress
programs with `-O3`.

| Workload | Before | After | Change |
|---|---:|---:|---:|
| `fib(40)` | 9,739 ms | 261 ms | 37.3× faster |
| 1M allocate/free | 175–233 ms | 111 ms | ~1.6–2.1× faster |

The recursion result is a genuine algorithmic improvement: LLVM can now convert
the second recursive call into a loop after the blocking runtime calls are
removed. The allocation result reflects removal of the HeapAlloc/HeapFree pair
and one registry lock from the common path.

## 6. Limitations

- The allocation measurement is a focused local check, not a full comparative
  C/C++/Rust benchmark run.
- The cache covers only the default general allocator and small blocks. Explicit
  pool/arena/TLSF strategies keep their existing behavior.
- GC-managed functions still use root frames and should be optimized separately
  once their root frames can be proven empty at compile time.
- Further allocator work is needed to approach the best platform allocator
  numbers on cross-thread and larger-allocation workloads.

## 7. Files changed

- `compiler/lib/Lowering/TirToMlir.cpp`
- `runtime/src/allocator_windows.cpp`
- `tests/run_tests.py`
- `tests/unit/allocator_test.cpp`
- `tests/integration/mlir/recursive_numeric_no_gc_frame/`
- `reports/performance/po-01-baseline.md`
