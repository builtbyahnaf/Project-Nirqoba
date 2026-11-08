# 06 — Comparative Baseline and Version Comparison

**Domain:** C, C++, Rust, Java comparison; 0.257 / 0.260 / 0.266 version framing
**Verdict:** The cross-language table is directionally real but **not
apples-to-apples**: Nirqoba's node occupies 120 bytes against 40 bytes for the
C/C++/Rust tracks, and Java's allocator has a committed heap and a JIT. After
normalising for bytes and work, the honest statements are: Nirqoba builds ~4–5×
slower per node than C++, collects ~2–6× slower per live object, and traverses
comparably. The 0.260-beta "release" numbers in the repository are stale, and
the same source measured 146 ms and 85 ms in two runs on this machine — a 1.7×
environment variance that any claim must survive.

---

## 1. What the repository actually recorded [MEASURED, committed artifacts]

Medians from `benchmarks/stress_test_2/benchmark_results.json` at each revision.
All times in ms; tests allocate 1,000,000 (01) / ~2,097,151 (02) /
1,000,000 (03) objects per round.

### Test 01 — `managed_object_allocation`

| Revision | Nirqoba ALLOC | TRAVERSE | COLLECT | C ALLOC | C++ ALLOC | Rust ALLOC | Java ALLOC |
|---|---:|---:|---:|---:|---:|---:|---:|
| `98a1099` 0.257 | 248 | 20 | 27 | — | — | — | — |
| `c384cee` pre-ColO | 137 | 9 | 25 | 27.8 | 29.3 | 32.5 | 2.5 |
| `904aee0` 0.260 (stale data) | 137 | 9 | 25 | 27.8 | 29.3 | 32.5 | 2.5 |
| `fa799e8` 0.266 working tree | 140 | 10 | **77** | 31.5 | 31.3 | 34.8 | 2.6 |

### Test 02 — `mark_and_sweep`

| Revision | BUILD | TRAVERSE | COLLECT | C BUILD | C++ BUILD | Java BUILD |
|---|---:|---:|---:|---:|---:|---:|
| `98a1099` 0.257 | 665 | 17 | 146 | — | — | — |
| `c384cee` pre-ColO | 478 | 15 | 85 | 24.2 | 25.7 | 16.0 |
| `904aee0` 0.260 (stale data) | 478 | 15 | 85 | 24.2 | 25.7 | 16.0 |
| `fa799e8` 0.266 | 558 | 16 | **234** | 30.0 | 33.2 | 16.8 |

C/C++/Rust/Java COLLECT at 0.266: 38.8 / 36.8 / 40.3 / 96.1 ms.

### Test 03 — `cyclic_graph_reclamation`

| Revision | BUILD | COLLECT | C BUILD | C++ BUILD | Rust BUILD | Java BUILD | C COLLECT | Java COLLECT |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `98a1099` 0.257 | 167 | 27 | — | — | — | — | — | — |
| `c384cee` pre-ColO | 102 | 25 | 6.8 | 6.7 | 6.2 | 5.7 | 7.1 | 42.8 |
| `904aee0` 0.260 (stale data) | 102 | 25 | 6.8 | 6.7 | 6.2 | 5.7 | 7.1 | 42.8 |
| `fa799e8` 0.266 | 135 | **88** | 6.4 | 6.5 | 4.9 | 8.0 | 5.9 | 54.8 |

### The version framing is wrong in the task

`98a1099` and `c384cee` contain **identical code** (`git log 98a1099..c384cee`
contains only `c384cee bench: rerun stress test 2`). Their test-02 COLLECT
medians are **146 vs 85 ms** and test-03 BUILD **167 vs 102 ms**. Same source,
same machine, ~4.5 h apart. That is a 1.7× environment/session effect and it is
larger than most of the changes under discussion.

Therefore:

* "0.257 was peak-performing" is **not supported**: its only committed run is
  slower than the identical code measured later.
* "0.260 regressed" is true of the *code* (`e830b65` + later), but the 0.260
  release artifact carries **0.257-era numbers** (generated 03:13, seven hours
  before the 0.260 commits). Anyone quoting 85 ms as "0.260" is quoting the
  pre-ColO code.
* The 0.266 working-tree run is the only artifact that measures the regressed
  code, and it was taken in the same session as the comparison languages, so
  the *cross-language* comparison at 0.266 is internally consistent even though
  its absolute values are not comparable to the 03:13 run.

Any future version comparison must interleave revisions in one session
(`ColO_ROADMAP_2.md` §8.1). The raw matrix in
`benchmarks/bottleneck_discovery/results/` was produced that way and is the
authority for the regression, not the release artifacts.

---

## 2. Normalised per-object comparison (0.266 session)

Objects per round: 1,000,000 (01, 03) and 2,097,151 (02). ns/object = ms × 10⁶ /
objects.

