# Nirqoba Performance Optimization Plan (PO_PLAN)

**Document type:** Execution plan  
**Parent specification:** `project_context/PERFORMANCE_ROADMAP.md`  
**Scope of this plan:** PART I, Section 3 — Compiler Optimization Infrastructure  
**Current priority:** Highest priority; complete this section before starting Sections 4–9  
**Status:** Planned and in progress
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

---

## 1. Authority and purpose

This file is the execution plan by Muhammad Tahmid Ahnaf, the creator of nirqoba, for the compiler-optimization portion of the
performance roadmap. It does not replace or weaken
`project_context/PERFORMANCE_ROADMAP.md`; that document remains authoritative for
requirements and acceptance criteria.

The current objective is to turn the existing direct TIR → MLIR → LLVM → object
pipeline into a real optimizing compiler with measurable, reproducible behavior.
Other performance work (CPU targeting beyond the required target plumbing,
parallel execution, numerical/SIMD kernels, GC scaling, allocator benchmarks) is
deferred until Section 3 has passed its acceptance gates.

Rules:

1. Do not mark a task complete without a matching report.
2. Do not report a test or benchmark as passed unless it was actually executed.
3. Do not replace a failing check by weakening its assertions.
4. Do not change language semantics to make an optimization pass easier.
5. Preserve existing runtime ABI, ownership metadata, GC roots, and cleanup
   behavior across optimization.
6. Reuse LLVM/MLIR passes instead of reimplementing standard optimizations.
7. Do not add speculative compiler frameworks. Extend the existing pipeline.
8. Keep the compiler buildable at every milestone.

---

## 2. Current baseline

The repository already has:

- A C++/MLIR/LLVM compiler with TIR verification and MLIR lowering.
- `compiler/lib/Lowering/PassPipeline.cpp` for the MLIR lowering pipeline.
- `compiler/lib/Backend/ObjectEmitter.cpp` for COFF object emission.
- `compiler/lib/Driver/CompilationPipeline.cpp` as the stage orchestrator.
- A Windows x64, MSVC, Ninja, RelWithDebInfo development build at
  `build/msvc-dev` using LLVM/MLIR 23.1.4.
- Unit tests, integration tests, MLIR checks, and a readiness suite.

Current compiler-optimization state:

- Earlier work added unvalidated scaffolding for `-O0`…`-O3`, `-Os`, `-Oz`,
  `--opt-level`, `--target-cpu`, `--target-features`, `--native`, and
  `--emit=llvm-raw`.
- An LLVM `PassBuilder` implementation and MLIR canonicalization/CSE calls exist
  in the worktree but have not been compiled or tested.
- Before that scaffolding, release builds used code-generation optimization
  level `Default` but did not run an LLVM IR optimization pipeline.
- LLVM IR inspection currently has both optimized and raw forms in the planned
  CLI, but their behavior must be verified.
- There is no optimization-correctness matrix across levels and no performance
  baseline for optimization levels.

The first task is therefore to establish a real baseline, not to assume the
scaffolding is correct.

---

## 3. Priority order

All tasks below belong to the current highest-priority section. Execute them in
dependency order.

| ID | Task | Priority | Depends on | Required report |
|---|---|---|---|---|
| PO-1 | Build and baseline validation | P0 | — | `reports/performance/po-01-baseline.md` |
| PO-2 | Optimization levels and IR inspection | P0 | PO-1 | `reports/performance/po-02-optimization-levels.md` |
| PO-3 | MLIR canonicalization, CSE, and cleanup | P0 | PO-1 | `reports/performance/po-03-mlir-cleanup.md` |
| PO-4 | LLVM PassBuilder optimization pipeline | P0 | PO-2, PO-3 | `reports/performance/po-04-llvm-pipeline.md` |
| PO-5 | Optimization correctness and semantic preservation | P0 | PO-4 | `reports/performance/po-05-optimization-correctness.md` |
| PO-6 | Target CPU, features, ABI, and code generation | P0 | PO-4 | `reports/performance/po-06-target-codegen.md` |
| PO-7 | Advanced IPO, loop, vectorization, and inspection support | P1 | PO-5 | `reports/performance/po-07-advanced-optimizations.md` |
| PO-8 | Optimization benchmark and regression gate | P0 | PO-5, PO-6 | `reports/performance/po-08-benchmark-gate.md` |

P0 tasks must complete before work moves to Sections 4–9 of the performance
roadmap. PO-7 may proceed after P0 correctness is green, but no claim of
“optimizing compiler complete” is allowed until every Section 3 checkbox has an
evidence-backed status.

---

## 4. Detailed tasks

### PO-1 — Build and baseline validation

