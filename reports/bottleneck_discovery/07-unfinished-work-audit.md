# 07 — Unfinished Work Audit

**Domain:** Claims vs evidence for 0.257, 0.260, 0.266
**Verdict:** Several shipped changes are implemented but **never exercised by a
test or benchmark**, one documented change is absent from the code, one
benchmark artifact misrepresents its own version, and the 0.266 entry in
`docs/VERSION_LOG.md` is empty. None of this is hypothetical: each row below was
checked against the current tree.

Legend: **Implemented** = source present; **Tested** = a test exercises it;
**Measured** = a benchmark shows an effect; **Exercised** = a workload reaches
the path at all.

---

## 1. ColO-01 items (0.260-beta)

| Item | Implemented | Tested | Exercised by 01–03 | Measured effect | Status |
|---|---|---|---|---|---|
| Free-span list ordered on bridge merge (`f1d2b42`) | Yes (`gc.cpp:1768-1810`, tail shift) | Yes (`MergedFreeRunsServeThreadLocalReserves`, per 0.260 log) | No (02 frees nothing; 03 frees whole pages) | None on 02/03 | Correctness fix; performance **UNVERIFIED** |
| Page index for reusable runs (`1ca3c37`) | Yes (`span_pages`, `index_span_page_locked`, early exit `gc.cpp:755/1814`) | Not named in log | **No** | None on 02/03 | **UNVERIFIED** |
| Page-ordered sweep (`e830b65`) | Yes (`sweep_chunk_locked` bitmap walk) | Not a sweep-cursor unit test | Yes | **−62 ms/col in 02 (regression)** | **Defective** |
| Young-only minor page skip (`1646b93`) | Yes (`page->young_objects == 0` in sweep) | Minor semantics tested; the page-skip performance is not | **No** (major only) | None | **UNVERIFIED** |
| Byte-sized mark/sweep slices (`0708f75`) | Yes (`kMarkSliceBytes`, `kSweepSliceBytes`) | Not named | Yes | 34× fewer chunks, no time change | Implemented; responsiveness **UNVERIFIED** |

The release note for 0.260 says page-ordered sweep is a win because
"allocation no longer links every object into a heap-wide list". That is true,
and it is the reason not to revert the design. It omits that the replacement
scan examines 15 bitmap positions per object.

---

## 2. BO items (0.257-beta "Builder Path, Step One")

| Claim | Reality |
|---|---|
| Compiler emits `nrq_gc_roots_*` tables | Yes: `TirToMlir.cpp:2050-2093`, globals emitted |
| Lowering calls `nrq_gc_root_register_frame_static` | Yes (`TirToMlir.cpp:2082`) |
| Production trusts the table, development validates | Yes: `validate = nirq::runtime_debug_enabled` (`gc.cpp:3039-3042`) |
| Malformed table caught in development | Plausible; the validating path exists, but no test was found that feeds a malformed table |
| "Parity is tested: a unit test drives both entry points" | Test not located by name in this audit; **UNVERIFIED** |
| Seven integration cases inspect the emitted call | Integration expectations exist; **UNVERIFIED** as a count |

**Measured cost of the surviving protocol:** 63 ns per two-slot frame
(`02-builder-bo.md` §2). The static entry point removed validation but not the
per-slot `RootInfo` copy and the two hash-table inserts.

---

## 3. The unbuilt/undocumented builder fusion

`fa799e8` (the investigation commit) also contains a **working implementation**
of handoff fusion, not just a report:

* `compiler/lib/Lowering/TirToMlir.cpp:1536-1590` — `fuse_handoff_releases()`,
  called unconditionally from the function translator;
* `runtime/src/gc.cpp:3572` — `nrq_gc_root_store_release` = `root_store` +
  `unpin_value`;
* `runtime/include/nirqoba/runtime/gc.h` — declaration.

It is **compiled into the 0.269 build** and therefore changes the emitted
builder sequence relative to 0.257/0.260. It has no dedicated test, no
before/after benchmark, and the code comment's claim that it merges "two barrier
regions" is inaccurate (`unpin` does not open a barrier region). Expected effect:
~2–3 % of build (`02-builder-bo.md` §6).

This must be disclosed in the 0.269 comparison: **0.269's BUILD numbers include
a builder change that 0.257/0.260 do not have.** COLLECT comparisons are
unaffected.

---

## 4. Claims in earlier reports that are now wrong or stale

