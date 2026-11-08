# Nirqoba Bottleneck Discovery — Summary

**Scope:** evidence-driven investigation of AO, BO, ColO, the object header and
the surrounding system (`tasks/CPInBD.md`).
**Tree:** `fa799e8` on `904aee0`, version 0.266-beta, production runtime.
**Full evidence:** `reports/bottleneck_discovery/00…07`, cost model at
`reports/performance/performance_cost_model.md`, consolidated report at
`reports/performance/COMPREHENSIVE_PERFORMANCE_INVESTIGATION.md`.

---

## The one thing to know

Collection regressed because of **one commit**, and the fix is a hot-loop
rewrite, not a redesign:

> `e830b65` replaced a heap-wide object list with a per-page start-bitmap walk.
> The walk tests every one of a page's 8,192 bits. A 24-byte node has a
> 120-byte span, so only **546 of those bits are set** — 94 % of the loop
> iterations test zero. On a 2.1 M-node graph that is **28,651,521 zero-bit
> iterations per collection**, and sweep went from 25.1 ms to 87.5 ms.

Measured consequence (8 runs per revision, interleaved, harness compiled from
each commit's runtime sources):

| Revision | Collect 02 | Collect 03 |
|---|---:|---:|
| `98a1099` (0.257) | 86.3 ms | 25.4 ms |
| `1ca3c37` (last pre-regression) | 101.3 ms | 24.1 ms |
| **`e830b65`** (page-ordered sweep) | **197.2 ms** | **56.4 ms** |
| `904aee0` (0.266) | 224.9 ms | 60.2 ms |

The revision ranges do not overlap: every pre-`e830b65` run is below 133 ms,
every post-`e830b65` run is above 136 ms. Stage probes attribute 62.4 ms of the
02 change to sweep and 0.0 ms to mark.

---

## Corrections to the inherited picture

1. **The task compares the wrong versions.** "Earlier" (137/25, 478/85) is
   `c384cee`, a benchmark rerun of **0.257 code**; "Later" is the 11:21 run of
   **0.266**. The 0.260 release commit carries the *same stale file* — 0.260 was
   never benchmarked after its own changes.
2. **"0.257 was fastest" is unsupported.** `98a1099` and `c384cee` are identical
   code and measured 146 vs 85 ms (1.7×) on the same machine hours apart.
3. **The header is 96 bytes, not 128.** Span 120, metadata 80 %, 3,841 pages /
   240 MiB for 2.1 M nodes.
4. **AO-01/BO-01 cost models are stale.** `root_store` is 1.6 ns, not ~50
   cycles; `pin_transfer+unpin` 5.1 ns, not ~104 cycles. The validated-descriptor
   memo is dead in production because production no longer validates.
5. **Traversal is not a GC problem.** `sum_tree` compiles to 29 instructions
   with zero runtime calls.

---

## Findings by domain

| Domain | Verdict | Key number | Report |
|---|---|---|---|
| **ColO** | Sweep bitmap walk regression; mark unchanged; two features unexercised | 28.65 M zero bits/col; 2.18 ns each | `03` |
| **AO** | CPU fast path not dominant; footprint and page faults are | 23.2 ns warm, 53.9 cold (30.7 fault) | `01` |
| **BO** | Per-activation frame protocol is 33 % of build | 63 ns/frame, 10 calls/node | `02` |
| **Header** | 80 % metadata; 40 B is the best total design | 120 B span; 1.875 lines/object | `04` |
| **Hidden** | Non-LTO runtime, no memory effects, serial mark, harness blind spots | mark 32.5 ns/object, serial | `05` |
| **Comparative** | 4–6× C++ per node after normalising for 3× bytes | 266 vs 15.8 ns/node build | `06` |
| **Audit** | 5 ColO items, 3 unexercised; 0.260 shipped unbenchmarked | — | `07` |

---

## What to do, in order

1. **Fix the sweep walk (ColO-01).** Use `std::countr_zero` + `bits &= bits - 1`
   exactly as `for_each_live_object_locked()` already does (`gc.cpp:687-710`),
   with the cursor holding the remaining word mask. Projected 225 → ~130 ms (02)
   and 60 → ~33 ms (03); the regression disappears.
2. **Bound work by bitmap words, not only object bytes (ColO-02).** A page with
   one live object still scans 8,192 bits while charging 120 bytes, so sparse
   pages can hold the collector lock far longer than the byte budget implies.
3. **Add the missing workloads before optimising further (ColO-04/05/06).**
   Minor collections, fragmentation, and threads are currently unverified, and
   three shipped changes cannot be exercised by the suite.
4. **Then footprint:** compress `ReferenceLayout` (32 → 8 bytes) and evaluate a
   64-byte header; 40 bytes if array metadata can move to a prefix. This
   improves allocation faults, TLB, sweep pages and cache locality together.
5. **Then protocol:** per-TLAB accounting, frame fast path, runtime memory-effect
   attributes. Do not attempt the inlined fast path without an ABI design.
6. **Fix the measurement.** Interleaved runs, µs-resolution timers, recorded
   compiler versions and binary hashes, peak working set. The current artifacts
   cannot support any claim smaller than ~25 %.

---

## Honest limitations of this investigation

* No hardware performance counters: the mark cost split (hash probe vs bitmap vs
  cache miss) is a labelled hypothesis.
* Only the object-header size of 96 B is measured end-to-end; 64/48/40/32/24 are
  compiled layouts and arithmetic projections.
* The end-to-end benchmarks are noisy (build 291–757 ms for one binary); the
  stage probe is stable, and the regression conclusion rests on it plus
  non-overlapping 8-run ranges, not on a single measurement.
* Minor collection, fragmentation, multithreading, wide-record tracing, and
  large objects are outside the evidence.
* Comparison programs use a 40-byte node against Nirqoba's 120; ratios are
  normalised where stated and are not a like-for-like runtime ranking.
