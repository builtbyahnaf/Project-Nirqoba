# PO-7 — Inline bounds guards and loop vectorization

**Date:** 2026-11-03  
**Status:** BENCHMARKED  
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, RelWithDebInfo

---

## 1. Why `-O0` is slower

`-O0` keeps the source-shaped call tree. For `fib(40)`, every recursive call
remains a real call, and every one of the roughly 102 million leaf calls executes
the full recursive tree.

At `-O1` and above, LLVM rewrites the second recursive call into an accumulator
loop, reducing the hot path to one recursive call per level. That is an
algorithmic code-shape change performed by the optimizer, not a change in the
source program.

The earlier 9.7-second result was **not** `-O0`. It was an optimized build that
still emitted GC root-frame begin/end calls in every numeric function. Removing
those calls restored tail-recursion eligibility. `-O0` now runs `fib(40)` in
about 458 ms, which is comparable to MSVC C at `/O2` on this machine.

## 2. Bounds-check bottleneck

Array indexing used to emit an out-of-line runtime call:

```text
nrq_memory_array_bounds_check(index, length)
```

on every load and every store. That call:

- could not be inlined from the static runtime library;
- was treated as an opaque side-effecting call by LLVM;
- prevented loop-invariant analysis and vectorization;
- added a call/return pair per array element.

This dominated matrix multiplication, quicksort, and array traversal.

## 3. Inline guard implementation

The lowering now emits:

- an inline unsigned comparison `index < length`;
- a conditional branch to a cold failure block;
- a call to `nrq_memory_array_bounds_fail()`, marked `noreturn`, `cold`, and
  `nounwind`;
- an unreachable terminator in that failure block.

The hot path contains no runtime call. LLVM can:

- remove the guard when it is implied by the loop condition or a constant index;
- leave a cold failure edge that does not prevent vectorization;
- vectorize the loop with a scalar fallback when needed.

The runtime failure diagnostic and exit behavior are unchanged.

## 4. Validation

| Check | Result |
|---|---|
| Unit tests | 100 / 100 PASS |
| Integration tests | 173 / 173 PASS |
| Vectorization in matrix IR | PASS |
| Runtime bounds failure | Exit 70, diagnostic present |
| Inline-check MLIR regression | PASS |

New tests:

- `tests/integration/mlir/inline_bounds_guard`;
- `tests/integration/negative/runtime_array_bounds_fail`.

## 5. Focused cross-language results

Measurement: 1 warmup, 5 measured runs, median, `-O3` for Nirqoba, existing
C/C++/Rust binaries built from the same benchmark sources on the same host.
Lower is better.

| Workload | Nirqoba before | Nirqoba now | C | C++ | Rust |
|---|---:|---:|---:|---:|---:|
| Fibonacci `fib(40)` | 9,347 ms | **282 ms** | 465 ms | 412 ms | 250 ms |
| Array traversal | 111 ms | **74 ms** | 74 ms | 84 ms | 69 ms |
| Quicksort 1M | 215 ms | **88 ms** | 95 ms | 96 ms | 94 ms |
| Matmul 512² | 230 ms | **50 ms** | 66 ms | 84 ms | 47 ms |

Current standing:

- Quicksort: beats C, C++, and Rust.
- Matrix multiplication: beats C and C++; within about 7% of Rust.
- Array traversal: ties C; within about 7% of Rust.
- Fibonacci: beats C and C++; within about 13% of Rust.

The remaining gaps are no longer caused by bounds-check calls. They are in the
generated loop shape, alias information, and target-specific tuning.

## 6. Remaining work for the “beat every baseline” goal

1. **Aliasing metadata** — owned-array backing pointers are not currently
   exposed to LLVM as non-aliasing storage. Better alias information can improve
   loop scheduling and remove memcheck versioning.
2. **Allocation fast path** — the 1M allocate/free workload remains around
   111 ms versus roughly 45 ms for C. The ownership registry and thread
   synchronization are the limiting costs, not `HeapAlloc`.
3. **Target tuning** — `-O3` currently uses the portable `generic` CPU. A
   host-tuned profile can close small SIMD gaps but should not be the default.
4. **GC root frames in GC-heavy loops** — remaining safepoint and root-frame
   costs need loop-level proof work.

## 7. Files changed

- `compiler/lib/Lowering/TirToMlir.cpp`
- `runtime/include/nirqoba/runtime/allocator.h`
- `runtime/src/allocator_windows.cpp`
- `tests/integration/mlir/inline_bounds_guard/`
- `tests/integration/negative/runtime_array_bounds_fail/`
- `reports/performance/po-07-advanced-optimizations.md`