| Source | Claim | Correction |
|---|---|---|
| `reports/performance/ao-01-allocator-optimization.md` | Header "96 B padded to 128 B", 87.5 % metadata | Span is **120 B**, metadata 80 % (`04-object-header.md` §1) |
| `ao-01` | "No remaining allocator issues" | Reuse/fragmentation path never exercised; TLAB refill path unmeasured |
| `bo-01-builder-optimization.md` | `root_store` ≈ 50 cycles, handoff ≈ 104 cycles | Current runtime: 1.6 ns (~5.6 cycles) and 5.1 ns (~18 cycles); the barrier/pin paths were rewritten after BO-01 |
| `bo-01` | Validated-descriptor memo is the per-activation fix | Production never calls the validating entry point, so the memo is dead in production |
| `bo-02-builder-status.md` | Descriptor interning 80 B → 40 B per root | Not implemented; would need a stable descriptor arena |
| `GC_COST_INVESTIGATION.md` (if present) | TLAB publication per return | No longer true: transfer pins are buffered per mutator (32-entry) and folded at cycle start |
| `docs/VERSION_LOG.md` 0.260 | Implies the release was benchmarked | Its `benchmark_results.json` was generated 7 h **before** the ColO commits |
| `docs/VERSION_LOG.md` 0.266 | *(entry is empty)* | 0.266 shipped `5d62abf` + `904aee0` and the fusion, undocumented |

---

## 5. Missing tests (by path, not by intention)

| Path | Why it matters | Current coverage |
|---|---|---|
| Incremental sweep resume mid-word / mid-page | The bitmap cursor is the highest-risk state in the collector | No test drives `sweep_chunk_locked` with a budget that stops mid-word |
| Free-run bridge merge + reuse | `f1d2b42` fix | One unit test named in the log; not verified here |
| Mixed-size allocation/reclamation in one page | Alignment + coalescing | `BumpRegionsAlignMixedObjectSizesAndReclaimEmptyPages` exists; not run in this audit |
| Minor collection with old→young references | remembered-set correctness | Unit tests exist (`MinorCollectionPromotesSurvivorsAndRetainsOldGarbage`, `OldToYoungStoreBarrierKeepsYoungObjectAliveDuringMinorCollection`); no end-to-end workload measures cost |
| Promotion across ≥2 epochs | `age`/`birth_epoch` reset | Unit tests likely exist; no end-to-end workload |
| Multithreaded alloc + collect | park-before-scan, barriers, TLAB publication | Unit tests exist (`ConcurrentAllocationIsSerializedAndReclaimed`, `RootStoreSynchronizesConcurrentMutationAndCollection`, `CollectionMarksIncrementallyWhileMutatorsRun`, …); no committed throughput benchmark |
| Fragmentation steady state | span_pages growth, first-fit scan length | None |
| Malformed root descriptor in development | BO-02's stated safety property | Not verified |
| 96-byte header static assertion after any change | `static_assert` exists; no layout test beyond it | Partial |

---

## 6. Benchmark artifacts

| Artifact | Problem |
|---|---|
| `benchmarks/stress_test_2/benchmark_results.json` (committed at 0.260 and HEAD) | generated 03:13, before the 0.260 code; all `compilers: unknown` |
| Working-tree/`fa799e8` result | valid code/version pairing, still `compilers: unknown`, `memory_bytes: null` |
| `benchmark_results.json` has `valid: true` | it validates checksums, not provenance; a stale file passes |
| `benchmarks/stress_test_2/bin/*` committed | binaries without recorded build IDs; cannot be tied to a compiler |
| No fragmentation / minor / MT benchmark | see §5 |

**Minimum fix:** the runner should record `nirqoba --version`, `git rev-parse
HEAD`, `cl`/`rustc`/`java -version`, binary SHA-256, and peak working set, and
should refuse to write `valid: true` if any is missing.

---

## 7. Unfinished design work (not bugs, but unfinished)

1. **Header compression** — candidates compiled (`04-object-header.md`), none
   implemented. The `static_assert(96)` is the gate.
2. **Parallel marking** — no thread pool, no per-thread worklists. Largest
   architectural lever after ColO-01.
3. **Runtime memory-effect attributes** — inference exists for definitions only;
   external declarations remain `nounwind` (`05-hidden-bottlenecks.md` H2).
4. **Inlined allocation fast path** — blocked by the non-LTO runtime archive.
5. **Per-TLAB accounting** — exact per-object stats are deliberate; the cost is
   paid twice (allocation and `begin_cycle`).
6. **Page-run allocation** — over-aligned `operator new` may waste up to 2×
   committed memory; unmeasured.
7. **Minor/generational performance** — implemented, never exercised.

---

## 8. Audit conclusion

The collector work in 0.260 was **shipped without a benchmark of its own code**
and contains one change (`e830b65`) that more than doubles collection time on two
of three official workloads. Two of its five items cannot be exercised by the
suite at all, and the minor-collection feature has no workload. The builder work
in 0.257 is real but its cost model was never re-measured after the runtime
changed. 0.266 is undocumented.

The correct response is not a broad rewrite: revert the cost of `e830b65` inside
its own design (set-bit walk + work budget), add the missing workloads so future
claims are testable, and gate releases on an interleaved benchmark of the
release commit itself.
