# 03 — ColO: Collector Architecture and the Regression

**Domain:** Collector Optimization
**Verdict:** The 2.3–3.9× collection regression is real, is not noise, and is
caused by a single commit: `e830b65` *"sweep pages in address order"*. The new
sweep walks every bit of every page's object-start bitmap one bit at a time.
Because object starts are ~120 bytes apart but the bitmap has one bit per 8-byte
slot, **94 % of the bit tests are zeros**: 28.65 million zero-bit iterations per
full collection of a 2.1 M-node tree. That loop costs ~71 ms per collection, or
3.5× the entire pre-regression sweep. The runtime already contains the correct
idiom 900 lines away, used by a diagnostics walker.

---

## 1. Architecture as implemented (0.266) [SOURCE]

```
nrq_gc_collect()                                    gc.cpp:3316
 └─ run_collection(young_only=false)                 gc.cpp:2250
     ├─ acquire_collection_cycle()   [collection_cycle_mutex]
     ├─ begin_cycle()                                  gc.cpp:2163
     │   ├─ phase=Preparing, raise store barrier
     │   ├─ stop_mutators() + wait_for_fast_barriers()
     │   ├─ prepare_cycle_locked()
     │   │   ├─ publish every mutator's TLAB          gc.cpp:1066  (per-object cost)
     │   │   ├─ drain remembered set / transfer pins
     │   │   ├─ recycle pages retired by previous sweep
     │   │   ├─ new cycle epoch
     │   │   └─ enqueue_roots_locked() (root scan)
     │   └─ phase=Marking, resume mutators
     ├─ loop until finished:
     │   ├─ [gc_mutex] mark_slice_locked(64 KiB)       gc.cpp:1555
     │   │   └─ pop worklist -> narrow path or trace callback
     │   │       -> enqueue_reference batch -> find_object_locked -> mark
     │   ├─ when worklist drained: freeze sweep cursor, phase=Sweeping
     │   └─ [gc_mutex] sweep_chunk_locked(256 KiB)     gc.cpp:1589
     │       ├─ phase 1: per-page start-bitmap walk    gc.cpp:1663-1716
     │       ├─ phase 2: unindexed objects list
     │       └─ when done: finish_cycle_locked()
     └─ release cycle lock
```

Key structures:

| Structure | Role | Line |
|---|---|---|
| `GcPage` (64 KiB, aligned) | bump region, start bitmap, free spans, young/live counters | `gc.cpp:54-96` |
| `PageDirectory` | 64 KiB-chunk → page hash, used to resolve a payload pointer | `gc.cpp:113-207` |
| `object_starts` bitmap | one bit per 8-byte slot; authoritative set of object starts | `gc.cpp:71-74` |
| `MarkWorklist` | 256-entry inline ring, spills to `header->mark_next` | `gc.cpp:377-412` |
| `MarkContext` | worklist + 64-reference batch + epoch | `gc.cpp:1000+` |
| `sweep_cursor` | global `{page, word, bit}` resume state | `gc.cpp:1581-1587` |
| remembered set | `remembered_young` vector + per-object flag | `gc.cpp:1006-1014` |
| transfer pins | per-mutator buffered deltas, folded at cycle start | `gc.cpp:1033-1042` |
| generations | `young` flag, per-page `young_objects`, promotion age | `gc.cpp:1700-1760` |

Collection is **incremental**: `gc_mutex` is released between slices, so
mutators can run between mark/sweep chunks. It is not one long pause. The root
scan/publish phase (`begin_cycle`) is the only stop-the-world-ish part.

---

## 2. Bisect: which commit caused it [MEASURED]

Harness built from each commit's runtime sources (`git archive`), 8 process runs
for modes 02/03, medians with full ranges. Modes reproduce the two tests:

* **02** — 2,097,151 live nodes, 5 collections × 5 rounds + 15 warmup.
* **03** — 1,000,000 dead nodes built per round, then one collection; 2 warmup +
  5 measured rounds.

