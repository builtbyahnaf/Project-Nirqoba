# PO-3 follow-up — Dead-code elimination and dead-store elimination

**Date:** 2026-11-03  
**Status:** TESTED  
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, RelWithDebInfo  
**Base commit:** `2f617ab` plus the MLIR cleanup and this change in the worktree

---

## 1. Verification of the existing cleanup implementation

Before adding the new stages, the current PO-3 implementation was re-verified:

| Check | Result |
|---|---|
| Build (`cmd /c build\msvc-dev\build_project.bat`) | PASS |
| Unit suite | 102 / 102 PASS |
| Integration suite | 173 / 173 PASS |
| `--emit=mlir` at `-O0/-O1/-O2/-O3/-Os/-Oz` | exit 0 at every level |
| Duplicate-computation program at `-O0/-O2/-O3` | output `84` at every level |

Probe of the pre-existing behavior for a dead store (`y = 111; y = x + 1;`)
and a dead computation (`x * 7 + 12345`):

| Level | `store i64 111` | `12345` |
|---|---|---|
| `-O0` | present | present |
| `-O1` | absent | absent |
| `-O2` | absent | absent |

This confirmed that LLVM's default pipelines already performed the transforms,
but the compiler had no explicit stage, no dedicated regression test, and no
evidence artifact for either requirement.

## 2. Implementation

### 2.1 MLIR dead-code elimination

The MLIR cleanup added in PO-3 (`reports/performance/po-03-mlir-cleanup.md`)
runs:

```text
canonicalize -> cse -> trivial-dce
```

`trivial-dce` removes side-effect-free operations with no users and blocks that
are unreachable from their region entry. It runs before `nrq` lowering and again
after `func`-to-LLVM conversion. `-O0` skips it.

### 2.2 Explicit LLVM dead-code and dead-store elimination

`compiler/lib/Backend/Optimization.cpp` now appends a final function-pass stage
after the standard PassBuilder module pipeline at every optimized level:

```cpp
llvm::FunctionPassManager final_cleanup;
final_cleanup.addPass(llvm::DSEPass());
final_cleanup.addPass(llvm::ADCEPass());
pipeline.addPass(
    llvm::createModuleToFunctionPassAdaptor(std::move(final_cleanup)));
```

- `llvm::DSEPass` (`llvm/Transforms/Scalar/DeadStoreElimination.h` in LLVM 23)
  removes stores whose value is overwritten before any read.
- `llvm::ADCEPass` removes instructions and computations that cannot affect
  observable behavior, including dead loop computations that simpler DCE
  misses.
- The stage runs for `-O1`, `-O2`, `-O3`, `-Os`, and `-Oz`. It is intentionally
  skipped for `-O0`, preserving the direct source-shaped IR.
- Both passes are upstream LLVM passes, consistent with PO_PLAN rule 6 (reuse
  LLVM/MLIR passes instead of reimplementing standard optimizations).

Rationale for an explicit final stage: LLVM's per-module pipeline already
contains DCE and DSE, but module-level transforms (inlining, global
simplification, constant propagation) can expose dead stores and computations
after the per-function simplification stage has already run. The final stage
guarantees the two Section 3 requirements independently of upstream pipeline
ordering and is a no-op when nothing is dead.

## 3. Regression tests

New integration cases under `tests/integration/mlir/`:

| Case | Mode | Assertion |
|---|---|---|
| `dead_store_elimination` | `--emit=llvm -O2` | `define i64 @compute` present; `store i64 111` and `12345` absent |
| `dead_store_retained_at_o0` | `--emit=llvm -O0` | `store i64 111` and `12345` present |
| `dead_store_execution` | run at `-O2` | exit 0, stdout `42` |

The O0/O2 pair proves the elimination is performed by optimization rather than
by frontend lowering, and that `-O0` keeps the direct lowering shape.

## 4. Validation executed

```text
cmd /c build\msvc-dev\build_project.bat
build\msvc-dev\tests\nirqoba_unit_tests.exe
python tests\run_tests.py --compiler build\msvc-dev\bin\nirqoba.exe
```

| Check | Result |
|---|---|
| Build | PASS |
| Unit suite | 102 / 102 PASS |
| Integration suite | 176 / 176 PASS (3 new cases) |
| `dead_store_elimination` | PASS |
| `dead_store_retained_at_o0` | PASS |
| `dead_store_execution` | PASS, stdout `42` |

Optimized IR evidence (`--emit=llvm -O2`):

```llvm
define i64 @compute(i64 %0) local_unnamed_addr #0 {
  ...
  ret i64 %2
}
```

No `store i64 111` and no `12345` remain. At `-O0` the same function still
contains both.

## 5. Requirements mapping

| PERFORMANCE_ROADMAP Section 3 requirement | Status | Evidence |
|---|---|---|
| Implement dead-code elimination | TESTED | MLIR `trivial-dce`; explicit LLVM `ADCEPass`; O0/O2 IR tests |
| Implement dead-store elimination | TESTED | Explicit LLVM `DSEPass`; `store i64 111` absent at O2, present at O0 |

## 6. Limitations

- DSE is LLVM's store-level analysis; it does not perform cross-module or
  whole-program store elimination, and ThinLTO remains future work.
- Dead functions are not removed because user functions have external linkage.
  Removing unused public functions requires whole-program or LTO analysis and is
  not claimed here.
- No optimization remarks or pass logging exists yet to show which pass removed
  a specific instruction; that remains PO-7 work.
- No runtime performance claim is made. This task verifies the transforms, not
  benchmark deltas.

## 7. Files changed

- `compiler/lib/Backend/Optimization.cpp`
- `tests/integration/mlir/dead_store_elimination/main.nrqba` (new)
- `tests/integration/mlir/dead_store_elimination/expect.json` (new)
- `tests/integration/mlir/dead_store_retained_at_o0/main.nrqba` (new)
- `tests/integration/mlir/dead_store_retained_at_o0/expect.json` (new)
- `tests/integration/mlir/dead_store_execution/main.nrqba` (new)
- `tests/integration/mlir/dead_store_execution/expect.json` (new)
- `reports/performance/po-03-dce-dse.md` (new)