**Objective:** Prove the current compiler builds and establish the exact
pre-optimization baseline.

**Steps**

1. Build in the Visual Studio x64 developer environment with Ninja and the
   `msvc-dev` preset.
2. Run the unit test executable.
3. Run the full integration suite through `tests/run_tests.py`.
4. Record:
   - compiler version output,
   - LLVM/MLIR version,
   - compiler build type,
   - exact build and test commands,
   - unit/integration counts,
   - any pre-existing failures.
5. Capture one unoptimized object/executable and one `--emit=llvm-raw` dump as
   the baseline evidence.
6. Do not change optimization behavior in this task.

**Acceptance**

- Build succeeds.
- Pre-existing test failures, if any, are documented rather than hidden.
- Baseline artifacts and counts are recorded in the report.

**Report must include:** environment, commands, pass/fail counts, existing
failures, and a statement of what is not yet implemented.

---

### PO-2 — Optimization levels and IR inspection

**Objective:** Make optimization levels explicit, validated, and inspectable.

**Steps**

1. Verify or complete CLI support for:
   - `-O0`, `-O1`, `-O2`, `-O3`, `-Os`, `-Oz`;
   - `--opt-level=<level>`;
   - invalid-level diagnostics;
   - help text and option parsing tests.
2. Define one default level and document it. The current intended default is
   `-O2` for normal builds; `-O0` must preserve a direct lowering path.
3. Ensure `-O0` disables non-essential MLIR cleanup and LLVM optimization, but
   still permits mandatory correctness passes.
4. Provide:
   - `--emit=llvm` for optimized LLVM IR;
   - `--emit=llvm-raw` for pre-optimization LLVM IR;
   - `--emit=mlir` with both pre-lowering and post-lowering views.
5. Make `--verbose` report the selected optimization level, target triple, and
   CPU model.
6. Add CLI unit tests for every accepted level, the long form, invalid levels,
   and the two LLVM emit kinds.

**Acceptance**

- Each level parses and is forwarded to the backend.
- Invalid levels fail with a clear diagnostic and no compiler crash.
- Raw and optimized IR are observably different for a non-trivial program.
- `-O0` produces valid code, even if unoptimized.

**Report:** include a table of accepted spellings, default behavior, and test
cases.

---

### PO-3 — MLIR canonicalization, CSE, and cleanup

**Objective:** Remove redundant high-level IR before LLVM translation.

**Steps**

1. Verify or complete a pipeline that runs:
   - canonicalization,
   - common subexpression elimination,
   - unreachable/dead operation cleanup where safe.
2. Run cleanup before `nrq` → LLVM conversion and again after
   `func` → LLVM conversion where it is legal.
3. Keep the cleanup disabled at `-O0` for debuggability.
4. Ensure passes do not erase operations with side effects, runtime calls, GC
   root operations, ownership transfers, or cleanup calls.
5. Add MLIR regression cases showing duplicate calculations removed and
   side-effecting runtime calls retained.
6. Verify the MLIR lowering pipeline still passes its verifier.

**Acceptance**

- `--emit=mlir` succeeds at all optimization levels.
- Optimized MLIR is smaller or equal in operation count for the regression
  cases.
- No runtime call, cleanup, or GC-root operation disappears incorrectly.
- `-O0` retains the direct lowering shape.

**Report:** include before/after operation counts and the exact MLIR test cases.

---

### PO-4 — LLVM PassBuilder optimization pipeline

**Objective:** Execute the actual LLVM new-pass-manager pipeline before object
emission.

**Steps**

1. Create the target machine first; use it for target analyses and optimization
   cost modelling.
2. Set the module target triple and data layout from that target machine before
   optimization.
3. Build and register the standard analysis managers:
   - loop,
   - function,
   - CGSCC,
   - module.
4. Use:
   - `buildO0DefaultPipeline` for `-O0`;
   - `buildPerModuleDefaultPipeline` for `-O1`/`-O2`/`-O3`;
   - the `O2` pipeline plus `optsize`/`minsize` function attributes for
     `-Os`/`-Oz`, unless the LLVM version provides a better supported mapping.
5. Run the pipeline before code generation and verify the resulting module.
6. Map code-generation levels consistently:
   - `-O0` → `None`,
   - `-O1` → `Less`,
   - `-O2`, `-Os`, `-Oz` → `Default`,
   - `-O3` → `Aggressive`.
7. Reject unsupported optimization levels as internal diagnostics rather than
   silently falling back.
8. Ensure process exit status is preserved on pipeline failure.

**Acceptance**