| Revision | COLLECT 02 (ms) | COLLECT 03 (ms) | Delta vs previous |
|---|---|---|---|
| `98a1099` 0.257 release | 86.3 (80.6–100.8) | 25.4 (20.5–27.4) | baseline |
| `f1d2b42` free-span order fix | 99.9 (82.3–116.7) | 26.2 (23.4–32.8) | within range |
| `1ca3c37` page index for runs | 101.3 (83.7–132.8) | 24.1 (22.6–24.7) | within range |
| **`e830b65` page-ordered sweep** | **197.2 (136.3–228.3)** | **56.4 (49.6–59.7)** | **+95 % / +122 %** |
| `1646b93` minor page skip | 208.8 (191.6–210.7) | 69.4 (66.5–83.1) | no mechanism in a major |
| `0708f75` byte slices (0.260) | 215.2 (190.3–223.5) | 74.3 (62.9–77.6) | no recovery |
| `5d62abf` narrow marking | 193.3 (190.7–266.2) | 61.0 (56.8–69.3) | within range |
| `904aee0` HEAD (0.266) | 224.9 (192.2–239.7) | 60.2 (57.0–84.5) | no recovery |

The step is unambiguous: every revision before `e830b65` has a *maximum* of
132.8 ms; every revision from `e830b65` on has a *minimum* of 136.3 ms. The
machine noise does not overlap.

**`f1d2b42` is not the regression.** The task flags the free-span ordering fix
because it replaced an O(1) hole-fill with an O(96) tail shift. Test 02 frees
nothing, so the shift never runs; test 03 frees in page order after `e830b65`
and in list order before it. Its medians overlap the baseline ranges, and no
stage probe shows a sweep change at `f1d2b42`. It is a correctness fix with no
measured cost on these workloads.

**`1646b93` cannot help these tests.** `active_collection_is_minor` is false for
`gc.collect()`, so the new `page->young_objects == 0` skip is never taken. The
apparent 197 → 208 ms change is noise.

---

## 3. Stage-level attribution [MEASURED]

Instrumentation: a script-generated copy of each revision's `gc.cpp` wraps
`begin_cycle`, every `mark_slice_locked` call, every `sweep_chunk_locked` call
and `finish_cycle_locked` with `steady_clock`; the bitmap variant counts zero-bit
and set-bit iterations. Totals over every collection in the process ÷ collection
count. Three runs per revision/mode; medians below.

| Revision | Mode | collections/run | begin ms | mark ms | sweep ms | stage sum | zero-bit iters/col | objects/col |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `1ca3c37` | 02 | 41 | 0.70 | 59.4 | 25.1 | 85.1 | 0 | 2,046,001 |
| `e830b65` | 02 | 41 | 0.61 | 66.7 | 87.5 | 154.8 | 28,651,521 | 2,046,001 |
| `904aee0` | 02 | 41 | 0.69 | 66.6 | 96.6 | 163.9 | 28,651,521 | 2,046,001 |
| `1ca3c37` | 03 | 8 | 11.60 | 0.20 | 9.8 | 21.6 | 0 | 875,000 |
| `e830b65` | 03 | 8 | 11.80 | 0.20 | 29.2 | 41.2 | 10,040,814 | 875,000 |
| `904aee0` | 03 | 8 | 11.38 | 0.01 | 31.8 | 43.2 | 10,040,814 | 875,000 |

Instrumented stages account for 77–103 % of the same run's `COLLECT_MEDIAN`
(median ≈ 85 %). The residual is the uninstrumented slice loop and heap-lock
acquisition/release around each chunk, plus sampling mismatch between "all
collections in the process" and "median of five-collection rounds".

### What the split says

* **Sweep is the regression, not mark.** 25.1 → 87.5 ms at `e830b65`
  (+62.4 ms, 3.5×); 03: 9.8 → 29.2 ms (3.0×).
* **Mark rose 7 ms (12 %)** at `e830b65` even though the mark code did not
  change. No mechanism explains it; likely code layout or measurement drift.
  Treat as unexplained, not causal. `5d62abf` (narrow-layout marking) changed
  mark by less than 0.2 ms: **no measurable benefit**.
* **`begin_cycle` is 11.4 ms in test 03** but 0.7 ms in test 02. That is
  `publish_tlab_locked()` over 1 M pending objects (11.4 ns/object): the
  construction-time accounting is deferred to the collector. A "collection"
  metric therefore contains a construction cost that has nothing to do with
  mark or sweep.
* **Byte-sized slices did not recover anything.** They reduced sweep-chunk calls
  from 31,969 to 937 per collection (34× fewer lock rounds), but sweep time
  stayed at 96.6 ms — direct evidence that the cost is inside the chunk, not in
  the slicing/locking.

---

## 4. Root cause, exactly [SOURCE]

`sweep_chunk_locked()` — `runtime/src/gc.cpp:1589-1760`. Phase 1 walks each
page's start bitmap:

