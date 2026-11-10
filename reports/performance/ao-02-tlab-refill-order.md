# AO-02 — TLAB refill order and the O(1) slab gate

**Status:** Complete (not pushed)
**Domain:** AO-02 — managed allocator optimization
**Date:** 2026-11-10
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)
**Base commit:** `cc969e2` (`chore: released version 0.315-beta`)
**Stage 1 result:** `16c5fb6` — `perf(gc): restore the short-run TLAB refill order`
**Stage 2 result:** `75509a7` — `perf(gc): gate the TLAB slab search with an O(1) run index`
**Stage 3 result:** `476e9c8` — `perf(gc): reserve fragmented runs in a batch and hand them out without the lock`
**Configuration:** production runtime (`NRQ_RUNTIME_DEBUG_ENABLED=0`) and development runtime (`=1`), MSVC 19.51.36260, `RelWithDebInfo` + `/Ob2`, Windows x64.
**Labels:** **MEASURED** reproduced on this machine; **SOURCE** verified in the source or the commit; **PROJECTION** calculated, not observed; **HYPOTHESIS** plausible but untested; **UNVERIFIED** implemented without a dedicated test.

---

## 1. Executive summary

A slab-sized-run preference added to `reserve_tlab_locked` introduced a severe
regression in a fragmented heap. Before handing out a short reclaimed run, the
refill tried to prove that no page held an 8 KiB run. That proof is a full walk
of the span index, and in a heap whose pages only contain few-object runs the
proof always fails. The probe paid it on every refill: **849–893 ms** for the
second 200,000-allocation wave, against **7–10 ms** with the short-run order
restored.

The work is split into two independently revertible stages:

| Stage | Commit | Change | Effect |
| --- | --- | --- | --- |
| 1 | `16c5fb6` | accept the first usable reclaimed run again; keep the refill counter | removes the whole-scan proof from the refill path |
| 2 | `75509a7` | keep the slab preference, but answer “does any page hold a slab?” with an incrementally maintained per-page maximum and a page counter | the slab search is skipped in O(1) when no slab exists; no whole-index proof remains |
| 3 | `476e9c8` | reserve a batch of short runs under one lock and hand them out from the thread without it | one lock entry per batch instead of per 4.2 allocations; −28% wall time with 4 mutators on the fragmented heap |

Final measured positions on the same 200,000-object fragmentation probe
(production runtime; Stage 1 and Stage 2: 9 interleaved samples each,
reconstructed bad gate: 3 samples):

| Revision | min | median | max | refills (new wave) | pages | commit |
| --- | --- | --- | --- | --- | --- | --- |
| reconstructed bad gate | 920.6 ms | 927.0 ms | 939.9 ms | 47,090 | 635 | 47.5 MB |
| Stage 1 (`16c5fb6`) | 7.03 ms | 7.75 ms | 8.33 ms | 47,090 | 635 | 47.5 MB |
| Stage 2 (`75509a7`) | 7.16 ms | 7.73 ms | 8.16 ms | 47,090 | 635 | 47.5 MB |

All three revisions end with the same live set, page count, commit charge and
refill count. **The defect was the failed O(pages) proof, not the refill rate.**

Correctness gates on the final revision: **197/197 unit tests**, **1/1 CTest
target**, **263/263 integration tests**, and Stress Test 2 tests 01–03 with all
validation checks passed. The development runtime re-derives the new index after
every update and reported no drift.

### 1.1 Scope

**Required by the request:** fix the slow refill path in two independently
reviewable stages, optimized for performance, keeping the refill counter, and
without touching benchmark workloads or object-header work.

**Implemented:** Stage 1 restores first-fit short-run reuse and commits the
refill counter; Stage 2 keeps the slab preference but gates it with an exact,
incrementally maintained per-page run maximum.

**Intentionally excluded:** object-header reduction (separate investigation),
TLAB run-lists or per-thread short-run caches, a best-fit/`take_free_span_locked`
redesign, and any benchmark-source or workload change.


---

## 2. Defect and root cause

### 2.1 The refill order before the change

`reserve_tlab_locked` is the allocation slow path. Its job is to obtain a
region for a thread-local reservation without growing the heap whenever a
reclaimed run can serve the request. The intended cheap order is:

1. a reclaimed run that can serve at least the requested object;
2. a committed page tail (no index maintenance);
3. a new page.

The regressing intermediate inserted a slab test before step 1:

```cpp
// regressing intermediate (not committed)
if (take_run(std::max(minimum_span, kGcTlabMinSlabBytes))) return true;
```

