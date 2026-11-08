# 00 — Method and Benchmark Provenance

**Investigation:** Nirqoba performance architecture (task `tasks/CPInBD.md`)
**Working tree:** `904aee0` + uncommitted builder fusion experiment (see §6)
**Working version:** 0.266-beta (`CMakeLists.txt` `project(nirqoba VERSION 0.266.0)`)
**Date of measurements:** 2026-11-08, single machine (see §3)
**Status of this document:** verified facts and explicit unknowns. No claim below is inherited without a label.

Labels used throughout `reports/bottleneck_discovery/`:

| Label | Meaning |
|---|---|
| **[SOURCE]** | Read from the checked-out source; file and line given. |
| **[IR/ASM]** | Read from emitted LLVM IR or disassembled machine code. |
| **[MEASURED]** | Produced by an experiment in `benchmarks/bottleneck_discovery/`, raw output preserved. |
| **[PROJECTION]** | Arithmetic derived from a measured or source fact. Not a benchmark result. |
| **[PRIOR REPORT]** | Claim taken from an existing report; not independently reproduced. |
| **[UNKNOWN]** | Not established by this investigation. Do not treat as fact. |

---

## 1. What was measured, and what was not

This investigation did **not** rebuild the compiler, the runtime libraries, or the
five benchmark executables. Instead it measured the runtime in isolation:

* `benchmarks/bottleneck_discovery/gc_harness/` — a C++ harness that reproduces the
  exact runtime-call sequence the compiler emits for benchmark 02 and 03, built
  against the runtime sources of each historical commit extracted with
  `git archive`. This isolates collector/allocation behaviour from compiler and
  OS noise, and makes a three-way bisect possible without rebuilding a 23.1.4
  MLIR compiler eight times.
* `benchmarks/bottleneck_discovery/stage_probe/` — a **copy** of one revision's
  `gc.cpp` with timing and bitmap counters injected by script. Production sources
  were not modified. The injected timing accounts for 77–103 % of the harness's
  own collection median (median ≈ 85 %), so the stage split is representative.
* `benchmarks/bottleneck_discovery/call_probe/` — per-entry-point cost of the
  runtime calls the compiler emits around one object.
* `benchmarks/bottleneck_discovery/header_probe/` — exact `sizeof`/`offsetof`
  measured by compiling the real `gc.cpp` (not by summing field sizes), plus a
  compiled feasibility probe for candidate layouts.
* `benchmarks/bottleneck_discovery/analyze.py` — regenerates every matrix number
  in these reports from `results/raw/` and `results/stage_raw/`.

Raw results are committed under
`benchmarks/bottleneck_discovery/results/` so the tables are reproducible without
rerunning the machine.

**Not measured:** the compiler's own build time, multi-threaded behaviour,
minor-collection cost under mutation, fragmentation workloads, string-heavy
workloads, hardware performance counters (no VTune/ETW capture was taken), and
Java/C/Rust execution under controlled identical heap commitment. Every statement
about those areas is labelled as a hypothesis or left as an open question.

---

## 2. Version provenance — the most important correction

The task states: *"The likely comparison is `0.257-beta` against `0.260-beta`."*
That is **wrong**, and the error matters because it points the investigation at
the wrong commits.

### 2.1 What the repository actually contains

`git log` (newest first), with commit times:

| Commit | Time (UTC+6) | Subject |
|---|---|---|
| `904aee0` | 2026-11-08 11:16 | docs: investigation report on java gap |
| `5d62abf` | 2026-11-08 10:43 | perf(gc): narrow-layout marking |
| `86cfc43` | 2026-11-08 10:40 | **chore: released version 0.260-beta** |
| `0708f75` | 2026-11-08 10:37 | perf(gc): byte-sized mark/sweep slices |
| `1646b93` | 2026-11-08 10:36 | perf(gc): minor skips pages with no young object |
| `e830b65` | 2026-11-08 10:33 | perf(gc): page-ordered sweep |
| `1ca3c37` | 2026-11-08 10:26 | perf(gc): page index for reusable runs |
| `f1d2b42` | 2026-11-08 10:23 | fix(gc): keep the free-span list ordered |
| `c384cee` | 2026-11-08 03:25 | **bench: rerun stress test 2** |
| `98a1099` | 2026-11-08 02:57 | **chore: released version 0.257-beta** |

### 2.2 The committed benchmark file is stale

`git show <rev>:benchmarks/stress_test_2/benchmark_results.json`:

| Revision | `metadata.generated_at` | `compilers` | Nirqoba 02 COLLECT | 03 COLLECT |
|---|---|---|---|---|
| `98a1099` (0.257 release) | 2026-11-07T22:29:23 | all `unknown` | 146 ms | 27 ms |
| `c384cee` (bench rerun) | 2026-11-08T03:13:20 | all `unknown` | 85 ms | 25 ms |
| `86cfc43` (0.260 release) | **2026-11-08T03:13:20** | all `unknown` | 85 ms | 25 ms |
| `904aee0` (HEAD) | **2026-11-08T03:13:20** | all `unknown` | 85 ms | 25 ms |

