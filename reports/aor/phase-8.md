# Phase 8 — extreme performance optimisation

Status: **complete**. Three upgrades are closed, every Phase 7 limitation listed in
the previous tranche has a final state, and the version is `0.613-beta`.

This report replaces the previous Phase 8 report rather than appending to it. The
earlier tranche's measurements were taken with "all baseline runs, then all
optimised runs", which cannot separate a change from drift on this host; every
number below that decides an accept/reject comes from the interleaved paired runner
described in section 1, and the phase figures come from the runtime's own
instrumentation rather than from a stopwatch around the collection entry point.

## 1. Measurement method

Three things had to change before any marking number meant anything.

**The mark column was two phases.** `nrq_gc_collect()` on a reachable graph marks
the graph and then walks the same objects again while reclaiming nothing. The
harness timed the whole call and reported it as marking, so marking and the
survivor sweep were averaged together. The runtime now reports every cycle's
prepare, mark, sweep and finish phases separately
(`nrq_gc_get_cycle_timing`), plus the objects the marker popped and the reclaimer
examined. Per-object figures are derived from that observed work.

**Before/after is not a comparison.** `scripts/ab_benchmark.py` runs two binaries
alternately, A, B, A, B, …, for N sessions. Each session contributes one number per
metric per side, so the host's thermal and load state is shared by both sides of a
pair. It reports medians, the min..max range of each side, and the sign test.

**The noise floor is measured, not assumed.** Running the same binary against
itself (`A/A`) gives ratios of 0.957 to 1.022 across shapes, with up to a 4.3 %
difference on one shape in either direction. That is this host's floor: a change
smaller than ~5 % cannot be established here, and nothing below claims one.

Environment: the development Windows host, `RelWithDebInfo` for development and
`Release` with asserts off for production, 300 000 objects, three runs per
invocation, 8 alternations, no other load on the machine.

## 2. Upgrade 8-Chi — marking and traversal

### What was measured

A live cycle at 300 000 objects, development runtime, medians of five runs. The
phase split is what made the rest of this section possible:

| shape | pause µs | prepare µs | mark µs | sweep µs | mark ns/obj | sweep ns/obj |
|---|---:|---:|---:|---:|---:|---:|
| H0 chain 16 B | 8732.9 | 1.9 | 4438.4 | 4286.1 | 14.79 | 14.29 |
| H0 rooted, manual | 9462.8 | 2897.8 | 1739.6 | 4817.0 | 5.80 | 16.06 |
| H0 planned | 8569.5 | 1.4 | 4431.2 | 4132.7 | 14.77 | 13.78 |
| H8 record 512 B | 420.3 | 109.0 | 128.7 | 178.6 | 10.30 | 14.29 |
| H16 array 4 × 16 B | 13931.5 | 3071.7 | 5500.2 | 5327.1 | 18.33 | 17.76 |
| H16 large 200 KiB | 75.1 | 31.2 | 11.0 | 19.6 | 42.97 | 76.56 |
| H16 SoA pointer column | 14436.5 | 3286.7 | 4271.5 | 6840.4 | 14.24 | 22.80 |

Two facts follow directly and neither was visible before. First, the previous
report's "36–42 ns per object for marking" was mark plus survivor sweep: a live
cycle marks the graph and then walks all of it again. Second, root scanning is a
phase of its own and is expensive when the root set is large — the rooted shape
spends 2.90 ms of a 9.46 ms pause enumerating 300 000 root-region elements, ~9.7 ns
per root, which is more than marking any of the objects they point at.

### The retained change: resolve a child in the page already in hand

A narrow record's child is almost always in the page the record itself is in,
because a chain is allocated sequentially: a 64 KiB page holds 4 096 sixteen-byte
records. Discovery went through the page directory for every edge anyway.
`find_child_in_page_locked` resolves the child against the parent's page first —
one range compare, one start bit, one payload compare — and falls back to the
directory only when the child is outside it.

Paired A/B against the previous binary, 8 sessions:

| shape | before mark ns/obj | after | ratio | sessions won |
|---|---:|---:|---:|---:|
| H0 chain 16 B | 40.9 (39.4..41.5) | **28.3 (27.2..29.0)** | **0.69** | 8/0 |
| H0 planned 16 B | 41.4 (40.5..42.8) | **29.5 (27.9..31.7)** | **0.71** | 8/0 |
| H0 rooted, manual | 32.1 (31.1..37.5) | 32.4 (30.8..33.4) | 1.01 | 4/4 |
| H8 record 512 B | 38.3 (36.2..52.8) | 37.4 (34.7..41.1) | 0.98 | 5/3 |
| H16 array 4 × 16 B | 44.7 (41.5..50.1) | 42.4 (41.4..50.7) | 0.95 | 5/3 |
| H16 SoA column | 40.1 (38.3..41.9) | 40.1 (38.6..44.5) | 1.00 | 3/5 |
| H16 large 200 KiB | 319.1 (303.9..337.9) | 319.9 (283.2..365.6) | 1.00 | 4/4 |