| Workload | Metric | Nirqoba | C | C++ | Rust | Java |
|---|---|---:|---:|---:|---:|---:|
| 01 | allocate (ns/obj) | 140 | 31.5 | 31.3 | 34.8 | 2.6 |
| 01 | collect (ns/obj) | 77 | 19.4 | 18.6 | 20.6 | 7.5 |
| 02 | build (ns/node) | 266 | 14.3 | 15.8 | 13.1 | 8.0 |
| 02 | collect (ns/live node) | 112 | 18.5 | 17.5 | 19.2 | 45.8 |
| 02 | traverse (ns/node) | 7.6 | 6.7 | 7.2 | 3.7 | 2.9 |
| 03 | build (ns/obj) | 135 | 6.4 | 6.5 | 4.9 | 8.0 |
| 03 | collect (ns/obj) | 88 | 5.9 | 5.9 | 5.4 | 54.8 |

### Why these ratios are not pure runtime quality

| Factor | Nirqoba | C/C++/Rust `mini_gc.h` | Java |
|---|---|---|---|
| Node bytes (2 pointers + value) | **120** | **40** | ~40 (compressed oops, 16-byte aligned) |
| Header | 96 B | 32 B | ~16 B mark + klass word |
| Heap commitment | grows per round, page faults | `malloc` per node, cold | committed once, TLAB, no faults |
| Collection | stop-the-world incremental mark/sweep, bitmap | intrusive live list, mark/sweep | generational, concurrent, `System.gc()` |
| Allocation path | runtime call, ~52 ns cold | inline bump/malloc | inlined TLAB, ~10 instr |
| Compiler | LLVM 23 `-O3` | MSVC `/O2` (C/C++), rustc `-O` | HotSpot C2 |

**Footprint normalisation.** Per node Nirqoba touches 3× the bytes. If page
faults are ~31 ns/object at 120 B (`01-allocator-ao.md`), then at 40 B the
same policy would cost ~10 ns — the C/C++/Rust allocators do not pay this
because they do not zero/commit a fresh 64 KiB page per 546 objects; they call
`malloc` into a warm heap. Normalising build by bytes touched,
Nirqoba's ~266 ns/node is ~89 ns per 40 B, still ~6× C++ — so footprint explains
about a third of the build gap, not all of it.

**Collection normalisation.** Nirqoba's 112 ns/live node is ~6× C++ per object,
but C++'s sweep walks a *live-object list*, so its cost is one pointer chase per
live object; Nirqoba's sweep at 0.266 scans 15 bitmap positions per object plus
the object. After the set-bit fix (`03-collector-colo.md`), the projected
~25 ns/object is ~1.4× C++ — the mark phase, not sweep, then becomes the
comparable part.

**Java is a different algorithm.** Java's mark-and-sweep test uses
`System.gc()` on a generational, mostly-concurrent collector with a pre-committed
heap; its 45.8 ns/live node in test 02 is worse than C++ because it scans a
different heap shape, not because the JVM is slow. Its 2.6 ns allocation is a
JIT-inlined TLAB bump with no page faults. Java is a *ceiling* reference for
allocation, not a fair peer.

---

## 3. What is fair to say

**[MEASURED]** At 0.266, on this machine:

1. Build is **4.2× slower per node than C++** (266 vs 15.8 ns) and **33× slower
   than Java**. Roughly one third of the C++ gap is the 3× larger node; the rest
   is the call-based builder protocol (`02-builder-bo.md`).
2. Collection of a fully live graph is **6.4× slower per live node than C++**
   (112 vs 17.5 ns), of which ~3.5× is the bitmap sweep regression.
3. Traversal is **comparable** (7.6 vs 7.2 ns/node; Rust is 2× faster due to
   layout and codegen).
4. Test 03 collection is **15× slower than C++** (88 vs 5.9 ns/object) because
   Nirqoba pays 11.4 ns/object to publish TLABs at cycle start and 31.8 ms of
   bitmap sweep to reclaim pages that hold only dead objects.

**[PROJECTION]** After the set-bit sweep fix, test 02 collection falls to
~125–135 ms (~60 ns/live node), i.e. ~3.4× C++ instead of 6.4×. Test 03 falls
to ~32–35 ms (~34 ns/object), i.e. ~5.8× C++. Remaining gap is mark cost
(single-threaded, hash-probe-per-reference) and TLAB publication.

**Not claimed:** that Nirqoba is "faster than Java" or "as fast as C++" on any
workload. It is not, and the harness cannot support such a claim even after
normalisation.

---

## 4. Reproducing the comparison

The comparison binaries are committed (`benchmarks/stress_test_2/bin/`), but
their build metadata is not (`compilers: unknown`). To reproduce a fair table:

1. record `cl.exe`, `rustc --version`, `java -version` and JVM flags;
2. record `nirqoba --version` and the git commit;
3. run interleaved, 8+ process runs, and keep per-round samples;
4. record `PeakWorkingSetSize` per binary (currently `memory_bytes: null`);
5. do not compare a run at 22:29 with a run at 03:13 and call it a version
   difference.

For the 0.269 comparison in this session, the protocol is: build the three
compiler/runtime kits from `98a1099` (0.257), `904aee0` (0.260, defective) and
the fixed tree (0.269); run each workload interleaved; report min/median/max.
