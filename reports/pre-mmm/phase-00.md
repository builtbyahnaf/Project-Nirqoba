# Pre-MMM Phase 0 — Establish the migration baseline

**Phase identifier:** Pre-MMM Phase 0
**Title:** Establish the migration baseline
**Implementation date:** 2026-11-11
**Status:** Complete except the multi-threaded workload, which is blocked by a missing concurrency API
**Objective:** Record a reproducible, verifiable baseline of the current GC and ownership paths before any ownership work begins.
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

---

## 1. Scope

**Required:** preserve the current GC and ownership paths as a working baseline; add a compact
benchmark suite covering small allocation, large allocation, allocation-heavy loops, array
traversal, aggregate construction/destruction, nested owned resources and multi-threaded
allocation; record flags, hardware, build mode, warmups, repetitions, checksums and
median/best timings; separate compiler startup and runtime initialisation from workload
timings.

**Implemented:** all of the above except multi-threaded allocation. There is no
thread-creation builtin in the compiler or runtime (`thread.spawn` and `nrq_thread_create`
do not exist), so that workload cannot be written in Nirqoba today. It is recorded as
unavailable rather than approximated with a single-threaded loop, which would be a fabricated
result.

**Intentionally excluded:** any change to compiler, runtime, allocator or collector code.
This phase is measurement only, so it introduces no syntax and no semantics.

---

## 2. Architecture and design

The suite lives in `benchmarks/pre_mmm_baseline/` and is deliberately independent of the
existing performance tracks (`reports/performance/*`), which measure individual optimisations
rather than the baseline the migration will be compared against.

Three decisions shape it:

1. **Timing inside the program.** Each program wraps only its workload in `time.cycles()`
   (`__rdtsc` deltas) and prints the cycle count after its checksum. Compiler start-up,
   runtime initialisation and process launch therefore cannot contaminate the measurement,
   which satisfies the roadmap's separation requirement directly rather than by subtracting
   a start-up average. The runner additionally records whole-process wall time, so the gap
   between the two is visible and the start-up row quantifies it.
2. **Checksums as the correctness gate.** Every workload adds a value derived from the data
   it touched and prints it. The runner refuses to report a configuration whose checksum
   drifts between repetitions; a benchmark that computes the wrong answer is not a
   benchmark.
3. **A guard against optimized-away workloads.** See §4; this guard exists because the first
   version of the suite silently measured empty loops.

---

## 3. Implementation details

| Program | Workload | What it exercises |
|---|---|---|
| `00_startup.nrqba` | empty measured region | start-up and timer overhead |
| `01_small_alloc.nrqba` | 200,000 × 64 B owned array | small-allocation fast path, per-iteration destruction |
| `02_large_alloc.nrqba` | 500 × 1 MiB owned array | large-block allocation, first/last element only |
| `03_alloc_loop.nrqba` | 20,000 × 256-int owned array, filled | allocation plus real per-element writes |
| `04_array_traversal.nrqba` | 1 M elements, fill + 5 passes + sum | sequential owned-array throughput |
| `05_aggregate_construct.nrqba` | 200,000 struct constructions | mixed scalar, nested struct and fixed-array aggregate |
| `06_nested_owned.nrqba` | 10,000 × three live owned arrays over nested scopes | LIFO destruction through nested scopes |

`run.py` builds each program once with the working compiler, runs one warmup and seven
measured repetitions, verifies the checksum, and writes `BASELINE.md` and `BASELINE.json`.
The JSON carries the compiler version string, build mode read from `CMakeCache.txt`, flags,
CPU, OS, warmup/repetition counts and every raw sample.

---

## 4. The elision problem, and why the allocation workloads changed shape

The first version of `01_small_alloc.nrqba` used `memory.allocate<int>(8)` in the loop, wrote
the loop counter, read it back and freed the allocation. It reported **22 cycles** for 500,000
allocate/free pairs, which is impossible.

Three checks agreed on the cause:

- `--emit=llvm` contained **zero** `nrq_memory_allocate` calls and the printed checksum was a
  literal `124999750000`.
- A variant that started the memory profiler and captured it afterwards reported
  `allocations == 0`, so the runtime never saw the calls either.
- Casting the pointer to `int` to make its address observable is **rejected**:
  `NRQ3015: cannot cast value of type 'int*' to 'int'`.

The store-to-load forwarding reduces `checksum += p[0]` to the loop counter, after which the
allocation result is dead and the allocator pair is removed. This is legal optimisation, but
it means a naive allocation microbenchmark measures an empty loop and reports it as a result.