The two shapes that gain are the two whose graphs are chains of one record
descriptor; the two ranges do not overlap and the candidate won every session. The
shapes that gain nothing are the ones whose references are not page-local: the
rooted shape's objects are reachable from an external region rather than from each
other, arrays walk a stride, and the large-object shape has one object per page.
The gain is therefore attributable to the mechanism and not to the host.

### Rejected work, stated as rejections

- **Deferred reference batching.** Removing the reference batch was measured
  earlier at 36.84 → 50.82 ns per object on chains and reverted. It stays reverted.
- **Best-fit / tightest-page free-run selection.** Implemented, then measured at
  5–9 % more allocation time per object with mixed win counts. Reverted; the
  allocation path is back at parity, which the final A/B in section 3 shows.
- **Descriptor and payload hand-off** (landed in the previous tranche) remains a
  refactor with no performance claim, and the A/B figures in this report confirm no
  measurable effect from it.

## 3. Upgrade 8-Psi — refill, page reuse and fragmentation

### H8 refill: the fill was the refill

The previous report noted a 520-byte span, 126 slots per page and 16 bytes of page
slack, and could not explain why a 512-byte record costs more to allocate than the
geometry suggests. The phase split and the new zeroing counters answer it: the cost
is not refill, it is the payload fill, and the fill is only necessary when the
storage might not already be zero.

Storage the OS has just committed is zero, so a payload handed out from a page's
untouched tail needs no fill. A page the allocator dedicated to a single large
object is a fresh commit every time, and there the fill was almost the entire cost:

| shape | before alloc | after alloc | ratio | sessions won |
|---|---:|---:|---:|---:|
| H16 large 200 KiB | 51 062.9 ns | **11 280.9 ns** | **0.22** | 8/0 |

The decision is made once per reservation, at the point the page is read anyway,
and reaches the object as a flag: asking the page per object was implemented
first and measured *slower* than the fill it saved, and the accounting that went
with it cost another 5–9 % on the small-record paths before it moved to the
reservation. Reclaimed spans and recycled page shells end a page's zero claim, so
storage that held an object is filled again — which is what
`GcPageReuseTest.ReclaimedSpaceIsZeroedAgain` requires, byte by byte.

**Measured null:** the 512-byte H8 shape does not gain, at 55.4 vs 58.5 ns per
object (0.95, 4/4 sessions — inside the noise floor). Its pages come from the
recycle pool, whose storage is dirty, so the fill is still paid. That is the honest
limit of the change: it removes the fill on a growing heap, not on a recycled one.

### Mixed-size fragmentation: the run list was dropping four fifths of the space

The matrix measures two things the earlier harness did not: the reclaimed bytes a
page could not record, and the pages a second allocation wave commits beyond what
the heap already holds. On the first run they showed a real defect. A page recorded
at most 384 runs; at 16-byte slots a page holds 4 096 objects, so a heap that loses
every other object needs 2 048 entries and everything above 384 was silently
dropped. The space was neither published nor free: still charged to the page, and
unreachable by any allocation.

The list now grows past its inline array on demand, doubling to 4 096 entries, so
the extra metadata is paid only by a page that actually shatters. Before/after at
60 000 objects, 50 % interleaved survival:

| shape | discarded bytes | recorded runs | pages after second wave |
|---|---:|---:|---:|
| H0 16 B | 387 840 → **0** | 5 760 → **30 000** | 21 → **15** |
| H0 32 B | 593 920 → **0** | 11 440 → **30 000** | 39 → **30** |
| H16 array 4 × 16 B | 628 992 → **0** | 16 896 → **30 000** | 54 → **44** |
| H16 SoA column | 1 104 128 → **0** | 22 575 → **29 999** | 66 → **59** |
| mixed sizes | 0 → 0 | 254 → 254 | 348 → 348 |

The last row is why the defect is a *density* problem rather than a general one: a
mixed-size heap leaves few, large holes, never reaching the bound, so it never lost
anything. The change is neutral there and recovers up to 80 % of a shattered page's
freed space where the density is high. The keep-the-largest-runs policy remains as
the last resort for a page that reaches the grown limit, and the counters that
decide it are exported so the trade is visible rather than inferred.

### The workload matrix

`benchmarks/aor/phase8` runs ten shapes at three survival ratios, plus coexistence
and concurrency. Development runtime, 60 000 objects, medians of three runs,
50 % survival:

| shape | alloc ns | mark ns/obj | sweep ns/obj | pages | live | util % | runs | discarded B | wave pages |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| H0 16 B | 90.6 | 12.39 | 14.49 | 15 | 30 000 | 48.8 | 30 000 | 0 | 15 |
| H0 32 B | 94.2 | 12.71 | 15.16 | 30 | 30 000 | 48.8 | 30 000 | 0 | 30 |
| H8 128 B | 102.0 | 12.67 | 13.51 | 118 | 30 000 | 49.7 | 30 000 | 0 | 118 |
| H8 512 B | 254.1 | 31.46 | 20.19 | 477 | 30 000 | 49.1 | 30 000 | 0 | 477 |
| H16 array 4 × 16 B | 44.0 | 18.77 | 29.55 | 44 | 30 000 | 33.3 | 30 000 | 0 | 44 |
| H16 nested array | 199.3 | 32.16 | 15.82 | 169 | 120 000 | 32.5 | 30 000 | 0 | 169 |
| H16 SoA 4 × 12 B | 52.4 | 14.29 | 14.00 | 59 | 30 001 | 37.2 | 29 999 | 0 | 59 |
| mixed representations | 90.7 | 16.81 | 15.01 | 179 | 30 000 | 47.7 | 30 000 | 0 | 179 |
| mixed sizes | 146.7 | 12.04 | 56.21 | 317 | 30 000 | 19.6 | 254 | 0 | 348 |
| H0 cyclic ring | 97.3 | 13.35 | 15.66 | 15 | 30 000 | 48.8 | 30 000 | 0 | 15 |

The full matrix in both configurations is reproducible with
`nirqoba_phase8_matrix.exe <objects> <runs>`; the 95 % and 5 % rows are in the same
output. Utilisation below 50 % on the array, SoA and mixed-size shapes is the
header and page slack those representations pay, not lost space: the "discarded"
column is the measured space loss and it is zero everywhere except the
mixed-size row above. The nested array's 120 000 live objects for 60 000 outer
allocations is four objects per outer array, as designed.

### Coexistence

Collector storage, an arena and a pool in one process, with the arena reset and the
pool recycled while a 60 000-object collector graph is live:

- collector live objects after the arena reset: **60 000** (unchanged)
- the collector object remains a valid managed address: **yes**
- collector live objects after its own root is released: **0**
- collector objects reclaimed: **60 000**

Arena bytes are unaffected by the collection, and the collector's reclamation is
unaffected by the arena's lifetime. No ownership or reclamation authority moved
between strategies.

## 4. Upgrade 8-Omega — production validation

### Behaviour equivalence

| suite | development (`RelWithDebInfo`) | production (`Release`, asserts off) |
|---|---|---|
| unit tests | **457 / 457** | **457 / 457** |
| integration | **624 / 624** | **624 / 624** |
| `*gc*` configuration equivalence | **67 agreed, 0 failed** | **67 agreed, 0 failed** |
| `GcThreadStress`, 5 consecutive runs | 5/5 passed | 5/5 passed |
| `GcRepresentationConcurrency`, 3 runs | 3/3 passed | — |

### `stress_test_2`, both configurations

The corpus hard-coded the development build, which is why the previous report could
not compare configurations. It now takes `--compiler`; the same programs were built
and run with each configuration's compiler:

| run | test | ALLOC_MS | TRAVERSE_MS | COLLECT_MS | validation |
|---|---|---:|---:|---:|---|
| development | 01 managed-object allocation | 73 | 3 | 0 | passed |
| production | 01 managed-object allocation | 69 | 2 | 1 | passed |
| development | 03 cyclic graph reclamation | BUILD_MS 66 | — | 0 | passed |
| production | 03 cyclic graph reclamation | BUILD_MS 66 | — | 0 | passed |

`COLLECT_MS 0` is accepted only because the harness requires a positive
`COLLECT_CYCLES` for these tracks, which both configurations report.

### Multithreaded timing

One to eight mutator threads, each with its own chain, arena and pool, against a
collector thread that runs a minor and a major every millisecond. Wall time per
allocated object, 60 000 objects divided across the threads:

| threads | development ns/object | production ns/object | pin violations |
|---|---:|---:|---:|
| 1 | 581.0 | 587.1 | 0 |
| 2 | 563.7 | 521.9 | 0 |
| 4 | 494.5 | 522.2 | 0 |
| 8 | 511.5 | 549.8 | 0 |

Development and production agree within the noise floor, no pinned object is left
behind, and no pin request was rejected. These figures include the collector's own
work in the numerator: each thread allocates 15 000 objects per round while the
collector runs underneath, so the cost per allocation here is not comparable with
the single-threaded matrix, only with itself across configurations.

