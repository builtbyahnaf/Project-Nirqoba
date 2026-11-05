# PO-18 - Interprocedural Optimization

**Task:** P1 3.9 - Interprocedural Optimization  
**Date:** 2026-11-05  
**Status:** Completed & Verified (IR inspection, multi-module correctness, symbol
handling, and unit/integration suites passed; runtime, compilation-time, and
peak-memory benchmarks deferred per Section 5 policy)  
**Compiler:** Nirqoba 0.120-beta, LLVM/MLIR 23.1.4, MSVC x64 Debug (`msvc-dev`)

---

## 1. Problem Identified

Before this task the optimizer already saw one LLVM module per compilation, but
the compiler had no way to state or tune how much interprocedural work should
happen, and several facts that make that work possible were either incomplete or
unnecessarily expensive to compute:

1. **No interprocedural policy.** `-O0`–`-O3` selected LLVM's per-module
   pipeline, but a user could not ask for a bounded compile (no inlining) or for
   deeper analysis (LLVM's Attributor) without changing the optimization level
   and therefore every unrelated decision.
2. **No auditable record of the LTO strategy.** An optimized artifact did not
   say whether it had been built as one whole-program unit, nor which policy
   governed inlining and specialization.
3. **Global dead code could survive.** LLVM's per-module pipeline contains
   DCE, but inlining and global simplification can expose unreachable functions
   after the last built-in module cleanup. Without a guaranteed final
   `GlobalDCE`, an internal helper that became unreachable at the end of the
   pipeline could still be emitted.
4. **Memory-effect inference rescanned the whole module.** The lowering pass
   that derives `memory_effects` for generated functions iterated every
   definition repeatedly until a global fixpoint. On a merged multi-module
   program that is `O(functions × iterations)` even when only one callee
   changed, which works against the scalable-LTO requirement.
5. **Specialization and cross-module inlining were untested.** The front end
   merges imported modules before LLVM runs, but no test pinned that a helper
   defined in another `.nrqba` module is inlined, folded, specialized, or
   removed, nor that same-named private helpers in different modules stay
   distinct.

---

## 2. Implementation Details

### 2.1 Explicit IPO policy

**What:** Added `IpoMode` (`Standard`, `Aggressive`, `Minimal`), a name
function, and a strict parser. `--ipo=<mode>` and `--ipo <mode>` are accepted by
`build`, `run`, and `check`; the default is `standard`.

**Where:** `compiler/include/nirqoba/Backend/Optimization.h`,
`compiler/lib/Backend/Optimization.cpp`,
`compiler/include/nirqoba/CLI/CommandLine.h`,
`compiler/include/nirqoba/Driver/CompilationPipeline.h`,
`compiler/lib/CLI/Driver.cpp`,
`compiler/lib/Driver/CompilationPipeline.cpp`.

The policy is carried in `CommandLineOptions`, copied into `PipelineOptions`,
then into `OptimizationOptions`; no stage re-parses the command line.

**Behaviour:**

| Mode | Inliner budget | Extra analysis | Intended use |
|---|---|---|---|
| `standard` (default) | LLVM's level-dependent default | none | Normal release builds |
| `aggressive` | at least 400 | LLVM `AttributorPass` after inlining; speculative devirtualization enabled | Builds that can trade compile time for deeper facts |
| `minimal` | 0 (no inlining) | none | Very large programs where compile time and code growth dominate |

`IPSCCP`, `GlobalOpt`, `GlobalDCE`, and the per-function cleanup pipeline still
run in every mode. `minimal` therefore still performs interprocedural constant
propagation and dead-function removal; it only declines cloning. `-O0` remains
unoptimized in every mode.

### 2.2 Whole-program LTO as the scalable strategy

**What:** The compiler resolves every `import` into one `ModuleUnit` set, lowers
all of them into a single MLIR module, and exports a single LLVM module. The
backend therefore always optimizes the complete program, which is the
equivalent of full LTO without a separate link-time stage.

**Why this is the scalable equivalent of ThinLTO:** ThinLTO exists to recover
cross-module visibility when a build produces many object files and links them
later. Nirqoba's compilation model has no such boundary: module resolution
happens before TIR lowering and the object emitter sees the already-merged
program. Adding a second bitcode/link step would duplicate work and introduce a
new artifact format, not add visibility. The compiler instead records the
strategy so the choice is explicit and auditable.

**Evidence:** every optimized module contains:

```llvm
!{i32 1, !"nrqoba.ipo.mode", !"standard"}
!{i32 1, !"nrqoba.ipo.strategy", !"whole-program"}
```

The mode follows `--ipo`; `ipo_policy_metadata` pins the `aggressive` spelling.

### 2.3 Internal linkage and guaranteed global DCE

**What:** `InternalizeFunctionsPass` gives every generated definition except the
C entry point `main` internal linkage. The backend then appends a final
`DSEPass` + `ADCEPass` function stage and a module-level `GlobalDCEPass` after
LLVM's default pipeline. Internal linkage is what makes removal of a helper
legal; the final `GlobalDCE` is what guarantees removal happens even when the
last inlining round exposes it.

**Where:** `compiler/lib/Lowering/PassPipeline.cpp`,
`compiler/lib/Backend/Optimization.cpp`.

Evidence (`ipo_cross_module_global_dce`): a public `helper.unused` defined in
another source module disappears from `--emit=llvm` output, while the folded
`@nrq_print_int(i64 42)` from `helper.used` remains.

### 2.4 Interprocedural memory effects

**What:** `InferFunctionMemoryEffectsPass` now solves the same greatest
fixpoint with a worklist over a reverse call graph instead of rescanning every
definition repeatedly.

- Declarations with known `memory_effects` are fixed inputs.
- Declarations without effects force conservative caller summaries.
- Each definition starts optimistic (no effects) and grows monotonically; a
  changed summary enqueues only its direct callers.
- Each summary has six monotone bits, so a definition can change at most six
  times. A change therefore costs work proportional to the affected callers,
  not to the size of the whole module.

**Where:** `compiler/lib/Lowering/PassPipeline.cpp`.

Because all source modules are merged, a call from `main` to
`helper.combine` in another module is an ordinary direct call in the analysed
module; the inferred fact crosses the source-module boundary. Evidence
(`ipo_cross_module_memory_effects`):

```mlir
llvm.func internal @helper.combine(%arg0: i64, %arg1: i64) -> i64
  attributes {memory_effects = #llvm.memory_effects<other = none,
    argMem = none, inaccessibleMem = none, ...>}
```

### 2.5 Escape analysis and devirtualization

**Interprocedural escape analysis** is expressed as `noalias`, `captures(none)`,
and `readonly` on generated parameters, derived from Nirqoba's ownership and
borrowing rules, plus LLVM's own capture/alias analyses. `--ipo=aggressive`
additionally runs the Attributor, which is LLVM's canonical implementation of
interprocedural capture and alias reasoning. Evidence
(`ipo_cross_module_escape`, `-O0`, so the attributes come from Nirqoba rather
than from a later LLVM cleanup):

```llvm
define internal void @helper.copy_scaled(ptr noalias captures(none) %0,
                                         ptr readonly captures(none) %1)
```

**Devirtualization is not applicable.** Nirqoba has no virtual dispatch,
traits, interfaces, or first-class function values, so no call site in the
language can be indirect through a virtual table. The `aggressive` tuning sets
LLVM's speculative-devirtualization flag so the policy remains correct if such
a construct is added later, but no current program exercises it and the report
makes no devirtualization claim.

### 2.6 Optimization responsibilities

Ordinary transformations are LLVM's, per the architectural rules:

| Requirement | Owner |
|---|---|
| Interprocedural constant propagation | LLVM `IPSCCP` (default pipeline) |
| Function specialization | LLVM function specialization inside `IPSCCP` |
| Cross-module inlining | LLVM CGSCC inliner over the merged module |
| Global dead-code elimination | LLVM `GlobalDCEPass` (explicit final run) |
| Internal linkage optimization | Nirqoba `InternalizeFunctionsPass` + LLVM |
| Interprocedural escape analysis | Nirqoba ownership facts + LLVM Attributor |
| Interprocedural memory effects | Nirqoba `InferFunctionMemoryEffectsPass` |
| Devirtualization | Not applicable (no virtual dispatch) |
| Scalable LTO | Whole-program merged module |

---

## 3. Changes Made

| File / Component | Change | Reason |
|---|---|---|
| `compiler/include/nirqoba/Backend/Optimization.h` | Added `IpoMode`, name/parser, `OptimizationOptions::ipo` | One explicit policy vocabulary. |
| `compiler/lib/Backend/Optimization.cpp` | `pipeline_tuning_for`, `nrqoba.ipo.*` flags, Attributor, final `GlobalDCE` | Make the policy affect LLVM and record the LTO strategy. |
| `compiler/include/nirqoba/CLI/CommandLine.h` | Added `ipo_mode` | Carry the policy from the parser. |
| `compiler/lib/CLI/Driver.cpp` | `--ipo` parsing, help text, pipeline forwarding | Expose the policy to users. |
| `compiler/include/nirqoba/Driver/CompilationPipeline.h` | Added `PipelineOptions::ipo_mode` | Keep the pipeline options complete. |
| `compiler/lib/Driver/CompilationPipeline.cpp` | Backend forwarding and verbose `ipo:` line | Make the active policy observable. |
| `compiler/CMakeLists.txt` | Linked the LLVM `ipo` component | Provide Attributor and GlobalDCE. |
| `compiler/lib/Lowering/PassPipeline.cpp` | Reverse-call-graph worklist for memory effects | Scale analysis to merged multi-module programs. |
| `tests/unit/optimization_test.cpp` | IPO parse/name tests | Pin the vocabulary. |
| `tests/unit/cli_test.cpp` | Default, long-form, short-form, and rejection tests | Pin CLI plumbing. |
| `tests/unit/mlir_pipeline_test.cpp` | Caller-propagation and writer-effect tests | Pin worklist correctness. |
| `tests/integration/positive/ipo_policy_metadata/` | Asserts `nrqoba.ipo.mode` and strategy | Keep the artifact auditable. |
| `tests/integration/positive/ipo_cross_module_inlining/` | Two-module inline and constant fold | Prove cross-module visibility. |
| `tests/integration/positive/ipo_cross_module_symbols/` | `alpha.helper` vs `beta.helper` | Prove module-qualified symbols do not collide. |
| `tests/integration/positive/ipo_cross_module_global_dce/` | Unused exported helper removed | Prove internalization plus DCE. |
| `tests/integration/positive/ipo_function_specialization/` | Recursive constant-argument specialization | Prove specialized clones exist. |
| `tests/integration/mlir/ipo_cross_module_memory_effects/` | `memory(none)` fact across modules | Prove interprocedural effect inference. |
| `tests/integration/mlir/ipo_cross_module_escape/` | `noalias`/`captures(none)`/`readonly` across modules | Prove escape facts cross modules. |
| `README.md` | `--ipo` usage and whole-program LTO explanation | Document the user-facing control. |
| `reports/performance/po-18-interprocedural-optimization.md` | Created this report | Record design, evidence, and deferred measurements. |

---

## 4. Verification & Testing

| Test / Verification | Result | Evidence |
|---|---|---|
| Unit suite (Debug) | PASS | 135/135 tests. |
| Integration suite (Debug) | PASS | 231/231 cases. |
| IPO parsing | PASS | `standard`, `aggressive`, `minimal` accepted; `full`, empty, and capitalized rejected. |
| CLI plumbing | PASS | Default `Standard`; `--ipo=aggressive`; `--ipo minimal`; invalid mode reports an error. |
| Policy metadata | PASS | `nrqoba.ipo.mode` follows the flag; strategy is `whole-program`. |
| Cross-module inlining | PASS | `helper.scale(14)` and `helper.add(40, 2)` both fold to `@nrq_print_int(i64 42)`; no helper definitions remain. |
| Cross-module symbols | PASS | Program prints `7` then `11`, proving `alpha.helper` and `beta.helper` stayed distinct. |
| Global DCE | PASS | `helper.unused` absent from IR; folded call remains. |
| Function specialization | PASS | Recursive call `@helper.calc(i64 6)` and specialized definition `define internal fastcc range(i64 18, 17) i64 @helper.calc(i64 range(i64 0, 7) %0)` with `memory(none)`. |
| Interprocedural memory effects | PASS | `@helper.combine` carries `other = none, argMem = none, inaccessibleMem = none`. |
| Interprocedural escape | PASS | `@helper.copy_scaled` carries `noalias captures(none)` and `readonly captures(none)`. |
| Worklist unit coverage | PASS | `writer` yields `argMem = Mod`; `propagate_caller` is revisited and also yields `argMem = Mod`; unknown declaration keeps the conservative default. |
| Runtime benchmarks | DEFERRED | Section 5 verification requires explicit project-owner permission. |
| Compilation time / peak memory | DEFERRED | Same Section 5 restriction; no timing or memory numbers are claimed. |

### Reproducing the evidence

```powershell
# Cross-module inlining: both calls must print the constant 42
build\msvc-dev\bin\nirqoba.exe build `
  tests\integration\positive\ipo_cross_module_inlining\main.nrqba `
  --emit=llvm -O2

# Same-named helpers stay distinct; prints 7 then 11
build\msvc-dev\bin\nirqoba.exe run `
  tests\integration\positive\ipo_cross_module_symbols\main.nrqba -O2

# Recursive specialization
build\msvc-dev\bin\nirqoba.exe build `
  tests\integration\positive\ipo_function_specialization\main.nrqba `
  --emit=llvm -O2

# Aggressive policy adds Attributor-derived attributes (!guid) and keeps the
# recorded mode at "aggressive"
build\msvc-dev\bin\nirqoba.exe build main.nrqba --emit=llvm -O2 --ipo=aggressive
```

---

## 5. Safety and Acceptance

- **Correctness:** The worklist preserves the same greatest fixpoint as the
  previous global iteration: optimistic start, monotone growth, caller revisit
  on change, and a fixed conservative result for unknown declarations. No
  attribute is strengthened without a proof from the body or a callee.
- **Memory safety:** Internalization applies only to generated definitions
  other than `main`. Runtime functions remain declarations with their existing
  attributes. No ownership, borrow, GC-root, or cleanup operation is removed or
  duplicated by these changes.
- **Symbol handling:** Module resolution qualifies non-entry symbols with the
  module name before lowering, so same-named helpers in different modules are
  separate LLVM symbols; `ipo_cross_module_symbols` executes the result.
- **No duplicate pipeline:** All ordinary transforms remain LLVM's. Nirqoba adds
  policy, semantic facts, internalization, and one guaranteed final cleanup.
- **Auditability:** `nrqoba.ipo.mode` and `nrqoba.ipo.strategy` are present in
  every optimized module, and `--verbose` prints the active policy.
- **Deferred measurements:** Section 5 requires explicit project-owner
  permission before running performance verification. This report therefore
  makes no runtime, compilation-time, or peak-memory claim. The required
  measurement harness already exists (`benchmarks/`, `tests/readiness/`) and can
  be run when permission is granted.
- **Trade-offs:** `aggressive` can increase compile time and code size through
  more inlining and the Attributor; `minimal` intentionally gives up inlining
  and therefore some constant folding, but retains IPSCCP and GlobalDCE.

---

**End of PO-18 report.**