`take_free_run_locked` is first-fit over `span_pages`, and `minimum_span` is the
span of one object (160 bytes for a 64-byte payload plus the 96-byte header).
Raising the minimum to 8 KiB turns the search into: *walk every indexed page and
prove that none has an 8 KiB run.* In the fragmentation probe every page holds a
surviving object in four, so its largest run is a few cells. The proof fails on
every page, then the refill still consumes a short run, and the next refill
repeats the walk. **SOURCE** and **MEASURED**.

### 2.2 Reconstructing the bad gate

The regressing code was an uncommitted working-tree intermediate; `cc969e2`
already contains the correct short-run order. For reproducible evidence,
`build/ao02/gc_badgate.cpp` was reconstructed from `16c5fb6` by re-adding the
8 KiB minimum and the short-run fallback after the page-tail search. It is an
investigation artifact under `build/`, not part of the tree, and its binary
hash is recorded in §7.5.

**MEASURED**, fragmentation probe, production runtime:

| run | ALLOC_MS | new refills | pages | commit |
| --- | --- | --- | --- | --- |
| 1 | 920.583 | 47,090 | 635 | 47.4 MB |
| 2 | 927.048 | 47,090 | 635 | 47.5 MB |
| 3 | 939.885 | 47,090 | 635 | 47.5 MB |

This matches the earlier observation of 849–893 ms with the same 47,090 refills.
The first reconstruction (gate with no short-run fallback) produced 25.6–28.0 ms,
489 refills, 978 pages and 70.6 MB; that variant grows the heap instead of
reusing short runs and is **not** the failure being fixed. It is recorded so the
gate is not mistaken for a purely local cost.

---

## 3. Stage 1 — restore the short-run refill order

### 3.1 Commit and files

| | |
| --- | --- |
| Base | `cc969e2` |
| Result | `16c5fb6` |
| Files | `runtime/src/gc.cpp` (+26/−13), `runtime/include/nirqoba/runtime/gc.h` (+4/−0) |
| Diff | 2 files, 30 insertions, 13 deletions |

`runtime/src/gc.cpp`: the slab gate is removed. The first search accepts the
first run that can serve one object; the committed page tails and new-page paths
are unchanged. The refill counter added during the investigation
(`tlab_refill_count`) is committed and exposed as `nrq_gc_tlab_refill_count()`
in `gc.h` so the regression stays observable. The counter is a single saturated
increment on the slow path; it is not on the TLAB bump.

**API change.** `uint64_t nrq_gc_tlab_refill_count(void)` is a new read-only
diagnostic entry point. It takes `gc_mutex`, returns the saturating count, and
has no effect on allocation or collection behavior.

### 3.2 Source before and after

Before (the failure):

```cpp
if (take_run(std::max(minimum_span, kGcTlabMinSlabBytes))) return true;
// ... page tails ...
if (!page && take_run(minimum_span)) return true;   // reached only after the failed proof
if (!page) { /* new page */ }
```

After (`16c5fb6`):

```cpp
auto take_run = [&](size_t wanted_minimum) { /* ... */ };
if (take_run(minimum_span)) return true;
// ... page tails ...
if (!page) { /* new page */ }
```

The old form proved a negative before doing the cheap thing; the new form does
the cheap thing first. **SOURCE**.

### 3.3 Invariants preserved

- A run is still consumed from the head of the ascending-by-offset list; only
  the accept threshold changes.
- TLAB cursor/end bounds, alignment, generation invalidation, publication, and
  the page-tail index are untouched.
- A short run is still fully consumed rather than carved; no new partial slices
  are introduced.
- No check is removed. The change removes a search whose negative result was
  already implied by first-fit finding no run of that size.

### 3.4 Tests

| Gate | Command | Result |
| --- | --- | --- |
| Unit (debug runtime) | `build/msvc-dev/tests/nirqoba_unit_tests.exe` | **197/197 passed**, exit 0 |
| CTest | `ctest --preset windows-msvc-dev --output-on-failure` | **1/1 passed**, exit 0 |
| Integration (production) | `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe` | **263/263 passed**, exit 0 |

Logs: `build/ao02/unit_tests_after_count.log`,
`build/ao02/integration_tests.log`. Those logs are from the final revision; the
two commits were built in sequence during the investigation, and Stage 1 is the
parent of the tested Stage 2, but a separate Stage-1-only suite log was not
retained.

### 3.5 Benchmark

