# PO-19 - Profile-Guided Optimization

**Task:** P2 3.10 - Profile-Guided Optimization  
**Date:** 2026-11-05  
**Status:** Completed & Verified (IR inspection, end-to-end profile round trips,
sample-profile consumption, and unit/integration suites passed; runtime,
compilation-time, and peak-memory benchmarks deferred per Section 5 policy)  
**Compiler:** Nirqoba 0.120-beta, LLVM/MLIR 23.1.4, MSVC x64 Debug (`msvc-dev`)

---

## 1. Problem Identified

Before this task every optimization decision was based on static heuristics. The
compiler could produce a profile-less build, but it had no supported way to
observe a real workload, no way to feed the observation back into LLVM, and no
auditable record that a build had been profile-guided:

1. **No profile generation.** There was no instrumentation mode, no profile
   runtime link, and therefore no way to produce `default.profraw` from a
   Nirqoba executable.
2. **No profile consumption.** Hot and cold functions, branch probabilities,
   loop trip counts, code layout, function specialization, and hot/cold
   separation were all estimated by LLVM's static heuristics instead of measured
   execution.
3. **No sample-profile path.** Sampling profilers such as ETW, `perf`, or
   Instruments produce LLVM sample-profile text, but the compiler could not load
   it.
4. **Missing profiles were silent.** A misspelled or empty profile path would
   have been reported by LLVM as a warning and the build would quietly fall back
   to a profile-less pipeline, making the result indistinguishable from an
   unprofiled build.
5. **No reproducibility check.** Nothing built an instrumented executable, ran
   it, merged the raw counters with `llvm-profdata`, rebuilt with the merged
   profile, and verified both the IR and the program's observable behaviour.

---

## 2. Implementation Details

### 2.1 Profile policy vocabulary

**What:** Added `PgoMode` (`None`, `Generate`, `Use`, `Sample`) and
`PgoColdMode` (`Default`, `OptSize`, `MinSize`, `OptNone`), with strict name and
parser functions. The default is `None`, so an ordinary build is byte-for-byte
the same policy as before.

**Where:** `compiler/include/nirqoba/Backend/Optimization.h`,
`compiler/lib/Backend/Optimization.cpp`.

The policy is carried in `CommandLineOptions`, copied into `PipelineOptions`,
then into `OptimizationOptions`; no later stage re-parses the command line.
`--verbose` prints the active `pgo:` mode, profile path, and cold-function mode
before optimization.

### 2.2 User-facing controls

`build`, `run`, and `check` accept:

| Option | Meaning |
|---|---|
| `--pgo-generate` | Instrument the optimized program so a representative run writes raw profile counters. |
| `--pgo-use=<file>` | Optimize with a merged instrumentation profile produced by `llvm-profdata merge`. |
| `--pgo-sample=<file>` | Optimize with an LLVM sample-profile text file. |
| `--pgo-cold=<mode>` | How functions the profile marks cold are optimized: `default`, `optsize`, `minsize`, or `optnone`. |

Generation and consumption are mutually exclusive; passing conflicting options
is rejected during argument parsing. `--pgo-use` and `--pgo-sample` require a
non-empty path.

### 2.3 Passing the policy to LLVM

LLVM already owns profile instrumentation, profile loading, and every
transformation that consumes profile facts. Nirqoba therefore maps its policy to
`llvm::PGOOptions` and hands it to the existing `PassBuilder`:

| Nirqoba mode | `PGOOptions::PGOAction` | Effect |
|---|---|---|
| `generate` | `IRInstr` | Adds front-end/IR instrumentation and lowers the counter globals. |
| `use` | `IRUse` | Loads the merged instrumentation profile before the optimization pipeline. |
| `sample` | `SampleUse` | Loads an LLVM sample profile into the sample-profile loader. |
| `none` | `NoAction` | No profile pass; unchanged pipeline. |

`pgo_options_for` also maps `PgoColdMode` to
`PGOOptions::ColdFuncOpt::{Default, OptSize, MinSize, OptNone}`, which is LLVM's
supported control for how cold functions are optimized.

The profile affects the decisions the task requires through LLVM's own passes:

| Requirement | LLVM mechanism |
|---|---|
| Hot/cold function identification | Profile summary plus per-function entry counts (`ProfileSummaryInfo`). |
| Branch probability estimation | Instrumentation branch counters or sample line counts feeding `BranchProbabilityInfo`. |
| Inlining decisions | Call-site hotness in the CGSCC inliner. |
| Loop optimization | Measured loop trip counts and block frequencies used by the loop passes and vectorizer. |
| Code layout | Profile block/edge frequencies used by machine block placement and function splitting. |
| Function specialization | Profile-guided specialization inside `IPSCCP`/function specialization. |
| Hot-path/cold-path separation | Cold-function treatment plus profile-driven function/block splitting. |

No profile pass or inliner is reimplemented in Nirqoba; the compiler contributes
policy, validation, and linking.