The allocation workloads therefore use `memory.owned_array<T>(n)`, whose element count is
loaded from the runtime-created header rather than folded, and which the compiler keeps as
verified in `--emit=llvm`. The raw `memory.allocate` path remains unmeasurable at `-O2` with
the current observable surface; that is recorded here for Phase 4 rather than worked around
by lowering the optimisation level, which would change what is being measured.

The same elision hit the first `05_aggregate_construct.nrqba`, whose values came only from the
loop counter (47 cycles for 500,000 constructions). It now reads its field values through an
owned array, which keeps the construction in the generated code.

To stop this recurring silently, `run.py` now aborts when a workload measures under
0.05 cycles per unit, naming the workload and the likely cause.

---

## 5. Results

Environment: Windows x64, MSVC 14.51, LLVM/MLIR 23.1.4, `RelWithDebInfo` (`build/msvc-dev`),
default `-O2`, compiler `0.371-beta`, 1 warmup + 7 measured repetitions, time only inside the
workload. Full table with wall time and raw samples in
`benchmarks/pre_mmm_baseline/BASELINE.md`.

| Workload | Checksum | Median cycles | Best cycles | Median cyc/unit |
|---|---|---|---|---|
| startup | 0 | 28 | 25 | 28 per program |
| small alloc (64 B) | 59999700000 | 71,975,073 | 70,688,765 | 359.9 per alloc+free |
| large alloc (1 MiB) | 374250 | 12,980,752 | 12,128,839 | 25,961.5 per alloc+free |
| allocation loop (256 ints) | 51850240000 | 17,594,724 | 16,765,903 | 879.7 per owned array |
| array traversal | 112127563435580512 | 11,260,434 | 11,116,582 | 1.61 per element visit |
| aggregate construction | 40005900000 | 568,904 | 519,132 | 1.14 per aggregate |
| nested owned | 8024000000 | 9,902,724 | 8,655,086 | 990.3 per iteration |

Reading the baseline:

- **A 64-byte owned allocation plus release costs ≈360 cycles**, roughly 120 ns at the
  observed clock, while a 1 MiB pair costs ≈26,000 cycles. The large case is about 72× the
  small case's cost for 16,384× the bytes, so small allocations are dominated by fixed
  per-operation overhead, not by size.
- **Plain traversal is ≈1.6 cycles per int visit** including bounds checks, and struct
  construction is ≈1.1 cycles per aggregate. Stack aggregates are effectively free next to
  any heap operation, which is the appropriate comparison for Phase 1's field-ownership work.
- **Nested owned scopes cost ≈990 cycles per iteration** for three live 32-int arrays, i.e.
  the inner allocations and their releases dominate, not the scope bookkeeping.
- **Start-up is ≈24 ms of wall time against 28 measured cycles.** Any wall-clock-only
  benchmark of a short workload is measuring process launch; this is why the cycles are taken
  inside the program.

Run-to-run spread is visible in `BASELINE.json` (median versus best differs by up to ~20% on
the allocator workloads). The medians are used for the table; the spread is the reason no
sub-1% claim should be made from a single run.

---

## 6. Gaps confirmed while building the baseline

These are recorded because they define the work the following phases must do. None were
worked around.

1. **Owned arrays cannot be struct fields.** `struct Bucket { memory.owned_array<int> data; }`
   fails at parse time with `NRQ2002: expected field name in struct`. The parser's
   `parse_type_annotation` does not accept a qualified generic builtin as a field type, so
   `05` and `06` had to use fixed arrays and nested scopes instead. This is exactly Pre-MMM
   Phase 1's first item (ownership contracts for pointer/aggregate fields).
2. **An owned array cannot own allocations per element.** Documented in
   `docs/memory/management.md` and confirmed by the shapes the parser accepts; nested
   adaptation uses scopes rather than container-of-container.
3. **`memory.allocate` results are unobservable at `-O2`.** No pointer→int cast, no profiler
   visibility after elision. Phase 4 will need an observable allocation surface (or a
   documented lower-optimisation measurement mode) to measure the raw path.
4. **No thread creation API**, so the multi-threaded allocation workload is unavailable.

---

## 7. Modified files

