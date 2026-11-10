# AO-06 — TLAB size and reuse tuning

**Base commit:** `bf0b989` (AO-06 stage 3)
**Result commits:** `51414d7` (one-pass run batch), `001cc15` (reservation-cost
observability), `bf0b989` (tail reclamation at safepoints), `af1ea9c` (whole-object
grants), `f14f9ef` (AO-07 allocation facts)
**Configuration:** production runtime (`NRQ_RUNTIME_DEBUG_ENABLED=0`), MSVC 18,
RelWithDebInfo + `/Ob2`, Windows x64.
**Labels:** MEASURED / DERIVED / PROJECTION / UNVERIFIED. Timings are MEASURED with
their sample count; machine noise on this host is 5–20%, so only medians of
interleaved pairs are treated as changes.

## 1. Scope and method

The roadmap (§7) permits a TLAB size change only after measuring refill frequency,
unused tail, page fragmentation, reuse hit rate, peak committed memory and
multithreaded contention, and warns that a larger slab can trade refills for stranded
memory. This stage measured all six, then landed four changes that remove the cost
those measurements exposed — without changing `kGcTlabBytes`, `kGcPageBytes` or the
object header.

Probes (all in `build/ao/`, source committed alongside the earlier stages):

| probe | workload |
| --- | --- |
| `tail_probe.exe <rounds> <count> <bytes> <mix>` | pure or mixed payloads from a fresh heap; reports refills, stranded tail (bytes and max), reserved runs, pages with runs, reused span bytes, pages, commit, peak |
| `frag_probe.exe 200000 64` | two waves into a heap the first wave then fragmented (`index % 4` pinned) |
| `mt_probe.exe 200000 4` | the same two waves with 4 mutator threads |
| `mixed_probe.exe` | churn with collection cycles, reports reuse |

Baselines: `frag_b2.exe` / `mt_b2.exe` are the stage-2 build, `tail_u3.exe` the
stage-3 build. A/B pairs are run interleaved in one shell; each table row is the
median or the individual samples when the sample count is small.

## 2. The six measurements

### 2.1 Refills per workload (MEASURED, rounds=1, fresh heap, final build)

| payload | objects | refills | objects per refill | objects per page | pages |
| --- | --- | --- | --- | --- | --- |
| 64 B | 100,000 | 245 | 408 | 409 | 245 |
| 512 B | 100,000 | 935 | 107 | 107 | 935 |
| 4 KiB | 20,000 | 1,334 | 15 | 15 | 1,334 |
| 8 KiB | 20,000 | 2,858 | 7 | 7 | 2,858 |
| 32 KiB | 6,000 | 6,000 | 1 | 1 | 6,000 |

One refill per page is the floor for a bump allocator whose reservation is a whole
page: every object in a page arrives under a single reservation. The 64 B case is the
hot one and it is already at 1 refill per 408 objects (≈1.5 refills per MiB
allocated); a larger slab cannot beat the floor, it can only make one reservation
cover *several* pages, which is the stranded-memory trade the roadmap warns about.

### 2.2 Unused tail — before and after (`tail_u3.exe` vs final, MEASURED)

"Stranded tail" is reserved-but-unused bytes charged to pages at the end of the run;
it is unreachable for allocation until a safepoint returns it.

| payload | objects | tail before | max/page before | tail after | max/page after |
| --- | --- | --- | --- | --- | --- |
| 64 B | 100,000 | 23,424 | 96 | **0** | 0 |
| 4 KiB | 20,000 | 3,540,448 | 2,656 | **0** | 0 |
| 32 KiB | 6,000 | 195,999,328 | 32,672 | **0** | 0 |

The 32 KiB row is the important one: 196 MB of charged dead space over 6,000 objects,
i.e. ~32.7 KB stranded per 32 KB object. Before the whole-object grant, the
reservation charged a whole page and the remainder of the last object's span stayed
charged forever. Two changes removed it (`bf0b989` returns a live tail at the next
safepoint; `af1ea9c` never creates one, because every grant ends on an object
boundary).

Across 200,000-object churn rounds the same quantity fell from 74,048 B per cycle to
0, with the per-cycle maximum from 62,144 B to 640 B and then to 0.

### 2.3 Page fragmentation (`frag_probe.exe 200000 64`, MEASURED, 3 interleaved pairs)