The ColO-01 commits (`f1d2b42` … `0708f75`) were authored at **10:23–10:37**,
seven hours *after* the benchmark file that the 0.260-beta release commit
carries. **0.260-beta shipped without a benchmark run of its own.** The
"0.260" numbers in the repository are 0.257-era code.

The uncommitted working-tree file
(`benchmarks/stress_test_2/benchmark_results.json`) was generated at
**2026-11-08T11:21:43**, i.e. with the 0.266 compiler built at 11:14
(`build/msvc-dev/bin/nirqoba.exe`). Its Nirqoba medians (140 / 558 / 135 build,
77 / 234 / 88 collect) are exactly the "Later" column supplied in the task.

### 2.3 Correct identification

| Task label | Actually is |
|---|---|
| "Earlier" (137/25, 478/85, 102/25) | `c384cee`, 03:13 run, compiler built from **pre-ColO-01** source (0.257-beta era + `46c1e5c`) |
| "Later" (140/77, 558/234, 135/88) | working tree at 11:21, compiler built from **0.266-beta** (0.260 + `5d62abf`) |
| "0.257 vs 0.260" | incorrect. 0.257's committed run was *worse* than 0.260's stale one (146 vs 85 collect); no 0.260 benchmark exists |

Therefore the regression window is:

```
c384cee (pre-ColO)  ->  f1d2b42 -> 1ca3c37 -> e830b65 -> 1646b93 -> 0708f75
                    ->  86cfc43 (0.260 release) -> 5d62abf -> 904aee0
```

and it is bisected in report `03-collector-colo.md` to a single commit.

### 2.4 Provenance defects that remain

**[UNKNOWN]** The recorded metadata has `compilers: {nirqoba: unknown, msvc:
unknown, rustc: unknown, java: unknown}`. The task's required metadata table
(§2.3) is therefore **not satisfied by any committed artifact**. In particular:

* the exact compiler binary used for the "Earlier" run is not recorded, only
  inferred from the commit time;
* the C/C++/Rust/Java binaries' build flags are in `run_benchmark.py` but the
  compiler versions are not captured;
* Java version, `-Xms/-Xmx`, GC selection and JIT warmup are not recorded in
  the JSON (the runner's command line is in the per-run `command` fields; the
  report uses them only to confirm `System.gc()` and the serial GC intent);
* `memory_bytes` is `null`;
* the task's "Peak-performing compiler: probably 0.257-beta; regressed compiler:
  probably 0.260-beta" cannot be confirmed: 0.257 is slower than the 0.260-era
  code on every metric in the committed data, and 0.260 has no data of its own.

Machine (from the JSON): Windows 11 build 26300, Intel Family 6 Model 154
(Alder Lake), 12 logical processors, 32 GB class (not recorded). Benchmark
timings later in the same session degrade by 10–100 % as the machine heats; see
§4.

---

## 3. Measurement environment used for the new experiments