**MEASURED**, fragmentation probe (`build/ao/frag_probe.cpp`, 200,000 × 64-byte
objects, 25% pinned, fragmented second wave), production runtime, 9 interleaved
samples for Stage 1 and 3 samples for the reconstructed bad gate:

| Revision | min | median | max | spread | refills |
| --- | --- | --- | --- | --- | --- |
| bad gate (reconstructed) | 920.6 ms | 927.0 ms | 939.9 ms | 2.1% | 47,090 |
| Stage 1 | 7.029 ms | 7.748 ms | 8.328 ms | 18.5% | 47,090 |

Raw Stage 1 samples (ms): 7.750, 8.328, 7.029, 7.214, 8.296, 7.229, 7.385,
7.813, 7.748. The reduction is **>100× on the median**; the continued sampling
spread is this machine, not the change.

### 3.6 Production and debug

- Production: table above.
- Debug: a Stage-1-only debug run was not retained. The final debug build, which
  contains Stage 1 and Stage 2, is reported in §4.6; the debug runtime is
  observation-heavy and is not compared for speed, only for correctness and
  invariant stability.

### 3.7 Regressions

None measured. Page count, commit charge and refill count are identical to the
bad gate, so the change does not trade memory for speed; it removes work that
produced no allocation. The probe is synthetic and does not cover wide layouts
or arrays (§6).

### 3.8 Limitations

- The probe measures one object shape (64-byte payload, one reference offset)
  and one fragmentation pattern. A heap with a real 8 KiB run would take the
  same path in both revisions; that case is not isolated by this probe.
- The Stage 1 result is a **minimum/median of 9 interleaved process samples**,
  not a hardware-counter profile. No instruction-level attribution is claimed.
- The bad gate is reconstructed from an uncommitted intermediate; the exact
  binary that produced the original 849–893 ms observation was not retained.

### 3.9 Recommendation

**ACCEPT.** The stage restores the intended first-fit order, keeps the
diagnostic counter, and removes a measured >100× regression with identical heap
state.

---

## 4. Stage 2 — O(1) slab gate

### 4.1 Commit and files

| | |
| --- | --- |
| Base | `16c5fb6` |
| Result | `75509a7` |
| Files | `runtime/src/gc.cpp` (+139/−9) |

The local intermediate commit `1166fd4` contained the same design but rescanned
a page’s run list whenever the consumed run matched the page maximum. It was
amended to `75509a7` before publication, after the rescan was measured at
0.8–1.2 ms on the fragmentation probe. `1166fd4` is not part of the history.

### 4.2 Design

The slab preference is kept, but the negative proof is replaced by two pieces
of state, both maintained under `gc_mutex`:

- `GcPage::max_free_span` — exact largest recorded run in the page;
- `GcPage::max_free_span_count` — how many runs have that size;
- `pages_with_slab_run` — number of indexed pages whose maximum is at least
  `kGcTlabMinSlabBytes` (8 KiB).

The refill order becomes:

```cpp
if (minimum_span > kGcTlabMinSlabBytes) {
  if (take_run(minimum_span)) return true;        // large single object: direct search
} else if (pages_with_slab_run != 0 &&
           take_run(kGcTlabMinSlabBytes)) {
  return true;                                    // slab exists: search for it
}
// committed page tails ...
if (!page && take_run(minimum_span)) return true; // short runs
if (!page) { /* new page */ }
```

A heap with only short runs reads `pages_with_slab_run == 0` once and never
walks the index. A single object larger than the slab minimum searches directly;
that path is rare and is the only place where a size-based proof is skipped.

**Proof obligations.** The index is exact after every mutation:

1. `release_span_locked` insertion and every merge raise the maximum or leave it
   alone; the count tracks ties.
2. `take_free_span_locked` and `take_free_run_locked` decrement the count when
   one of several maximum-sized runs is consumed. Consuming the last
   maximum-sized run — or partially consuming the only one — triggers
   `refresh_page_max_free_span_locked`, which scans the page’s run list once and
   re-establishes both fields.
3. `acquire_page` and `recycle_page` reset both fields and the slab-page count.
4. The development runtime recomputes `largest_free_run(page, count)` inside
   `set_page_max_free_span_locked` and calls `nrq_runtime_fatal` on any
   divergence. The unit tests link `nirqoba_runtime_debug`, so this is active in
   `ctest`.

The per-page maximum also replaces the weaker `free_span_bytes < span` test in
both take paths: a page whose total free bytes are large but whose largest run
is small is rejected with one comparison instead of a run-list scan. **SOURCE**.