| build | wave-2 ALLOC_MS | wave-2 refills | pages | commit |
| --- | --- | --- | --- | --- |
| stage 2 (`frag_b2.exe`) | 7.221 / 7.757 / 7.135 (median 7.221) | 2,102 | 635 | 47.4 MB |
| final | 8.003 / 6.254 / 7.157 (median 7.157) | 2,102 | 635 | 47.4 MB |

Neutral: the stage-2 batch already removed the repeated-cell walk this probe used to
measure, and the later stages do not touch it. Wave-2 refills stay at 2,102 for
200,000 objects — 1 per 95 objects, 4.3× the fresh-page rate — which is the honest
cost of a heap whose free space is short runs rather than whole pages. Lowering it
further needs a run-merging policy, not a bigger slab.

### 2.4 Reuse hit rate

DERIVED from the same run: wave 1 leaves 489 pages holding 50,000 live objects; wave 2
allocates 200,000 objects and the heap grows to 635 pages. At 409 objects per page,
150,000 new objects would need 367 fresh pages if nothing were reused; 146 were
allocated, so ~60% of wave 2 was served from space an earlier cycle had freed. The
`tail_probe` pure workloads report `reused=0`, which is correct — a fresh heap has
nothing to reuse — and that is why the fragmentation and churn probes, not the tail
probe, carry the reuse numbers.

### 2.5 Peak committed memory (MEASURED, `tail_probe` and `mt_probe`)

| workload | pages before→after | commit before→after | peak WS |
| --- | --- | --- | --- |
| 64 B × 100,000 | 245 → 245 | 18.4 → 18.4 MB | 24.9 → 24.4 MB |
| 4 KiB × 20,000 | 1,334 → 1,334 | 89.7 → 89.7 MB | 95.6 → 95.6 MB |
| 32 KiB × 6,000 | 6,000 → 6,000 | 394.4 → 394.4 MB | 236.0 → 236.0 MB |
| fragmented 200k × 64 B | 635 → 635 | 47.4 → 47.4 MB | — |

No stage raised committed memory on any measured workload. The whole-object grant
therefore bought its 196 MB of dead tail back at no page cost — the pages were
already there, they just stopped being charged for space nobody could use.

### 2.6 Multithreaded contention (`mt_probe.exe 200000 4`, MEASURED, 3 interleaved pairs)

| build | MT_MS |
| --- | --- |
| stage 2 (`mt_b2.exe`) | 12.777 / 13.613 / 12.110 (median 12.777) |
| final | 10.514 / 9.487 / 11.867 (median 10.514) |

−18% median with 4 mutators. The gain is the run batch from `51414d7`: one lock
acquisition now answers the next 24 refills, and the earlier stage-2 report measured
the same effect from 17.33 → 12.37 ms with the same workload shape. A single thread
sees no change (`frag_probe` is neutral), which is what a lock-side change should
look like.

## 3. The four changes, and what each is worth

### 3.1 One pass over a fragmented page's free list — `51414d7`

`take_free_runs_locked` compacts the tombstoned list once and refreshes the page's
maximum run in the same pass, instead of restarting the walk per run. MEASURED,
interleaved pairs: fragmented ALLOC 8.42 → 7.20 ms median, MT4 13.26 → 10.01 ms,
mixed 7.08 → 6.52 ms, with identical refills, pages and commit. The per-cycle
`free_spans` maintenance the roadmap flagged is therefore no longer the dominant
cost; what remains is the run-splitting itself (2,102 refills for 200,000 objects).

### 3.2 Observability — `001cc15`

`nrq_gc_tlab_tail_bytes`, `nrq_gc_tlab_tail_max_bytes`, `nrq_gc_free_run_count`,
`nrq_gc_page_with_runs_count`, `nrq_gc_reused_span_bytes`, plus `account_tlab_tail`
and `install_tlab` so every cursor install charges the same accounting. Without these
the 196 MB of dead tail in §2.2 would have been invisible: the heap reported 394 MB
committed for 196 MB live and nothing explained the difference.

### 3.3 Return an unused tail to its page at a safepoint — `bf0b989`