- Release builds demonstrably execute LLVM transforms, not only codegen.
- `--emit=llvm` differs from `--emit=llvm-raw` for a non-trivial function.
- Optimized IR passes `llvm::verifyModule`.
- Programs produce identical observable behavior at `-O0` and all optimized
  levels for the correctness suite.

**Report:** include pass-pipeline construction details, LLVM API versions,
verification results, and representative raw/optimized IR snippets.

---

### PO-5 — Optimization correctness and semantic preservation

**Objective:** Prove that optimization does not invalidate Nirqoba semantics.

**Steps**

1. Build a differential matrix that runs the same positive programs at
   `-O0`, `-O1`, `-O2`, `-O3`, `-Os`, and `-Oz` and compares:
   - exit code,
   - stdout/stderr,
   - expected files or artifacts.
2. Prioritize:
   - ownership transfer and cleanup,
   - array bounds checks,
   - GC root registration and collection,
   - shared retain/release,
   - file/mutex/mapping/socket cleanup,
   - `defer` and automatic owner drops,
   - borrow/view lifetime checks,
   - module-qualified calls,
   - integer overflow/division behavior already defined by the language.
3. Add negative tests for cases that must be rejected before optimization.
4. Check that runtime calls and cleanup calls are not removed, duplicated, or
   reordered across unsafe boundaries.
5. Add a test proving a deliberate side effect survives all optimization levels.
6. Run the full unit/integration suite at least at the default level and at
   `-O0`.

**Acceptance**

- Every differential case passes at every supported level.
- No semantic test relies on “optimization is off”.
- Any failing case is a release blocker, not a documented workaround.

**Report:** include the full level matrix, counts, failures, and retained test
artifacts.

---

### PO-6 — Target CPU, features, ABI, and code generation

**Objective:** Generate target-aware code without breaking the portable default.

**Steps**

1. Keep the default CPU `generic` and an empty feature string for portable
   builds.
2. Support:
   - `--target-cpu <model>`,
   - `--target-cpu=native` or `--native`,
   - `--target-features <features>`,
   - a configurable triple where the backend supports it.
3. Validate target and CPU failures with diagnostics instead of crashes.
4. Document that `--native` may emit instructions that do not run on older
   CPUs.
5. Inspect generated assembly/LLVM IR for at least one scalar and one
   array-loop case at O2/O3.
6. Verify calling convention and ABI compatibility with the existing runtime.
7. Keep the packaged Windows runtime on the supported target.

**Acceptance**

- Portable default objects do not require host-specific instruction sets.
- Host-tuned builds produce valid executables on this machine.
- Target flags affect code generation only through LLVM, not language
  semantics.
- No ABI mismatch appears at runtime-call boundaries.

**Report:** record CPU detected, flags used, assembly snippets, and any target
features that were not usable.

---

### PO-7 — Advanced IPO, loop, vectorization, and inspection support

**Objective:** Cover the remaining Section 3 optimization requirements using
LLVM’s supported passes, with evidence rather than checkbox claims.

**Steps**

1. Verify that the default pipelines include the required transforms where
   applicable:
   - dead-code and dead-store elimination,
   - constant folding and propagation,
   - sparse conditional constant propagation,
   - CFG simplification and unreachable-block elimination,
   - function inlining with a cost model,
   - interprocedural optimization,
   - loop-invariant code motion,
   - loop unrolling,
   - loop strength reduction,
   - induction-variable simplification,
   - loop vectorization,
   - SLP vectorization,
   - tail-call optimization,
   - escape analysis and allocation elimination,
   - scalar replacement of aggregates,
   - indirect-call/devirtualization opportunities where the IR permits them.
2. For each transform that is claimed, add an IR-level or execution test showing
   the transform occurred or that its absence is structurally justified.
3. Evaluate ThinLTO or equivalent link-time optimization as an optional mode;
   do not make it the default until the runtime and link workflow are proven.
4. Provide optimization remarks and pass diagnostics, for example through an
   opt-in remarks/pass-logging flag. Do not pollute normal compiler output.
5. Provide optimized IR inspection and, if practical, machine-code inspection.
6. Document any LLVM-version limitations.

**Acceptance**

- Every claimed transform has an evidence artifact.
- Optional LTO is clearly marked experimental and cannot silently replace the
  normal link path.
- Remarks/diagnostics are opt-in and do not break existing tests.

**Report:** map each Section 3 checklist item to IMPLEMENTED / TESTED /
DEFERRED with a concrete reason and artifact.

---

### PO-8 — Optimization benchmark and regression gate

**Objective:** Measure whether optimization levels actually improve execution,
and prevent silent regressions.

**Steps**

1. Build a Nirqoba-only benchmark set that does not require Rust or an external
   toolchain:
   - scalar arithmetic/loop,
   - recursion,
   - contiguous array traversal and mutation,
   - small/large allocation churn,
   - string/print-light workload,
   - one ownership/cleanup workload.
