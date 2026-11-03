# PO-4 — LLVM PassBuilder optimization pipeline

**Date:** 2026-11-03  
**Status:** TESTED  
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, RelWithDebInfo

---

## 1. Pipeline implementation

The compiler creates the target machine before optimization, then configures the
LLVM module with that machine's data layout and target triple. This is required
before target-aware inlining, vectorization, and code-generation cost modelling.

The pipeline uses the LLVM new pass manager:

1. Register module, CGSCC, function, and loop analyses.
2. Cross-register analysis proxies.
3. Select the pipeline:
   - `-O0`: `buildO0DefaultPipeline`
   - `-O1`, `-O2`, `-O3`, `-Os`, `-Oz`:
     `buildPerModuleDefaultPipeline`
4. Apply `OptimizeForSize` for `-Os` and `MinSize` for `-Oz`, matching the
   function-attribute mechanism LLVM uses to select size-oriented decisions.
5. Verify the module before optimization and again after optimization.
6. Emit the object with code-generation levels `None`, `Less`, `Default`, or
   `Aggressive` as appropriate.

`-Os` and `-Oz` cannot be represented by LLVM 23's four-value
`llvm::OptimizationLevel`; they use the `O2` pipeline with the size attributes
above. That behavior is now explicit and tested.

## 2. Defensive verification

A malformed module now fails with a compiler diagnostic before the optimizer
runs:

- verification before the pipeline prevents optimizer crashes on invalid IR;
- verification after the pipeline prevents invalid transformed IR from reaching
  object emission;
- failure returns a compiler error instead of producing a partial artifact.

## 3. CLI and level validation

All supported spellings are covered by unit tests:

```text
-O0 -O1 -O2 -O3 -Os -Oz
--opt-level=0 ... --opt-level=z
--opt-level 3
```

Invalid values such as `-O9` produce:

```text
error[NRQ5002]: invalid optimization level '-O9';
expected -O0, -O1, -O2, -O3, -Os, or -Oz
```

`--emit=llvm` and `--emit=llvm-raw` are both parsed and tested. The help
diagnostic now lists both forms.

## 4. Correctness across levels

The Fibonacci stress program was compiled and executed at every level:

| Level | Output | Median runtime |
|---|---:|---:|
| `-O0` | 102334155 | 458 ms |
| `-O1` | 102334155 | 252 ms |
| `-O2` | 102334155 | 257 ms |
| `-O3` | 102334155 | 258 ms |
| `-Os` | 102334155 | 263 ms |
| `-Oz` | 102334155 | 334 ms |

All outputs match. The expected level ordering is visible: `-O0` retains the
direct recursive call tree, while optimized builds eliminate the second
recursive call and run as a tail-recursive loop.

The optimized `-O3` LLVM IR contains:

```llvm
%4 = tail call i64 @fib(i64 %3)
```

and no GC root-frame calls for this numeric function.

## 5. Target selection

Verified:

- default target: `x86_64-pc-windows-msvc`, CPU `generic`;
- `--target-cpu=native` compiles and runs the integer benchmark correctly;
- an unknown CPU produces an LLVM diagnostic and a compiler failure rather than
  silent success;
- `--verbose` reports the selected optimization level, target triple, and CPU.

## 6. Validation

| Check | Result |
|---|---|
| Unit tests | 100 / 100 PASS |
| Integration tests | 171 / 171 PASS |
| All six optimization levels | PASS |
| Raw vs optimized IR distinction | PASS |
| `-O9` rejection | PASS |
| Native CPU compilation | PASS |

New tests:

- `tests/unit/optimization_test.cpp` — level parsing, names, invalid values;
- `tests/unit/cli_test.cpp` — optimization flags, target selection, LLVM emit
  variants;
- `tests/integration/mlir/recursive_numeric_no_gc_frame` — optimized tail
  recursion and absence of redundant GC frame calls.

## 7. Files changed

- `compiler/lib/Backend/Optimization.cpp`
- `compiler/include/nirqoba/Backend/Optimization.h`
- `compiler/lib/CLI/Driver.cpp`
- `tests/CMakeLists.txt`
- `tests/unit/cli_test.cpp`
- `tests/unit/optimization_test.cpp`
- `reports/performance/po-04-llvm-pipeline.md`

## 8. Remaining pipeline work

- ThinLTO/link-time optimization remains optional future work.
- Optimization remarks and pass-logging flags are not yet exposed.
- Full comparative C/C++/Rust benchmarking is outside this focused validation.
- Advanced loop/vectorization evidence still needs per-transform artifacts under
  PO-7.