* MSVC 19.51.36231 (toolset `14.51.36231`), `/O2 /DNDEBUG /std:c++20 /EHsc /MD`,
  `NRQ_RUNTIME_DEBUG_ENABLED=0`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`.
* Harness binaries are built per revision from `git archive <rev> runtime`;
  the compiler and the harness never see a mixed tree.
* Timer: `QueryPerformanceCounter` for harness second-scale work; the original
  benchmarks use `time.now()` at 1 ms resolution (a real weakness, §5).
* Each harness run performs its own warmup rounds; the matrix medians are
  computed over 8 independent process runs for modes 02/03 and 3 runs for
  01/alloc. The stage probe is 3 runs per revision/mode.

---

## 4. Measurement noise — read every number with this in mind

The same binary produced these COLLECT medians across 8 runs:

| Revision | mode | median over 8 runs | min | max |
|---|---|---|---|---|
| `98a1099` | 02 | 86.3 ms | 80.6 | 100.8 |
| `e830b65` | 02 | 197.2 ms | 136.3 | 228.3 |
| `904aee0` | 02 | 224.9 ms | 192.2 | 239.7 |

Build times are worse: `904aee0` mode 02 build ranges 291–757 ms. The machine is
a laptop-class 12-thread part without locked clocks. Consequences:

* **Effect sizes below ~25 % cannot be resolved** by the end-to-end matrix.
  Statements that an optimization is "neutral" mean smaller than this
  resolution, not zero.
* The **stage probe is far more stable** (three runs agree within a few percent)
  because it measures totals over 41 collections per process. Use it for
  attribution; use the matrix only for end-to-end direction and magnitude.
* Any future optimization claim must use repeated runs, report min/median/max,
  and interleave A/B runs so thermal drift does not favour the second build.

---

## 5. Benchmark harness defects found (they hide the problem)

1. **1 ms clock.** `time.now()` returns whole milliseconds
   (`10_string_processing` etc. aside). Test 02/03 time a batch of five
   collections to get above the resolution, but `BUILD_MS` is a **single
   1 ms-resolution sample** in 02 and 03, and `TRAVERSE_MS` is 6–30 ms with
   1 ms granularity. A 3 % improvement is invisible.
2. **Build measured once.** Build is not repeated or medianed; the reported
   build number is one sample taken immediately after warmup, when the CPU is
   coldest. This is why build medians in this investigation (291–757 ms) look
   nothing like the committed 558 ms.
3. **No cold/warm heap distinction.** Each round of test 01/03 allocates into a
   freshly collected heap, so the cost includes first-touch page faults for the
   whole object graph every round. That is legitimate, but it is never
   identified, so it is easy to misattribute to the allocator's CPU path (§AO).
4. **Collection is a batch of five, not one.** Reported `COLLECT_MS` is
   `batch/5`; the first collection after a build includes TLAB publication of
   every object created since the last cycle (`begin_cycle`), which in test 03
   is 11.4 ms of the ~46 ms median. A "collection" therefore includes a
   construction cost.
5. **The C/C++/Rust comparison is not metadata-equal.** `mini_gc.h` gives those
   tracks a 32-byte header (`next_all`, `next_free`, `trace`, `object_bytes`,
   `marked`) for a 40-byte node; Nirqoba's node is 120 bytes. The C sweep
   visits only live objects (an intrusive all-objects list), while Nirqoba's
   current sweep scans the page bitmap. The comparison is "same algorithm
   shape", not "same memory layout".
6. **Java has a different memory regime.** The JVM commits its heap up front and
   allocates from per-thread TLABs with a ~10-instruction fast path. Java's
   2.57 ns/object in test 01 is not a fair baseline for an allocator that must
   obtain fresh pages from the OS each round; it is a fair baseline for what a
   mature runtime achieves once memory is committed.
7. **Missing workloads.** No minor-collection workload under mutation, no
   fragmentation/reuse workload, no multithreaded allocation/collection, no
   large-object workload, no string/container workload. Several ColO claims
   (`1646b93`, `1ca3c37`) cannot be exercised by tests 01–03 at all.

These defects do not invalidate the regression finding (a 2.3–3.9× change in
sweep is far above the noise), but they do invalidate small-percentage claims
and they explain why earlier reports disagreed with each other.

---

## 6. Uncommitted working-tree changes

At the start of this investigation the working tree already contained an
uncommitted prototype that was **not** part of `904aee0`:

| File | Lines | Content |
|---|---|---|
| `compiler/lib/Lowering/TirToMlir.cpp` | +63 | `fuse_handoff_releases()`: folds `nrq_gc_unpin(x)` + `nrq_gc_root_store(slot, x)` into `nrq_gc_root_store_release` |
| `runtime/include/nirqoba/runtime/gc.h` | +5 | declares `nrq_gc_root_store_release` |
| `runtime/src/gc.cpp` | +16 | `nrq_gc_root_store_release` = `root_store` + `unpin_value` |

This prototype was **left untouched, not reverted, and not built**. The compiler
binary used for all IR/assembly evidence in this report predates it, so no
emitted IR shown here contains the fused call. Its expected value is analysed in
`02-builder-bo.md` §6 and is small (<2 % of build, 0 % of collection). It is
recorded in the audit as *implemented but unverified, unbuilt*.

No production source was modified by this investigation. All instrumentation
lives in copies under `benchmarks/bottleneck_discovery/`.

---

## 7. Repository and version facts used as anchors

| Fact | Value | Source |
|---|---|---|
| Working version | 0.266-beta | `CMakeLists.txt:5,10` |
| Compiler binary | `build/msvc-dev/bin/nirqoba.exe`, built 11:14 | file timestamp + `--version` |
| LLVM/MLIR | 23.1.4 | `toolchain/toolchain.json` |
| Runtime configurations | `nirqoba_runtime`, `nirqoba_runtime_debug` | `runtime/CMakeLists.txt` |
| Production macro | `NRQ_RUNTIME_DEBUG_ENABLED=0` (PUBLIC) | `runtime/CMakeLists.txt` |
| Benchmark compile | `nirqoba build … -O3` (no `--debug-enabled`) | `benchmarks/stress_test_2/run_benchmark.py:270-276` |
| Linker | `lld-link.exe`, **no `/LTCG`** | `toolchain/toolchain.json`, `compiler/lib/Driver/CompilationPipeline.cpp:405-441` |
| Whole-program boundary | Nirqoba modules only; runtime is a prebuilt static lib | `compiler/lib/Backend/Optimization.cpp:196-200` comment |

The last row is the single most consequential architectural fact outside
AO/BO/ColO and is treated in `05-hidden-bottlenecks.md` §1.