### 2.4 Profile generation and runtime linking

**What:** For `--pgo-generate`, the driver locates and links
`clang_rt.profile-x86_64.lib` from the packaged clang-only toolchain. The LLVM
SDK used for headers and libraries does not contain compiler-rt, so the search is
limited to the expected
`<toolchain>/<name>/lib/clang/<version>/lib/windows/` layout. If the runtime is
missing, the build fails with `NRQ5003` rather than producing an executable that
cannot write a profile.

Instrumented executables write `default.profraw` in their current working
directory when they exit normally. The compiler does not post-process that file;
merging is the documented `llvm-profdata merge` step, which keeps profile
production reproducible and identical to the standard LLVM workflow.

### 2.5 Validation and auditability

**What:** `validate_pgo_profile` rejects a profile that does not exist, is a
directory, or is empty with diagnostic `NRQ5009`. This turns a silent fallback
into a build error.

Every profile-guided module records:

```llvm
!{i32 1, !"nrqoba.pgo.mode", !"use"}       ; or "generate" / "sample"
!{i32 1, !"nrqoba.pgo.cold", !"optsize"}   ; requested cold policy
```

The profile path itself is deliberately not embedded: an absolute
build-machine path would make otherwise identical artifacts differ, while the
mode, the cold policy, and LLVM's profile-summary metadata are the facts a
consumer needs.

### 2.6 End-to-end test harness

**What:** The integration runner gained two modes.

- `pgo_roundtrip` performs the complete workflow: instrumented build → execute
  the executable in the profile directory → require a non-empty
  `default.profraw` → `llvm-profdata merge` → profile-use IR build with
  assertions → profile-use executable build → run and compare output.
- `pgo_sample` builds twice with a checked-in sample profile, inspects the IR for
  `ProfileFormat SampleProfile` and the recorded mode, then runs the optimized
  program and compares output.

Sample profiles come from external samplers, so that fixture is intentionally
checked in. Instrumentation profiles are never checked in: they are produced by
executing the test program and merged during the test run.

---

## 3. Changes Made

| File / Component | Change | Reason |
|---|---|---|
| `compiler/include/nirqoba/Backend/Optimization.h` | Added `PgoMode`, `PgoColdMode`, name/parsers, `OptimizationOptions` fields | One explicit PGO vocabulary. |
| `compiler/lib/Backend/Optimization.cpp` | `pgo_options_for`, cold mapping, validation, `nrqoba.pgo.*` flags, `PassBuilder` wiring | Make the policy affect LLVM and record it. |
| `compiler/include/nirqoba/CLI/CommandLine.h` | Added PGO fields | Carry the policy from the parser. |
| `compiler/include/nirqoba/Driver/CompilationPipeline.h` | Added PGO fields | Keep pipeline options complete. |
| `compiler/lib/CLI/Driver.cpp` | `--pgo-*` parsing, conflict checks, help text, forwarding | Expose PGO to users. |
| `compiler/lib/Driver/CompilationPipeline.cpp` | Backend forwarding, verbose `pgo:` line, profile runtime lookup/link | Make generation usable and observable. |
| `compiler/include/nirqoba/Diagnostics/Diagnostic.h` | Added `kPgoProfileMissing = "NRQ5009"` | Report invalid profile inputs. |
| `tests/run_tests.py` | `find_llvm_profdata`, `pgo_roundtrip`, `pgo_sample` modes | Reproduce generation and consumption end to end. |
| `tests/unit/optimization_test.cpp` | PGO mode and cold-mode parse/name tests | Pin the vocabulary. |
| `tests/unit/cli_test.cpp` | Default, long-form, sample, cold, conflict, and rejection tests | Pin CLI plumbing. |
| `tests/integration/positive/pgo_generate_instrumentation/` | Instrumented IR assertions | Prove counters and runtime hooks are emitted. |
| `tests/integration/pgo/profile_consumption_branch/` | Branch-heavy round trip | Prove profile generation, merge, consumption, and behaviour. |
| `tests/integration/pgo/profile_consumption_loop/` | Loop-heavy round trip | Prove PGO is not tied to one workload shape. |
| `tests/integration/pgo/sample_profile_consumption/` | Sample-profile fixture and execution | Prove the sample path loads and preserves behaviour. |
| `tests/integration/negative/pgo_missing_profile/` | `NRQ5009` assertion | Prove missing profiles fail loudly. |
| `README.md` | PGO usage and round-trip instructions | Document the user-facing workflow. |
| `reports/performance/po-19-profile-guided-optimization.md` | Created this report | Record design, evidence, and deferred measurements. |

---

## 4. Verification & Testing