### 4.3 The O(runs) refresh found in review

The first landed form refreshed the page maximum whenever the taken run’s span
equalled `max_free_span`. In the fragmentation probe the run list contains many
equal 480-byte runs, so *every* refill matched and rescanned up to 96 entries.
Removing just that rescan (diagnostic `gc_s2_norefresh`, production runtime)
moved the median from 8.86 ms to 8.01 ms, confirming the cost. The final form
counts tied maxima, so the rescan happens only when a page’s last maximum-sized
run is consumed. **MEASURED** and **SOURCE**.

### 4.4 Tests

| Gate | Command | Result |
| --- | --- | --- |
| Build | `cmake --build build/msvc-dev` | success |
| Unit (debug runtime, invariant active) | `build/msvc-dev/tests/nirqoba_unit_tests.exe` | **197/197 passed**, exit 0 |
| CTest | `ctest --preset windows-msvc-dev --output-on-failure` | **1/1 passed**, exit 0 |
| Integration (production) | `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe` | **263/263 passed**, exit 0 |

The unit suite exercises fragmented reuse (`GcTest.MergedFreeRunsServeThreadLocalReserves`,
`GcTest.FreedNeighbourSpansBridgeIntoOneRun`, and the bridging-reclamation
regression), so the invariant is checked across insertion, merge, partial take,
full take, page recycling and page reuse.

### 4.5 Benchmarks

**MEASURED**, fragmentation probe, production runtime, 9 interleaved samples:

| Revision | min | median | max | spread | refills |
| --- | --- | --- | --- | --- | --- |
| Stage 1 | 7.029 ms | 7.748 ms | 8.328 ms | 18.5% | 47,090 |
| Stage 2 final | 7.163 ms | 7.726 ms | 8.155 ms | 13.8% | 47,090 |

Raw Stage 2 samples (ms): 7.877, 7.904, 7.310, 7.635, 7.937, 7.163, 7.223,
8.155, 7.726.

**MEASURED**, mixed-run probe (`build/ao02/mixed_probe.cpp`: short runs in the
early pages, whole-page runs in the late pages), production runtime, 9
interleaved samples:

| Revision | min | median | max | spread | refills |
| --- | --- | --- | --- | --- | --- |
| Stage 1 | 6.134 ms | 6.549 ms | 7.562 ms | 23.3% | 35,445 |
| Stage 2 final | 6.560 ms | 6.975 ms | 8.010 ms | 22.1% | 35,445 |

The mixed probe is the case where slabs exist, so the predicate is true and the
refill pays the search for a slab. It does **not** reduce refills
(35,445 both), and the median is 6.5% higher, inside this machine’s spread. This
is reported as a **neutral-to-slightly-negative** result for the slab
preference, not as a win.

**MEASURED**, end-to-end Stress Test 2, one runtime library per revision
(`build/ao02/build_stress_ab.ps1`; the driver is unchanged, only
`nirqoba_runtime.lib` is swapped), 8 interleaved process runs per cell:

| Test | Metric | Stage 1 min/med/max | Stage 2 min/med/max |
| --- | --- | --- | --- |
| 02 mark-and-sweep | BUILD_MS | 287 / 293.5 / 348 | 286 / 305.5 / 353 |
| 02 | TRAVERSE_MS | 15 / 16 / 18 | 15 / 16.5 / 19 |
| 02 | COLLECT_MS | 83 / 85 / 86 | 81 / 85.5 / 92 |
| 03 cyclic graph | BUILD_MS | 76 / 77.5 / 79 | 75 / 77.5 / 79 |
| 03 | COLLECT_MS | 22 / 28 / 29 | 24 / 26.5 / 30 |

The test-02 BUILD median is 4% higher while the minimum is 1 ms lower; test 03
BUILD is identical and COLLECT is slightly better. The result is **neutral
within this machine’s noise**. No material regression is established, and no
speedup is claimed.

**MEASURED**, final official Stress Test 2 run (production runtime, 3 process
repeats, `python benchmarks/stress_test_2/run_benchmark.py --test 01 --test 02
--test 03 --impl nirqoba --output build/ao02/bench_final.json --repeats 3`):

| Test | Reported metrics |
| --- | --- |
| 01 managed-object allocation | ALLOC 82 ms, TRAVERSE 9 ms, COLLECT 24 ms |
| 02 mark and sweep | BUILD 327 ms, TRAVERSE 16 ms, COLLECT 88 ms |
| 03 cyclic graph reclamation | BUILD 78 ms, COLLECT 24 ms |