2. Compile each workload at `-O0`, `-O1`, `-O2`, `-O3`, `-Os`, and `-Oz` where
   applicable.
3. Use:
   - Release or RelWithDebInfo compiler output,
   - warmups,
   - at least 7 measured repetitions,
   - median plus p50/p95/p99 or a documented equivalent,
   - recorded CPU, OS, compiler version, LLVM version, flags, and commit.
4. Keep correctness checksums alongside timing to prove benchmarks do real work.
5. Establish internal regression thresholds only after a stable baseline exists.
   Separate correctness gates from performance thresholds.
6. Report absolute times first. Only claim a speedup when the measurement design
   supports it.
7. Do not compare against C++/Rust unless equivalent algorithms, data, flags,
   and environment are verified. That comparison remains Section 6U:G work.

**Acceptance**

- Benchmark commands and raw samples are reproducible.
- `-O2`/`-O3` demonstrate a measurable improvement over `-O0` on at least the
  scalar-loop and contiguous-array workloads, or the report explains why not.
- No benchmark claim is based on a single run.
- The report identifies variance and remaining noise.

**Report:** include full methodology, raw/median results, environment metadata,
and limitations.

---

## 5. Mandatory report contract

A task is not complete until its report is written to the required path and
attached to the completion message.

Each report must contain:

1. Task ID and title.
2. Date and compiler commit/working-tree state.
3. Files created, modified, or removed.
4. Architectural changes and rationale.
5. Requirements completed, with direct evidence.
6. Requirements not completed and why.
7. Tests actually executed, with exact commands and counts.
8. Benchmarks actually executed, with methodology and raw or summarized samples.
9. Known limitations, regressions, and technical debt.
10. Remaining work and dependencies.
11. Status using only these labels:
    - NOT STARTED
    - IN PROGRESS
    - IMPLEMENTED
    - TESTED
    - BENCHMARKED
    - VERIFIED
    - BLOCKED
    - DEFERRED

A task may be marked VERIFIED only when implementation, correctness tests, and
applicable performance evidence have all been independently checked.

If a task changes the worktree after its report was written, update the report
in the same turn. Do not leave a stale “passed” claim.

---

## 6. Verification commands

Use the project’s established environment. A plain shell does not include the
MSVC standard-library paths.

```text
# Build inside the Visual Studio x64 developer environment
ninja -C build\msvc-dev

# Unit tests
build\msvc-dev\tests\nirqoba_unit_tests.exe

# Integration tests
python tests\run_tests.py --compiler build\msvc-dev\bin\nirqoba.exe

# Readiness suite, when relevant
python tests\readiness\run_readiness.py --runs 7
```

Additional required checks:

```text
nirqoba build program.nrqba -O0 --emit=llvm-raw
nirqoba build program.nrqba -O2 --emit=llvm
nirqoba build program.nrqba -O3 --emit=mlir
nirqoba run program.nrqba -O0
nirqoba run program.nrqba -O2
nirqoba run program.nrqba -O3
nirqoba run program.nrqba -Os
nirqoba run program.nrqba -Oz
```

Do not run destructive git commands. Do not push unless explicitly authorized.

---

## 7. Exit gate for this plan

Section 3 is complete only when all of the following are true:

- [ ] P0 tasks PO-1 through PO-6 are TESTED.
- [ ] Every Section 3 requirement has an evidence-backed status.
- [ ] All supported optimization levels pass the differential correctness matrix.
- [ ] Optimized LLVM IR is verifier-clean.
- [ ] MLIR cleanup preserves side effects, cleanup, GC roots, and ownership.
- [ ] Portable and host-tuned targets both produce valid executables.
- [ ] The benchmark gate has reproducible evidence for real optimization gains.
- [ ] Each task has its required report under `reports/performance/`.
- [ ] No known critical correctness or memory-safety regression is open.

Only after this gate may work move to PERFORMANCE_ROADMAP Section 4 and later
sections.

---

## 8. Explicitly deferred

The following are out of scope for this plan until Section 3 passes:

- Parallel execution, async runtime, and scheduler work (Section 5).
- Numerical/SIMD kernel and tensor optimizations (Section 6).
- Parallel/fully concurrent GC, compaction, and GC pause-time benchmarking
  (Section 8); generational and incremental collection are implemented.
- Allocator benchmarking and generalized allocator replacement (Section 9).
- Comparative C++/Rust performance claims (Section 10.3 / 6U:G).
- GPU backends (Section 12).

Deferral is not completion. Each deferred item must remain explicitly open in
the relevant report.