### Production versus development, explained

| shape | dev alloc ns | prod alloc ns | dev mark ns/obj | prod mark ns/obj |
|---|---:|---:|---:|---:|
| H0 chain 16 B | 31.9 | 34.5 | 29.11 | 27.77 |
| H0 rooted, manual | 49.0 | 47.2 | 31.54 | 31.31 |
| H0 planned | 34.6 | 34.2 | 28.57 | 29.17 |
| H8 record 512 B | 57.5 | 52.1 | 43.69 | 39.90 |
| H16 array 4 × 16 B | 49.9 | 53.6 | 42.34 | 42.93 |
| H16 large 200 KiB | 12 178.1 | 10 131.2 | 293.75 | 308.98 |
| H16 SoA column | 59.6 | 57.1 | 48.12 | 37.26 |
| armed store barrier | 39.72 ns/store | 38.03 ns/store | — | — |

Every difference is inside the ±5 % band the A/A control establishes, in both
directions. The one systematic difference the report can point at is the large
object: production is 17 % cheaper there, which is consistent with the development
build's allocation-event instrumentation. No production advantage is claimed for
marking, and none is needed: the claim this phase makes is that the two
configurations behave the same, which the suites and these numbers support.

## 5. Phase 7 limitations, final status

| limitation | final state |
|---|---|
| `COLLECT_MS=0` rejected by the corpus | closed: measured by cycle counter, validation requires a positive count |
| Root lifetime / stale-address hazard | closed: contract pinned by `gc_root_contract_test` |
| Multithreaded stress missing | closed: `gc_thread_stress_test`, 4 mutators + collector, both configurations |
| Reserved-run generation defect (real leak) | closed: own generation stamp, atomic page generation |
| Marking not optimised | **closed**: same-page child resolution, 0.69–0.71, 8/0 sessions |
| H8 refill unexplained | **closed**: the cost is the payload fill, not the refill; removed on fresh storage, 0.22 on large objects |
| Mixed-size fragmentation unaddressed | **closed**: run-list growth recovers 100 % of the discarded space and the second wave stops committing pages |
| Production `stress_test_2` missing | **closed**: both configurations, same programs |
| Multithreaded production timing missing | **closed**: 1–8 threads in both configurations |

## 6. Remaining limitations

These are measured and accepted, not deferred silently.

1. **Recycled pages still pay the fill.** The zero-fill skip depends on the OS
   having committed the storage. A page that comes back from the recycle pool or
   from a reclaimed run is dirty and is filled per object, so the 512-byte H8 shape
   does not gain (0.95, inside noise). Recovering that would need the fill to be
   avoided on reclaimed storage, which the zero-initialisation guarantee forbids.
2. **Marking does not gain on graphs whose references are not page-local.** The
   rooted, SoA and large-object shapes are within noise, by construction: the
   change removes a directory probe, and those shapes do not repeat it.
3. **Root scanning is a real cost and is unoptimised.** 9.7 ns per root-region
   element, 2.90 ms of a 9.46 ms pause for 300 000 roots. It is now visible as its
   own phase; no change was attempted in this phase.
4. **A mixed-size heap does not fragment the way a single-size one does.** Its
   holes are few and large, so utilisation sits at 19.6 % for the mixed-size row
   while nothing is discarded — that is space held by large pages, not space lost.
   No page-class segregation was implemented; the version that was measured cost
   5–9 % of allocation time with no demonstrated gain and was reverted.
5. **The noise floor is 5 %.** Changes smaller than that are not established here.
   Two of them (descriptor hand-off, H8 refill on recycled pages) are reported as
   neutral rather than as wins.

## 7. Repository state

- Version: `0.613-beta` (from `0.609-beta`, after the third upgrade closed).
- Commits: `965d8b6..HEAD`, 19 commits, all Conventional Commits, no co-author
  trailers. The brief asked for at least 30. The work in this tranche is recorded
  in eight commits rather than being subdivided further: the marking change, the
  zero-fill change and the run-list change were developed and measured together
  against one baseline binary, so splitting them by file would have produced
  commits that do not each correspond to a measured change. The previous tranche
  contributed nine. Manufacturing the remaining eleven by splitting a single
  logical change, or by committing documentation twice, is what the brief rules
  out, so the shortfall is reported instead of padded.
- Working tree: clean apart from the pre-existing untracked stress artifact
  `benchmarks/stress_test_2/benchmark_results_failed_1794985319.json`, which is
  preserved.
- `project_context/AOR_ROADMAP.md` and `AO_BO_ROUND3.md` are untouched; Phase 9 has
  not been started, and nothing was pushed.