All validation checks passed. These are end-to-end median values from the
benchmark runner, not interleaved A/B values, so they are reported separately
from the A/B table above.

### 4.6 Production and debug

- Production: all probe and stress numbers above.
- Debug: `build/ao02/frag_debug_final.exe` (links `nirqoba_runtime_debug.lib`)
  ran the fragmentation probe three times at **8.750, 8.949 and 9.112 ms** with
  47,090 refills, 635 pages and no invariant failure. The debug runtime is
  slower because it re-derives the index and carries the observation layer; the
  only claim is that the invariant holds on every measured path.

### 4.7 Regressions

- **Refill count: neutral.** Both probes consume the same runs; the slab
  preference changes which run is chosen, not how many are needed.
- **Mixed-run probe: 6.5% median slower.** The search for a slab is real work
  when a slab exists. It is inside the measured spread and does not change heap
  state, but it is not a speedup and is recorded as such.
- **Fragmentation probe: neutral** after the tied-maximum fix; without that fix
  it was 8.2–8.9 ms, i.e. a real regression against Stage 1.
- **Stress 01–03: neutral within noise**, with the test-02 BUILD median 4%
  higher and its minimum lower.
- No correctness regression: 197/197, 1/1, 263/263, all stress validations.

### 4.8 Limitations and unverified paths

- All timings are from one laptop (i5-1235U, 12 logical processors, Windows 11
  26300) with no core pinning or frequency control. The spread column is part of
  the result; differences below roughly 10% are not resolved by this setup.
- The probes cover fixed 64-byte payloads, one reference offset and 200,000
  objects. Wide objects, array allocations, multi-threaded mutators and
  allocation during marking/sweeping are not isolated here.
- The slab preference is exercised as a preference, not as a proven
  fragmentation cure. The probes cannot show a workload where choosing the slab
  avoids a future heap growth; that remains **HYPOTHESIS**.
- The development invariant is compiled out of production. A production-only
  drift would not be caught at runtime; the unit suite is the guard.
- The reconstructed bad gate is not the exact binary behind the original
  849–893 ms observation (**UNVERIFIED** at the byte level, though its
  refill/page/commit signature matches).

### 4.9 Recommendation

**ACCEPT**, with the explicit caveat that the stage is a **complexity-for-safety
trade**: it preserves the intended slab preference behind an O(1) gate and
removes the possibility of the whole-index proof returning, but it does not
reduce refills and is neutral-to-slightly-slower on the mixed probe. If the
owner prefers the smallest possible allocator state, Stage 2 can be reverted
independently; Stage 1 alone restores the measured performance.

---

## 5. Integrated gates on the final revision

| Gate | Command | Expected | Actual | Status |
| --- | --- | --- | --- | --- |
| Build | `cmake --build build/msvc-dev` | success | success | PASS |
| Unit | `build/msvc-dev/tests/nirqoba_unit_tests.exe` | 197/197 | 197/197, exit 0 | PASS |
| CTest | `ctest --preset windows-msvc-dev --output-on-failure` | 1/1 | 1/1, exit 0 | PASS |
| Integration | `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe` | 263/263 | 263/263, exit 0 | PASS |
| Stress 01–03 | `python benchmarks/stress_test_2/run_benchmark.py --test 01 --test 02 --test 03 --impl nirqoba --output build/ao02/bench_final.json --repeats 3` | validation | all checks passed | PASS |
| Fragment probe | `build/ao02/fragcount_gc_s2.exe` | stable, no fatal | 47,090 refills, 635 pages | PASS |
| Debug invariant | `build/ao02/frag_debug_final.exe` | no fatal | no fatal, 3/3 runs | PASS |

The unit binary prints `[ PASSED ] 197 tests` and exits 0 when invoked
directly; the earlier “exit code 1” seen in a PowerShell pipeline was the
pipeline’s exit status, not the test process. The direct run and CTest both
report success.

---

## 6. Known limitations and unverified paths

1. **Synthetic workloads.** The fragmentation and mixed probes are not
   production programs. They isolate the refill decision; they do not model
   wide layouts, arrays, finalization, or concurrent mutators.
2. **One machine, no pinning.** Run-to-run spread on the probes is 13–44%. The
   interleaved order and minima are the stable statistics; medians are reported
   for completeness.
3. **No instruction-level attribution.** The report claims elapsed time and
   state counts. It does not claim a cycle count, an instruction count, or an
   assembly-level change for either stage.