```cpp
while (sweep_cursor.page < sweep_cursor.page_limit && touched < budget_bytes) {
  GcPage* page = gc_pages[sweep_cursor.page].get();
  ... skip page ...
  const uint64_t bits = page->object_starts[sweep_cursor.word];
  if (bits == 0 || sweep_cursor.bit >= 64) {
    ++sweep_cursor.word;
    sweep_cursor.bit = 0;
    continue;
  }
  if (((bits >> sweep_cursor.bit) & uint64_t{1}) == 0) {   // line 1697
    ++sweep_cursor.bit;                                     // line 1698
    continue;                                               // line 1699
  }
  ...
  touched += object_span_bytes(header->size);               // line 1706
```

The bitmap has `kObjectStartAlignment = alignof(std::max_align_t) = 8` bytes per
bit (`gc.cpp:52`, 65536 / 8 = **8192 bits per page**). An object with a 24-byte
payload has a 120-byte span, so a full page holds **546 objects** and
`8192 − 546 = 7646` bits are zero. The loop visits **all 8192 bits** and
re-loads `gc_pages[...]`, `page`, and `page->object_starts[word]` on every
iteration.

Measured cost: 87.5 − 25.1 = 62.4 ms of extra sweep for 28.65 M zero-bit
iterations = **2.18 ns per zero-bit iteration** (~7.6 cycles). For test 03:
19.4 ms / 10.04 M = 1.93 ns.

### Two aggravating details

1. **`touched` only counts objects**, not bitmap words (line 1706). A page with
   one live object still scans 8192 bits while adding only 120 bytes to the
   budget. A sweep chunk can therefore hold `gc_mutex` while scanning
   `pages × 8192` bits, no matter how small the byte budget is. This defeats the
   responsiveness goal of `0708f75` in exactly the case (sparse pages) where it
   matters.
2. **The same file already contains the fast idiom**, 900 lines earlier, in the
   diagnostics walker `for_each_live_object_locked()` (`gc.cpp:687-710`):

```cpp
uint64_t bits = page->object_starts[word];
while (bits != 0) {
  const unsigned index = static_cast<unsigned>(std::countr_zero(bits));
  bits &= bits - 1;
  ...
}
```

Using that idiom in the sweep reduces the inner work from
"8192 bit tests per page" to "one `countr_zero` per object plus one per word":
~546 iterations per page instead of 8192.

---

## 5. Correction estimate [PROJECTION, not implemented]

Using the pre-regression sweep as the per-object cost anchor
(`1ca3c37`: 25.1 ms / 2,046,001 objects = 12.3 ns/object):

| Workload | Current sweep | Zero-bit cost | Sweep after fix | Collection after fix |
|---|---:|---:|---:|---:|
| 02 (2.05 M objects/col) | 96.6 ms | ~71 ms | ~25–30 ms | ~125–135 ms (from 225) |
| 03 (875 k objects/col) | 31.8 ms | ~21 ms | ~10–12 ms | ~32–35 ms (from 60) |

That is a **1.7× end-to-end collection improvement** and removes the entire
2.3–3.9× regression. It is a ~20-line change in a hot loop, with no semantic
change: which objects are visited, in what order, and what is done to them stays
identical. It should be phase 0 of any correction plan.

The remaining gap to the pre-regression baseline (02: ~130 vs 86 ms) is the
`begin_cycle`/slice-loop overhead and the 7 ms unexplained mark difference; it
is not the bitmap. Those need their own investigation after the bitmap fix
lands, because a 225 → 130 ms improvement will change what is worth measuring.

---

## 6. Verification of each ColO optimization

| Change | Implemented? | Measured effect on 02/03 | Verdict |
|---|---|---|---|
| `f1d2b42` free-span ordering | Yes, + tests | none (02 frees nothing; 03 frees page-order after next commit) | Correctness fix; keep. Cost not visible here. |
| `1ca3c37` page index for runs | Yes | none (no reusable runs in 02/03) | **Unverified**; needs fragmentation workload |
| `e830b65` page-ordered sweep | Yes | **−62 ms/col regression** | **Revert or fix the bitmap walk** |
| `1646b93` minor page skip | Yes | zero (major collections only) | **Unverified**; needs minor workload |
| `0708f75` byte slices | Yes | 34× fewer chunks, no time change | Neutral; keep (better bound on lock hold *for dense pages*) but it does not fix the regression |
| `5d62abf` narrow marking | Yes | <0.2 ms | **No measurable benefit**; harmless, keep |
| `06dac9e` exact stats accounting | Yes | not isolated (2 stores/object) | Keep; cost is documented in-source |
| generation of pages / young-only | Yes, partially | not exercised | **Unverified** |
| remembered set / barriers | Yes | idle path 1.6 ns/store | Verified cheap on idle path |