| Test / Verification | Result | Evidence |
|---|---|---|
| Unit suite (Debug) | PASS | 138/138 tests. |
| Integration suite (Debug) | PASS (PGO) | 236 cases, 235 passed. The only failure is the pre-existing, unrelated `positive/memory_advanced` case, whose expected version string is stale relative to the uncommitted working-tree version bump. |
| PGO parsing and CLI | PASS | Default `None`/`Default`; `--pgo-generate`; `--pgo-use=... --pgo-cold=minsize`; `--pgo-sample ...`; conflicting options and invalid cold modes rejected. |
| Instrumentation IR | PASS | `@__profc_main`, `@__profd_main`, `@__llvm_profile_runtime_user`, `@llvm.used`, `nrqoba.pgo.mode = generate`, `nrqoba.pgo.cold = default`. |
| Instrumented executable | PASS | The linked program runs and writes a non-empty `default.profraw` on exit. |
| Branch round trip | PASS | Instrumented run prints `92750`; merged profile produces `ProfileSummary` and `function_entry_count`, no `@__profc_main`; optimized run prints `92750`. |
| Loop round trip | PASS | Instrumented run prints `1641750`; merged profile produces `ProfileSummary` and `function_entry_count`, no `@__profc_sum_squares`; optimized run prints `1641750`. |
| Sample-profile consumption | PASS | `ProfileFormat SampleProfile`, `NumFunctions 3`, `nrqoba.pgo.mode = sample`; optimized run prints `1499585`. |
| Missing profile | PASS | Build exits non-zero with `NRQ5009`. |
| Reproducibility | PASS | Profiles are regenerated by executing the instrumented program in a temporary directory; no binary or profile artifact is checked in. |
| Runtime performance (PGO vs non-PGO) | DEFERRED | Section 5 verification requires explicit project-owner permission. No speedup is claimed. |
| Compilation time / peak memory | DEFERRED | Same Section 5 restriction; no timing or memory numbers are claimed. |

### Reproducing the evidence

```powershell
# 1. Instrumented build. The executable links the LLVM profile runtime.
build\msvc-dev\bin\nirqoba.exe build main.nrqba -O2 --pgo-generate -o app_instrumented.exe

# 2. Run a representative workload; this writes default.profraw in the cwd.
.\app_instrumented.exe

# 3. Merge raw counters into the format the use pass expects.
toolchain\llvm-23.1.4\bin\llvm-profdata.exe merge -o app.profdata default.profraw

# 4. Rebuild with the profile and inspect the IR.
build\msvc-dev\bin\nirqoba.exe build main.nrqba -O2 --pgo-use=app.profdata --emit=llvm

# 5. Tune cold functions.
build\msvc-dev\bin\nirqoba.exe build main.nrqba -O2 --pgo-use=app.profdata --pgo-cold=minsize

# Sample profiles skip generation entirely.
build\msvc-dev\bin\nirqoba.exe build main.nrqba -O2 --pgo-sample=app.sample

# The automated round trip:
python tests\run_tests.py --compiler build\msvc-dev\bin\nirqoba.exe --configuration Debug
```

---

## 5. Safety and Acceptance

- **Correctness:** Profile use only annotates LLVM IR with measured facts; the
  transformations remain LLVM's. Both round-trip workloads produce the same
  observable output before and after profile consumption.
- **No silent fallback:** `--pgo-use` and `--pgo-sample` validate the input and
  fail with `NRQ5009` for a missing, empty, or directory path.
- **Reproducibility:** Instrumentation is generated by the compiler, the profile
  is produced by executing the program, and merging uses the standard
  `llvm-profdata` tool. No checked-in profile can drift from the workload.
- **No absolute paths in artifacts:** Only the mode and cold policy are recorded
  as module flags; the profile path is not embedded.
- **No single-workload dependence:** The suite covers branch-heavy and
  loop-heavy instrumentation round trips plus a sample-profile workload with a
  hot and a cold path.
- **Runtime linking:** `--pgo-generate` links `clang_rt.profile-x86_64.lib`
  explicitly because Nirqoba invokes the linker directly rather than through
  clang. A missing runtime is a build error, not a broken executable.
- **Limitations:** The sample-profile fixture is a small hand-written function
  profile, not a capture from a production sampler. It proves the format is
  loaded and the mode is recorded; it does not claim line-level sampling
  fidelity. `--pgo-cold` is passed through to LLVM's supported cold-function
  control and recorded in the module; a direct cold-attribute assertion is not
  made because LLVM may fully inline a small cold helper during optimization.
- **Deferred measurements:** Section 5 requires explicit project-owner
  permission before performance verification. This report therefore makes no
  runtime, compilation-time, or peak-memory claim. The PGO and non-PGO build
  commands, plus the existing `benchmarks/` harness, are ready for that
  comparison when permission is granted.
- **Trade-offs:** Instrumented builds are slower and larger because every edge
  and function carries counters. Generating a useful profile requires a
  representative workload; a profile captured from an unrepresentative run can
  bias inlining and layout the wrong way. Sample profiles avoid instrumentation
  overhead but depend on an external sampler and debug information quality.

---

**End of PO-19 report.**