`release_tlab_tail_locked` runs in `prepare_cycle_locked` (before the page generation
moves, so no mutator can still hold the cursor) and on mutator leave. MEASURED on the
churn workload: stranded tail 74,048 → 864 B per cycle, per-cycle maximum 62,144 →
640, 61,280 B reclaimed and reused. Commit and peak unchanged.

### 3.4 Grant whole objects, never part of one — `af1ea9c`

`whole_object_grant` rounds every reservation down to a multiple of the object's
aligned span, and the page-tail path does the same. MEASURED: §2.2's table (tail to 0
at every payload), §2.5 (no page or commit change).

The first version of this change cost the 32 KiB case 25% (134–141 ms vs 101–114):
leaving sub-span tails behind put thousands of pages the request could not use into
the tail index, and the refill walked them to prove the negative. `max_indexed_tail_bytes`
answers "can any indexed page serve this size?" in one comparison and is recomputed
exactly on a full miss, which restored the case to neutral (83.9 / 101.5 vs 83.9 /
97.0 ms). This is recorded because it is the kind of regression a smaller measurement
set would have shipped.

## 4. Decision: do not change the TLAB, page or slab size

Evidence, in order of weight:

1. **Refills are at the floor.** Every payload measured costs one refill per page
   (§2.1). A 64 KiB slab already spans a whole page; making it larger means one
   reservation covers several pages, which directly increases the stranded tail the
   previous stage was written to eliminate.
2. **The tail is already zero** (§2.2). A larger reservation can only recreate it.
3. **The remaining waste is not a TLAB parameter.** Committed bytes per payload at
   64 B are 18.4 MB for 6.4 MB of payload, and the gap is the 96-byte object header
   (2.5× for a 64-byte object). That is the AO-07/header roadmap's subject and was
   explicitly out of scope here.
4. **The 32 KiB case is page quantisation, not slab size.** A 32,864-byte span cannot
   fit twice in a 64 KiB page, so one object per page and 2.00× commit is forced by
   `kGcPageBytes`, not by the reservation. Fixing it belongs to the large-object
   policy (charge a span of exactly the object's size, or a page of twice the span),
   and that decision needs its own measurement of peak commit on mixed heaps — a
   PROJECTION of 4× peak working set for small heaps is not worth an unmeasured
   change.
5. **Contention is solved elsewhere.** MT4 improved 18% by batching lock acquisitions
   (§2.6), which a bigger slab would also do — but it would do it by stranding
   memory, and the batch is free.

PROJECTION (not measured, not landed): a 256 KiB slab would turn the 32 KiB case into
4 objects per slab and cut page slack from 32,672 B to ~4 KB per 7 objects, at the
cost of 4× the per-thread working set and a larger stranded tail when a thread stops
allocating mid-slab. Under AO-06's own rule — no larger TLAB without stranded-memory
evidence on the other side — that trade is rejected.

## 5. Test coverage added

| test | what it pins |
| --- | --- |
| `GcTest.ReservationsChargeWholeObjectsOnly` | 200 × 32,000 B leaves less than one span stranded, and at most one page per object |
| `GcTest.AllocationProfileMatchesWhatTheAllocatorCharges` | the AO-07 profile model against real allocation: 4,090 × 64 B ⇒ 10 pages, 10 refills; 100,000 B ⇒ a page of exactly its span |
| `GcTest.FragmentedPagesServeReservedRunsWithoutRepeatingCells` | the run batch serves reserved runs without re-walking cells |
| `GcTest.ReservedRunsReturnToTheirPagesAtTheCycleBoundary` | no reserved run survives `prepare_cycle_locked` |

201/201 unit tests and 263/263 integration tests pass at `f14f9ef`.

## 6. Limits

- Timings are single-host and noisy (5–20%); the only timing claims above are ones
  that repeated across interleaved pairs (MT4 −18%) or are structural (one refill per
  page, tail to zero, identical page counts).
- The 512 B and 8 KiB payloads were measured only on the final build; their
  pre-change tail is UNVERIFIED because the pre-change probe binary predates the mix
  flag and would have compared different workloads.
- Reuse hit rate is DERIVED from page counts, not counted per allocation; the runtime
  now exposes `nrq_gc_reused_span_bytes` for a direct count, which the churn probe
  reports but the two-wave probes do not split by wave.
- Peak working set was sampled per process, not tracked per stage.