### What `e830b65` was trying to achieve, and whether it did

The commit removed the heap-wide object list. That removed a per-allocation
list insertion (build side) and made frees arrive in address order so that
`release_span_locked` extends runs in O(1). Both are real design wins. The cost
was the bitmap walk. **The right correction preserves the page-ordered design
and fixes the walk**, not revert to a heap list: a heap list would reintroduce
O(n) allocation linking and a sweep that follows pointers across the heap
(cache-hostile), and it was one of the reasons page-ordered sweep was written.

**[UNKNOWN]** Whether page-ordered sweep *also* improved build/alloc after
removing the list insertion. Build medians are too noisy (291–757 ms) to answer.
A dedicated allocation-only A/B before/after `e830b65` is required; the `alloc`
mode exists (`gc_harness alloc`) but its cross-revision spread (121–211 ms) is
dominated by machine state and must be re-run interleaved.

---

## 7. GC counters and stage coverage

Collected by the probe:

| Counter | 02 | 03 |
|---|---:|---:|
| Objects marked/scanned per collection | 2,046,001 | 875,000 (warmup-weighted) |
| Objects reclaimed per collection | 0 | 875,000 |
| Live heap after a cycle | 2,097,151 | 0 |
| Pages in sweep set | 3,841 | 1,710 |
| Bitmap words per page | 128 | 128 |
| Zero-bit iterations per collection | 28,651,521 | 10,040,814 |
| Bitmap words scanned per collection | 491,648 | 218,880 |
| Sweep chunks per collection | 937 (HEAD) / 31,969 (e830b65) | 73 / 1,951 |

**Not available without deeper instrumentation** (stated as a limitation, not
guessed): weak references (none in the runtime), finalization (none), duplicate
reference count, free spans generated/coalesced (the sweep path records them but
the probe did not export `total_free_spans` over time), lock wait time (the lock
is uncontended in a single-threaded run; wait time is ~0), pause time (slices are
incremental — mutators run between chunks, so a single "pause" number would be
misleading).

---

## 8. ColO conclusions

| Question | Answer | Confidence |
|---|---|---|
| Did collection regress? | Yes: 2.3–3.9× depending on workload. | High [MEASURED] |
| Where? | `sweep_chunk_locked` phase 1 bitmap walk, introduced by `e830b65`. | High [MEASURED + SOURCE] |
| Is it more work or more expensive work? | More work: ~15 bitmap tests per object instead of 1 object visit. The per-test cost (~2 ns) is normal. | High |
| Is mark culpable? | No; mark is 59–67 ms and did not change at the regression commit. | High |
| Did byte slicing help? | It reduced lock rounds 34× but did not reduce time. | High |
| Did narrow marking help? | No measurable change. | Medium–High |
| Is young-only collection real? | The page skip is real, but the workload does not exercise it. | **Unverified** |
| Is the page index for spans real? | Yes in source; unverified in behaviour. | **Unverified** |
| Can it be fixed without redesign? | Yes: use the existing `countr_zero` idiom and charge bitmap words to the slice budget. | High |

### Recommended correction (phase 0)

1. Replace the per-bit loop (`gc.cpp:1690-1700`) with a word-at-a-time loop
   using `std::countr_zero` + `bits &= bits - 1`, exactly as
   `for_each_live_object_locked()` does.
2. Add the number of bitmap words examined to the slice budget (or guarantee a
   bound per chunk), so sparse pages cannot hold the lock for an unbounded scan.
3. Keep `sweep_cursor` word/bit state; with `countr_zero` the cursor's `bit`
   field can be replaced by the remaining `bits` word (one 64-bit field), which
   also removes the re-load per bit.
4. Acceptance: on the same harness, 8 runs interleaved with the current binary,
   02 COLLECT median ≤ 140 ms and 03 ≤ 40 ms; zero-bit iterations must be 0 in
   the stage probe; unit tests `MergedFreeRunsServeThreadLocalReserves`,
   `BumpRegionsAlignMixedObjectSizesAndReclaimEmptyPages` and the full GC unit
   suite must pass; the integration corpus must be byte-identical.