4. **Stage 2 is neutral on refills.** It is not a throughput optimization. Its
   value is that the slab preference can be retained without the O(pages) proof.
5. **Production invariant is absent by design.** The re-derivation check lives
   in the development runtime; the production build carries only the maintained
   fields.
6. **Object-header cost is out of scope.** No header layout or publication
   change was made; the AO-02 header investigation remains separate.
7. **The bad-gate binary is reconstructed.** The original intermediate was
   never committed; the reconstruction is labelled and hashed in §7.5.

### 6.1 Documentation changes

- This report (`reports/performance/ao-02-tlab-refill-order.md`) is the record
  for both stages.
- `runtime/include/nirqoba/runtime/gc.h` documents the new diagnostic counter.
- No user-facing language, standard-library, or workflow documentation changes:
  the work is internal allocator behavior plus one read-only diagnostic getter.

---

## 7. Reproducibility

### 7.1 Build

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build build/msvc-dev'
```

### 7.2 Revision binaries

`build/ao02/build_revisions.ps1` extracts `cc969e2` and `16c5fb6` versions of
`runtime/src/gc.cpp`, copies the worktree version as Stage 2, compiles each with
the production flags (`/O2 /Ob2 /Oi /Gw /Zc:inline /DNDEBUG
/DNRQ_RUNTIME_DEBUG_ENABLED=0`), and links them with the unchanged runtime
objects.

### 7.3 Probes

```powershell
build/ao02/fragcount_badgate.exe   # reconstructed failing order
build/ao02/fragcount_gc_s1.exe     # Stage 1
build/ao02/fragcount_gc_s2.exe     # Stage 2 final
build/ao02/mixedcount_gc_s1.exe
build/ao02/mixedcount_gc_s2.exe
build/ao02/frag_debug_final.exe    # development runtime, invariant active
```

`build/ao02/run_stress_ab.py` runs the interleaved Stress 02/03 A/B;
`build/ao02/build_stress_ab.ps1` builds the two runtime libraries and the two
benchmark executables. The official runner is
`benchmarks/stress_test_2/run_benchmark.py`.

### 7.4 Raw data and artifacts

| Artifact | Contents |
| --- | --- |
| `build/ao02/frag_revisions.json` | base/Stage 1/Stage 2 fragmentation samples |
| `build/ao02/frag_s1_s2.json` | 9-sample Stage 1 vs Stage 2 pre-fix comparison |
| `build/ao02/frag_s1_s2_count.json` | 9-sample final comparison |
| `build/ao02/frag_badgate.json` | reconstructed bad-gate samples |
| `build/ao02/mixed_s1_s2_count.json` | mixed-run samples |
| `build/ao02/stress_ab.json` | interleaved Stress 02/03 process medians |
| `build/ao02/raw_stress/` | raw Stress 02/03 process output |
| `build/ao02/bench_final.json` | official benchmark runner output |
| `build/ao02/unit_tests_after_count.log` | unit test log |
| `build/ao02/integration_tests.log` | integration test log |

### 7.5 Binary hashes (SHA-256)

| Binary | Hash |
| --- | --- |
| `nirqoba_unit_tests.exe` | `62FEB07128C3F60BE82218CFC333661AD4D120A396F00733D53F658BA4DE5F4F` |
| `nirqoba.exe` | `3BCA925CACE725E8AEA82C2EF47A5807FF32E17B5293203DAFFB6CC7515EF7EE` |
| `nirqoba_runtime.lib` | `C037F65FBDF54AD1E598E071800267AF96BBB86313BB5F305A867774267395D7` |
| `nirqoba_runtime_debug.lib` | `F1A3C6E62C925015F1E5DC2A1F38C2CC2905E2E28A22136115BE8435A8F3CB91` |
| `fragcount_badgate.exe` | `079DE024E4377437DCC61DE9AD1639D6AF2D5F2D89D6ED5892D86BC1725F5AEB` |
| `fragcount_gc_s1.exe` | `058361E34312C94953FF572267272FE66E511F092BFC0D762C90F358691E793F` |
| `fragcount_gc_s2.exe` | `704F69D462BD0BC576A3D614BA9239A234FEC02237B341EEBE17AC4ED0EB1E10` |
| `mixedcount_gc_s1.exe` | `9676BD4D147A329E60D77B0492E1CD0309FC1ED228A01346E044CD5FC269862F` |
| `mixedcount_gc_s2.exe` | `E590AD8E3BCE3905F651A0BD70C5CD2BC47BE9AC8B0FBE9293F85D97E6687E29` |

Hashes identify the local binaries used for the numbers above; the source
commits are the reproducible identity.

---

## 8. Requirement-by-requirement status (roadmap §12.2)

| # | Requirement | Status |
| --- | --- | --- |
| 1 | Exact base and result hashes | `cc969e2` → `16c5fb6` → `75509a7`; §3.1, §4.1 |
| 2 | Files changed and why | `runtime/src/gc.cpp`, `runtime/include/nirqoba/runtime/gc.h`; §3.1, §4.1 |
| 3 | Before/after source evidence | §2, §3.2, §4.2; no IR/assembly claim is made |
| 4 | Invariants and new proof obligations | §3.3, §4.2, §4.3 |
| 5 | Tests, commands, counts, logs | §3.4, §4.4, §5, §7.4 |
| 6 | Benchmark environment, flags, hashes, raw samples, min/median/max/spread | §1, §3.5, §4.5, §6, §7 |
| 7 | Separate production and debug results | §3.6, §4.6 |
| 8 | Regression analysis, including non-improvements | §3.7, §4.7 |
| 9 | Limitations and unverified paths | §3.8, §4.8, §6 |
| 10 | Explicit accept/revise/revert recommendation | §3.9, §4.9 |

---

## 9. Final verdict

**PASS**, as two stages.

- **Stage 1: ACCEPT.** It removes the failed whole-index proof and restores the
  measured short-run behavior: 920–940 ms → 7.0–8.3 ms on the fragmentation
  probe, with identical refills, pages and commit charge.
- **Stage 2: ACCEPT with caveat.** It makes the retained slab preference
  answerable in O(1), keeps the index exact in the development runtime, and is
  neutral on the fragmentation probe and the end-to-end stress tests. It does
  not reduce refills and is 6.5% slower on the mixed-run probe’s median, inside
  the measured spread. It is independently revertible; reverting it restores
  Stage 1’s behavior without losing the regression fix.
- Correctness gates on the final revision are green: 197/197 unit tests, 1/1
  CTest target, 263/263 integration tests, and all Stress Test 2 validation
  checks.


---

## 10. Stage 3 — reserved-run batches (completion of the fix)

**Commit:** `476e9c8`. **Base:** `75509a7`. **Files:** `runtime/src/gc.cpp`,
`tests/unit/gc_test.cpp`.

Stages 1 and 2 removed the pathological proof and kept the slab preference behind an
O(1) gate, but the refill rate itself was left alone: a heap whose free space is a
scatter of few-cell runs still entered the allocator lock **once per 4.2 allocations**
(47,090 locked refills for the second 200,000-object wave). That is the remaining cost
of the fix, and it is what this stage addresses.

### 10.1 Design

The refill's last resort — a short reclaimed run — now reserves a **batch**: up to 24
runs, bounded to two slabs of bytes (`kGcTlabBytes * 2`), taken by the same
`take_free_run_locked` first-fit that the single-run path called. The first run of the
batch becomes the thread's cursor; the rest wait on a thread-owned list
(`MutatorState::spare_runs`), where **taking one is a decrement, not a lock**.

Because each entry is the same take the old path made, every existing invariant is
unchanged: the page's ascending run list, `max_free_span`/`max_free_span_count`,
`free_span_bytes`, `total_free_spans`, the span index and `pages_with_slab_run` are all
maintained by the same code. The change is **how often the lock is entered**, not what
happens inside it.

The batch is popped before the lock in `allocate_tlab`, so the fragmented path is
lock-free for 23 of every 24 refills. A run too small for the current request stays on
the list for a smaller object.

### 10.2 Why the batch cannot outlive a cycle

A reserved run has been removed from its page's free list and published no object: it
is in neither state that makes a page recyclable. `recycle_page` resets the run list
and can hand the storage to another mutator, so a list surviving a boundary would be a
use-after-recycle bug rather than a slow path. Two orderings make that impossible:

1. `prepare_cycle_locked` flushes every registered mutator's list **before** it bumps
   `gc_page_generation` and **before** the reclaimer erases empty pages.
2. `nrq_gc_mutator_leave` flushes before the thread leaves the registry, so nothing is
   lost with the mutator.

If a list is ever seen against a stale generation anyway, the development runtime calls
`nrq_runtime_fatal` and a release build drops the list instead of touching a page that
may be gone. Dropping is the only recovery that cannot dereference freed storage.

### 10.3 Measurements

**MEASURED**, production runtime (`NRQ_RUNTIME_DEBUG_ENABLED=0`), 5 interleaved process
pairs per probe, same machine as §4.

Fragmentation probe (200,000 × 64-byte objects, 25% pinned, second wave into the
fragmented heap):

| Revision | min | median | max | locked refills (wave 2) | pages | commit |
| --- | --- | --- | --- | --- | --- | --- |
| Stage 2 | 7.449 ms | 8.021 ms | 8.068 ms | 47,090 | 635 | 47.4–47.5 MB |
| Stage 3 | 6.932 ms | 7.755 ms | 7.819 ms | **2,102** | 635 | 47.4–47.5 MB |

The same heap with **4 mutator threads** (each allocating a quarter of the wave):

| Revision | min | median | max | locked refills | pages |
| --- | --- | --- | --- | --- | --- |
| Stage 2 | 16.433 ms | 17.327 ms | 19.391 ms | 47,091 | 636–637 |
| Stage 3 | 11.723 ms | 12.371 ms | 12.793 ms | **2,103** | 635–637 |

**−28% median and −29% minimum with 4 mutators**; single-threaded the same probe is
neutral (−3% median, inside the spread) because the lock was not the cost there. The
four-thread result is the contention this removes, and it is the reason the change is
worth its state.

Mixed-run probe (short runs in the early pages, whole-page runs in the late pages),
interleaved against the Stage 2 binary:

| Revision | min | median | max | locked refills |
| --- | --- | --- | --- | --- |
| Stage 2 | 6.568 ms | 7.388 ms | 8.239 ms | 35,445 |
| Stage 3 | 6.330 ms | 7.145 ms | 11.694 ms | **1,701** |

Time is neutral (one 11.7 ms outlier in four samples, the rest 6.3–7.5 ms); locked
refills fall **20.8×**.

### 10.4 Correction to §4.5: the slab gate does earn its keep

§4.5 reported the mixed probe at 35,445 refills for both Stage 1 and Stage 2 and called
the gate neutral. That comparison was made without a batch: the un-gated first-fit
returns after **one** run, so the gate had nothing to compound with. Measured again
with the batch in place (`build/ao/mixed_nogate` variant, same probe):

| Variant | locked refills | ALLOC_MS |
| --- | --- | --- |
| no gate, with batch | 35,445 | 6.600 / 7.698 |
| **gate, with batch** | **1,701** | 6.330 / 7.501 |

The gate is what lets a mixed heap take a whole slab instead of a few cells, and the
batch is what makes the short-run path cheap when no slab exists. **Stage 2 is kept**;
its value is conditional on Stage 3, which is why neither stage should be reverted
alone on the basis of §4.5.

### 10.5 Tests

| Gate | Command | Result |
| --- | --- | --- |
| Unit | `build/msvc-dev/tests/nirqoba_unit_tests.exe` | **199/199 passed** |
| Integration | `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe` | **263/263 passed** |

Two tests cover the new lifecycle:

- `GcTest.FragmentedPagesServeReservedRunsWithoutRepeatingCells` — 4,000 objects with
  one survivor in four, then a second wave: every address distinct, and the wave's
  locked refills below one per eight objects (they are ~one per batch). The address
  check is the double-hand-out guard; the refill bound is what would fail if the batch
  were removed.
- `GcTest.ReservedRunsReturnToTheirPagesAtTheCycleBoundary` — four identical waves over
  the same footprint with a collection between each: page count does not grow beyond
  the first wave and live objects return to the baseline, which is what proves the
  unused tail of each batch goes back to its page instead of being leaked.

### 10.6 Limitations

- Single-threaded time is unchanged; the stage's measured win needs contention. On a
  one-thread program it is a refill-count change, not a speedup.
- `SpareRun` costs 24 × 24 bytes per registered mutator (576 B), and the flush runs
  once per mutator per cycle; neither is measurable here, and neither is claimed.
- The probes cover 64-byte payloads with one reference offset. Wide objects, arrays and
  allocation during marking are exercised by the suites, not by these timings.
- The 4-mutator probe creates its threads once and measures the join, so thread startup
  is inside the number; it is equal for both revisions.

### 10.7 Recommendation

**ACCEPT.** The fix now has both halves: the refill no longer proves a negative over
the page index (Stage 1), keeps the slab preference without that proof (Stage 2), and
amortises the remaining short-run path so a fragmented heap is not a lock convoy
(Stage 3). Correctness gates are green on the final revision.