| File | Role |
|---|---|
| `benchmarks/pre_mmm_baseline/00_startup.nrqba` | start-up baseline and timer overhead |
| `benchmarks/pre_mmm_baseline/01_small_alloc.nrqba` | small owned-allocation workload |
| `benchmarks/pre_mmm_baseline/02_large_alloc.nrqba` | large owned-allocation workload |
| `benchmarks/pre_mmm_baseline/03_alloc_loop.nrqba` | allocation-heavy loop with real writes |
| `benchmarks/pre_mmm_baseline/04_array_traversal.nrqba` | sequential traversal throughput |
| `benchmarks/pre_mmm_baseline/05_aggregate_construct.nrqba` | aggregate construct/destruct cost |
| `benchmarks/pre_mmm_baseline/06_nested_owned.nrqba` | nested-scope destruction cost |
| `benchmarks/pre_mmm_baseline/run.py` | build, run, checksum verification, elision guard, report generation |
| `benchmarks/pre_mmm_baseline/BASELINE.md` | generated human-readable baseline |
| `benchmarks/pre_mmm_baseline/BASELINE.json` | generated machine-readable baseline with raw samples |
| `project_context/Pre-MMM_ROADMAP.md` | Phase 0 items ticked with evidence; multi-threaded item marked blocked |
| `reports/pre-mmm/phase-00.md` | this report |

---

## 8. Language and API changes

None. No compiler, runtime, syntax or semantic change was made. `time.cycles()` already
existed from the preceding profiling-counter work and is used as-is.

---

## 9. Testing

| Check | Command | Result |
|---|---|---|
| All seven programs compile | `build\msvc-dev\bin\nirqoba.exe build benchmarks\pre_mmm_baseline\<file> -o ...` | PASS, 7/7 |
| Every workload produces a stable checksum over 7 repetitions | `python benchmarks\pre_mmm_baseline\run.py` | PASS, run aborts on drift |
| Elision guard rejects a folded workload | temporary counter-only variant of `05` | PASS — rejected at 0.00009 cycles per aggregate before the fix |
| Unit suite (unchanged code) | `build\msvc-dev\tests\nirqoba_unit_tests.exe` | PASS 210/210 |
| Integration suite (unchanged code) | `python tests\run_tests.py --compiler build\msvc-dev\bin\nirqoba.exe` | PASS 284/284 |

The suites were run after the `time.cycles()` implementation and were not re-run for this
phase because no compiler or runtime source changed; the benchmark programs exercise the
same compiler and all seven build and execute.

---

## 10. Performance

See §5. This phase establishes the baseline; it makes no comparison claim and no improvement
claim. The numbers above are the reference that Phase 4 (owned allocation fast path) and
Phase 6 (lowering) will be measured against, on the same machine and build mode.

Limitations: single machine, single build, no CPU pinning or frequency control, `__rdtsc` is a
reference counter rather than core cycles, and the allocator workloads show up to ~20%
median-to-best spread. Comparative claims require repeated runs on a controlled host.

---

## 11. Known issues

- Multi-threaded allocation is not measured; there is no thread API (blocked, not skipped).
- The raw `memory.allocate` path cannot be measured at `-O2` because dead allocation pairs are
  removed and the pointer cannot be made observable. Owned arrays are measured instead.
- Owned arrays cannot be struct fields, so the aggregate and nested-resource workloads use
  fixed-array fields and nested scopes.
- SoA/AoSoA representations are covered by `benchmarks/soa_stage4/`, not duplicated here.

---

## 12. Acceptance criteria

| Requirement | Status |
|---|---|
| Preserve GC and ownership paths as a working baseline | PASS — no code changed; both suites green |
| Small allocation and release benchmark | PASS — `01_small_alloc` |
| Large allocation and release benchmark | PASS — `02_large_alloc` |
| Allocation-heavy loop benchmark | PASS — `03_alloc_loop` |
| Array traversal benchmark | PASS — `04_array_traversal` |
| Aggregate construction and destruction benchmark | PASS — `05_aggregate_construct` |
| Nested owned resources benchmark | PASS — `06_nested_owned` |
| Multi-threaded allocation benchmark | BLOCKED — no thread-creation API |
| Record flags, hardware, build mode, warmups, repetitions, checksums, median/best | PASS — `BASELINE.md` / `BASELINE.json` |
| Separate compiler start-up and runtime initialisation from workload timing | PASS — in-program `time.cycles()` plus start-up row |
| Reproducible baseline exists | PASS — one command regenerates it |

**Verdict: BLOCKED** — six of seven workloads and every recording requirement are complete and
reproducible, but the multi-threaded allocation workload cannot be implemented because the
language has no concurrency runtime. Adding one is explicitly outside this roadmap's scope
("unrelated SIMD, threading, or language-feature work"), so the item stays open rather than
being simulated. Everything else in Phase 0 is satisfied, and Phase 1 work can proceed.
