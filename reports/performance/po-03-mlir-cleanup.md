# PO-3 — MLIR canonicalization, CSE, and cleanup

**Date:** 2026-11-03  
**Status:** TESTED  
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, RelWithDebInfo  
**Base commit:** `2f617ab` with the worktree changes listed below

---

## 1. Scope

This task makes the MLIR cleanup stage of the lowering pipeline explicit,
complete, and regression-tested before LLVM IR translation:

1. Canonicalization, common-subexpression elimination (CSE), and dead-code /
   unreachable-block cleanup before `nrq` lowering.
2. The same cleanup again on the resulting LLVM-dialect IR.
3. No cleanup at `-O0`, so the direct lowering shape remains inspectable.
4. A regression test proving duplicate pure operations are removed while calls,
   runtime operations, and control flow with side effects are retained.
5. A test proving `-O0` keeps the un-cleaned shape.

## 2. Pipeline changes

`compiler/lib/Lowering/PassPipeline.cpp` now routes both cleanup stages through
one shared helper:

```text
canonicalize
-> cse
-> trivial-dce (removes trivially dead ops and unreachable blocks)
```

The pass list is inserted before `nrq-to-llvm` and again after
`convert-func-to-llvm` + `reconcile-unrealized-casts`.

Implementation notes:

- MLIR 23.1.4 no longer exposes `mlir::createRemoveUnreachableBlocksPass`. The
  equivalent supported pass is `createTrivialDeadCodeEliminationPass`, which
  removes side-effect-free operations with no users and, by default, blocks that
  are unreachable from their region entry.
- The canonicalizer and CSE consult MLIR memory-effect interfaces. Operations
  without a proven side-effect model are neither deleted nor deduplicated, so
  runtime calls, GC-root operations, ownership transfers, and cleanup calls are
  preserved.
- `-O0` still runs only the mandatory lowering passes (`nrq-to-llvm`,
  `convert-func-to-llvm`, `reconcile-unrealized-casts`); no cleanup pass is
  inserted.

## 3. Regression tests

New file: `tests/unit/mlir_pipeline_test.cpp` (registered in
`tests/CMakeLists.txt`).

The test builds one MLIR module containing all of the following:

- two identical value-producing multiplications that must be merged by CSE;
- two duplicate constants that must be merged;
- one trivially dead multiplication whose dead constant must be removed;
- two calls with unknown side effects that must both survive;
- one `nrq.print_int` that must lower to a runtime call and must not be erased;
- one unreachable block that must be removed.

Observed result after the optimized pipeline (`optimize=true`):

| Operation | Before | After |
|---|---:|---:|
| `llvm.mul` | 3 | 1 |
| `llvm.mlir.constant` | 3 | 1 |
| `llvm.return` | 2 | 1 |
| `llvm.call` | 0 (pre-lowering) | 3 |

Observed result with the `-O0` pipeline (`optimize=false`):

| Operation | Before | After |
|---|---:|---:|
| `llvm.mul` | 3 | 3 |
| `llvm.mlir.constant` | 3 | 3 |
| `llvm.return` | 2 | 2 |
| `llvm.call` | 0 (pre-lowering) | 3 |

The three retained calls are the two `@side_effect` calls and the lowered
`nrq_print_int` call. This proves the cleanup is not deleting or duplicating
side-effecting operations.

## 4. Validation executed

Build:

```text
cmd /c build\msvc-dev\build_project.bat
```

Result: PASS.

Unit suite:

```text
build\msvc-dev\tests\nirqoba_unit_tests.exe
```

Result: 102 / 102 PASS (includes the two new `MlirPipelineTest` cases).

Integration suite:

```text
python tests\run_tests.py --compiler build\msvc-dev\bin\nirqoba.exe
```

Result: 173 / 173 PASS.

`--emit=mlir` at every supported level on
`tests/integration/mlir/inline_bounds_guard/main.nrqba`:

| Level | Result |
|---|---|
| `-O0` | exit 0 |
| `-O1` | exit 0 |
| `-O2` | exit 0 |
| `-O3` | exit 0 |
| `-Os` | exit 0 |
| `-Oz` | exit 0 |

Semantic spot check with a program that contains a duplicated computation
(`tests` probe, `a = x * 2`, `b = x * 2`, `a + b`):

| Level | Output |
|---|---|
| `-O0` | `84` |
| `-O2` | `84` |
| `-O3` | `84` |

## 5. Requirements mapping

| PO-3 requirement | Status | Evidence |
|---|---|---|
| Canonicalization before lowering | TESTED | pipeline helper; unit test |
| CSE before lowering | TESTED | 3 muls -> 1, 3 constants -> 1 |
| Dead-operation cleanup | TESTED | dead mul and its constant removed |
| Unreachable-block cleanup | TESTED | 2 returns -> 1 |
| Cleanup after `func` -> LLVM conversion | TESTED | same helper applied twice |
| Cleanup disabled at `-O0` | TESTED | `O0PipelineKeepsDirectLoweringShape` |
| Side-effecting runtime/cleanup calls retained | TESTED | 3 calls retained in both modes |
| `--emit=mlir` succeeds at all levels | TESTED | six-level table above |
| Optimized MLIR op count not larger | TESTED | op-count table above |

## 6. Limitations

- The cleanup is intentionally local and conservative: it does not perform
  interprocedural analysis, inlining, or loop transformations. Those belong to
  PO-5/PO-7 and LLVM's pipeline.
- Trivial DCE does not remove dead use-def cycles and does not run liveness
  analysis. That is sufficient for current lowering output; a later value-range
  or liveness-based pass can be added if IR size becomes a problem.
- This report does not claim performance gains. Its purpose is structural
  cleanup and semantic safety evidence.

## 7. Files changed

- `compiler/lib/Lowering/PassPipeline.cpp`
- `compiler/include/nirqoba/Lowering/PassPipeline.h`
- `tests/unit/mlir_pipeline_test.cpp` (new)
- `tests/CMakeLists.txt`
- `reports/performance/po-03-mlir-cleanup.md` (new)
