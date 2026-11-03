# Stress Test 2 - Garbage-Collection Benchmark Report

**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)  
**Date:** 2026-11-04  
**Suite:** `benchmarks/stress_test_2`  
**Status:** Complete; all 45 benchmark processes validated

---

## 1. Executive summary

This report compares Nirqoba's managed heap with C, C++, Rust, and Java on the
three requested GC topics:

1. **Managed-object allocation**
2. **Mark and sweep**
3. **Cyclic graph reclamation**

Fifteen programs were compiled with optimized settings, each process was
repeated three times, and each process produced five measured rounds. Every
checksum, live-object count, collection count, and reclamation check passed.

The central findings are:

- **Nirqoba's mark-sweep collection is in the same order of magnitude as the
  Java control.** On test 02, a full collection of 2,097,151 live tree nodes
  took **328 ms** in Nirqoba versus **108.6 ms** in Java SerialGC (3.0x) and
  **43.0 ms** in the C mini-collector (7.6x). On test 03, reclaiming 500,000
  unreachable cycle pairs took **201 ms** in Nirqoba versus **63.3 ms** in Java
  (3.2x) and **8.3 ms** in the C mini-collector (24.3x).
- **Managed allocation and managed pointer traversal dominate the end-to-end
  gap.** Test 01 allocated one million chain nodes in **1,147 ms** in Nirqoba,
  versus **39.2 ms** in C, **38.6 ms** in C++, **41.2 ms** in Rust, and
  **3.7 ms** in Java. The same chain traversed in **215 ms** in Nirqoba versus
  **7.2 ms** in C and **2.6 ms** in Java.
- **Tree construction is the largest single cost.** Building the 2,097,151-node
  depth-20 tree took **3,527 ms** in Nirqoba, versus **32.6 ms** in C,
  **43.1 ms** in C++, **34.7 ms** in Rust, and **17.5 ms** in Java.
- **The collector is not the main bottleneck in tests 02 and 03.** For the
  full-live tree, one collection cost about 9% of Nirqoba's build time; for the
  cycle graph, one collection cost about 25% of build time. Allocation and
  traversal are the larger optimization targets in this suite.

These results are specific to the recorded host, compiler versions, workload
shapes, and the supplied mini-collectors. They do not claim that Nirqoba is
generally slower or faster than the other languages.

---

## 2. Environment

| Item | Value |
|---|---|
| Host | DESKTOP-VO51V7J |
| Operating system | Microsoft Windows 11 Pro 10.0.26200 |
| CPU | 14th Gen Intel Core i7-14700 (20 cores / 28 logical processors |
| Memory | 34,359,738,368 bytes (29.8 GiB) |
| Python | 3.12.7 |
| Nirqoba | 0.105-beta (Phase 6), LLVM/MLIR 23.1.4, RelWithDebInfo |
| C / C++ | Microsoft C/C++ Optimizing Compiler 19.51.36260 for x64 |
| Rust | rustc 1.99.1 (58cf8f9c3 2026-10-13) |
| Java | OpenJDK 25.0.3 LTS |
| Build flags | Nirqoba `-O3`; MSVC `/O2`; C++ `/std:c++20 /EHsc`; Rust `-C opt-level=3` |
| Java flags | `-Xms2g -Xmx2g -XX:+UseSerialGC` |

The full machine-readable record, including compiler strings and timestamps, is
in `benchmark_results.json`.

---

## 3. Protocol at a glance

- **In-process phase timing.** Allocation/construction, traversal, and
  collection are timed separately inside each process. Process start-up and JVM
  start-up are excluded.
- **Warm-up before measurement.** Tests 01 and 03 use two warm-up rounds; test
  02 uses three warm-up batches of five collections.
- **Five measured rounds per process, three process repeats.** Per-round metrics
  therefore have 15 samples per language. Test 02's build and traversal happen
  once per process, so those two metrics have three samples per language.
- **Medians are the headline statistic.** Minimum, maximum, and sample standard
  deviation are shown with every median. Raw per-run data is retained.
- **Validation.** A run is accepted only if its topic name, implementation name,
  object count, round count, checksum, live-object count, and collection count
  match the specification.
- **Fairness.** C, C++, and Rust use an embedded precise, non-moving
  mark-sweep collector for tests 02 and 03 so they are compared against the same
  algorithm as Nirqoba, not against `free()` or `Drop`. Java uses its closest
  stop-the-world configuration, `SerialGC`.

The complete rationale and limitations are in `METHODOLOGY.md`.

---

## 4. Results

### 4.1 Test 01 - Managed-object allocation

**Workload:** 1,000,000-node singly linked chain.  
**Phases:** build, checksum traversal, reclamation.  
**Checksum:** `499999500000` in every accepted run.

| Language | Allocation (ms) | Traversal (ms) | Reclamation (ms) | Allocation throughput |
|---|---:|---:|---:|---:|
| C | 39.210 ± 1.970 [33.400-41.409] | 7.177 ± 0.540 [6.421-8.559] | 22.903 ± 2.415 [20.509-29.298] | 25.50 M nodes/s |
| C++ | 38.552 ± 1.750 [33.840-41.187] | 6.979 ± 0.436 [6.409-7.747] | 22.627 ± 0.890 [21.052-23.934] | 25.94 M nodes/s |
| Rust | 41.203 ± 3.361 [35.022-45.864] | 7.057 ± 0.502 [6.388-8.190] | 24.083 ± 2.277 [20.192-29.068] | 24.27 M nodes/s |
| Java | 3.663 ± 0.956 [2.636-5.617] | 2.605 ± 0.686 [2.069-4.423] | 8.441 ± 0.845 [7.761-10.197] | 273.00 M nodes/s |
| **Nirqoba** | **1,147.000 ± 336.177 [1,117.000-2,371.000]** | **215.000 ± 82.814 [205.000-498.000]** | **221.000 ± 91.175 [203.000-530.000]** | **0.87 M nodes/s** |

Values are median ± sample standard deviation [minimum-maximum], 15 samples per
per-round metric. The reclamation column is not algorithm-equivalent: C, C++,
and Rust release the chain deterministically with `free`, `delete`, and `Drop`,
while Nirqoba and Java perform tracing collections.

#### Analysis

- **Allocation.** C, C++, and Rust cluster tightly at 38.6-41.2 ms, about
  24-26 million nodes per second. Java is an order of magnitude faster at
  3.66 ms (273 million nodes/s) because HotSpot allocates from thread-local
  bump-pointer regions and the warmed JIT generates very tight code. Nirqoba's
  `array.allocate<Node>(1)` path takes 1,147 ms (0.87 million nodes/s), about
  **29x** the native C/C++/Rust time and **313x** the Java time.
- **Traversal.** The same pointer-chasing loop takes 7.0-7.2 ms in C, C++, and
  Rust, 2.6 ms in Java, and 215 ms in Nirqoba. The Java advantage is consistent
  with contiguous TLAB placement and JIT-optimized pointer chasing. Nirqoba is
  about **30x** slower than the native tracks and **83x** slower than Java.
- **Reclamation.** C, C++, and Rust spend 22.6-24.1 ms walking the chain and
  freeing each node. Java spends 8.44 ms on a full collection of a chain with no
  surviving objects. Nirqoba spends 221 ms on an explicit tracing collection of
  one million dead managed objects. This column should be read as each
  language's native reclamation cost, not as an equal-algorithm GC comparison.
- **Dispersion.** One Nirqoba process repeat contained a disturbed stretch:
  one round reported 2,371 ms allocation, 498 ms traversal, and 530 ms
  collection. The median is unaffected, and the outlier is retained in the raw
  data rather than deleted. C, C++, Rust, and Java were comparatively stable.

### 4.2 Test 02 - Mark and sweep

**Workload:** complete binary tree, depth 20, 2,097,151 reachable nodes.  
**Phases:** one-time build, one-time checksum traversal, then five measured
batches of five full collections.  
**Checksum:** `2199022206976` in every accepted run.  
**Validation:** every post-collection count was 2,097,151 and every sample
reported exactly five collections.

| Language | Build (ms) | Traversal (ms) | Collection per GC (ms) | Collection throughput |
|---|---:|---:|---:|---:|
| C | 32.602 ± 6.932 [26.761-40.571] | 14.408 ± 1.041 [13.322-15.403] | 42.996 ± 5.148 [39.111-57.011] | 48.78 M nodes/s |
| C++ | 43.118 ± 11.089 [40.695-60.999] | 18.437 ± 4.178 [16.711-24.655] | 44.539 ± 13.429 [40.112-85.048] | 47.09 M nodes/s |
| Rust | 34.684 ± 1.052 [32.977-34.895] | 8.532 ± 0.407 [7.977-8.771] | 42.501 ± 2.945 [39.375-50.341] | 49.34 M nodes/s |
| Java | 17.524 ± 2.996 [15.505-21.401] | 6.435 ± 0.936 [5.682-7.542] | 108.579 ± 17.356 [84.577-143.296] | 19.32 M nodes/s |
| **Nirqoba** | **3,527.000 ± 513.594 [2,694.000-3,631.000]** | **2,002.000 ± 627.237 [1,760.000-2,947.000]** | **328.000 ± 24.795 [317.000-405.000]** | **6.39 M nodes/s** |

Build and traversal have three samples per language (one per process repeat).
Collection has 15 samples. Collection throughput counts all 2,097,151 nodes as
retained live objects.

#### Analysis

- **Build.** Nirqoba needs 3,527 ms to allocate and link 2.1 million managed
  tree nodes. The C/C++/Rust mini-collectors build the same tree in
  32.6-43.1 ms, and Java builds it in 17.5 ms. Nirqoba is about **108x** the C
  time and **201x** the Java time. The recursive tree workload performs two
  managed-field stores per internal node, so both allocation and reference-store
  costs contribute.
- **Traversal.** The checksum recursion takes 8.5-18.4 ms in the native tracks,
  6.4 ms in Java, and 2,002 ms in Nirqoba. Nirqoba is about **139x** the C time
  and **311x** the Java time. The gap against C is the largest in the suite;
  against Java it is essentially tied with the allocation gap. This points at
  managed pointer/reference access cost rather than at the collector.
- **Collection.** A full mark-and-sweep of 2.1 million live nodes takes 43.0 ms
  in C, 44.5 ms in C++, 42.5 ms in Rust, 108.6 ms in Java, and 328 ms in
  Nirqoba. The C/C++/Rust times are close to each other, which is expected from
  three implementations of the same algorithm. Java's SerialGC is generational
  and compacts, so it is not algorithm-identical. Nirqoba's non-moving sweep is
  **7.6x** the C mini-collector and **3.0x** Java SerialGC. The collector gap is
  much narrower than the allocation and traversal gaps.
- **Implication.** For a workload dominated by live data and repeated full
  collections, the collector itself is not the dominant end-to-end cost in
  Nirqoba. Build plus traversal cost 5,529 ms once, while one collection costs
  328 ms; even a full five-collection sample (about 1,640 ms) is less than a
  third of that one-time build-and-traverse cost.

### 4.3 Test 03 - Cyclic graph reclamation

**Workload:** 500,000 two-node cycles, 1,000,000 nodes total, no external root.  
**Phases:** build, then one collection per round.  
**Checksum:** `499999500000` in every accepted run.  
**Validation:** Nirqoba and C/C++/Rust ended with zero live objects; Java
reported 500,000 cleared weak references in every round.

| Language | Build (ms) | Collection (ms) | Collection throughput | Cycles cleared |
|---|---:|---:|---:|---:|
| C | 7.861 ± 1.469 [7.132-13.072] | 8.288 ± 2.605 [5.598-16.322] | 60.33 M pairs/s | 500,000 by live count |
| C++ | 6.114 ± 0.939 [5.057-8.051] | 5.351 ± 1.086 [4.845-9.026] | 93.44 M pairs/s | 500,000 by live count |
| Rust | 6.663 ± 0.781 [5.015-7.348] | 7.427 ± 1.220 [4.751-8.216] | 67.32 M pairs/s | 500,000 by live count |
| Java | 8.164 ± 1.698 [6.947-13.030] | 63.321 ± 7.747 [55.497-84.553] | 7.90 M pairs/s | 500,000 weak refs |
| **Nirqoba** | **803.000 ± 144.041 [620.000-1,193.000]** | **201.000 ± 42.616 [168.000-301.000]** | **2.49 M pairs/s** | **500,000** |

All 15 samples per language for build and collection.

#### Analysis

- **Build.** Creating one million nodes in 500,000 isolated two-node cycles
  takes 6.1-7.9 ms in C, C++, and Rust, 8.2 ms in Java, and 803 ms in Nirqoba.
  Nirqoba is about **102x** the C time and **98x** the Java time. This is
  consistent with the allocation cost observed in test 01.
- **Collection.** Reclaiming all 500,000 unreachable cycles takes 5.4-8.3 ms in
  the C/C++/Rust mini-collectors, 63.3 ms in Java, and 201 ms in Nirqoba.
  Nirqoba is **24.3x** the C mini-collector and **3.2x** Java. The C/C++/Rust
  collectors start from a null root and sweep one million objects; Nirqoba also
  traces from an empty root set and sweeps its managed heap.
- **Java observation cost.** Java's 63.3 ms includes processing 500,000
  `WeakReference` objects plus the live reference array. The other tracks report
  a live-object counter. This makes Java's collection time an upper bound
  relative to a Java run that only needed to reclaim the cycles.
- **Cycle correctness.** A reference-counting collector without cycle detection
  would leave all 500,000 pairs alive. Every accepted run proved full
  reclamation: Nirqoba and the native mini-collectors ended at zero live
  objects, and Java cleared every weak reference.

---

## 5. Relative comparison

The table below divides Nirqoba's median by the C and Java medians. A value of
1.0 means equal; larger means Nirqoba took longer.

| Topic / phase | Nirqoba (ms) | C (ms) | Java (ms) | vs C | vs Java |
|---|---:|---:|---:|---:|---:|
| 01 Allocation | 1,147.000 | 39.210 | 3.663 | 29.3x | 313.1x |
| 01 Traversal | 215.000 | 7.177 | 2.605 | 30.0x | 82.5x |
| 01 Reclamation | 221.000 | 22.903 | 8.441 | 9.7x | 26.2x |
| 02 Build | 3,527.000 | 32.602 | 17.524 | 108.2x | 201.3x |
| 02 Traversal | 2,002.000 | 14.408 | 6.435 | 139.0x | 311.1x |
| 02 Collection | 328.000 | 42.996 | 108.579 | 7.6x | 3.0x |
| 03 Build | 803.000 | 7.861 | 8.164 | 102.2x | 98.4x |
| 03 Collection | 201.000 | 8.288 | 63.321 | 24.3x | 3.2x |

C++ and Rust track C closely on the algorithm-equivalent phases (within roughly
1.1x of C in most cases), so the C column is a reasonable representative for the
native mini-collector tracks. The full C++ and Rust values are in section 4.

---

## 6. Discussion

### 6.1 The collector is the closest part of the comparison

Across the two collection-focused tests, Nirqoba's non-moving mark-sweep is
3.0x and 3.2x slower than Java's stop-the-world SerialGC. Given that Java is a
mature, generational, compacting collector and Nirqoba is a 0.105-beta runtime,
this is a much narrower gap than the allocation or traversal results. The
C/C++/Rust mini-collectors are faster still, but they are small, workload-tuned
implementations with a single root slot and no general runtime integration.

### 6.2 Allocation is the largest headroom

The per-node managed allocation path is the dominant cost in all three tests:

- 1,147 ms for one million chain nodes,
- 3,527 ms for 2.1 million tree nodes,
- 803 ms for one million cycle nodes.

The native baselines allocate the same objects in tens of milliseconds, and Java
does so in single-digit or low-double-digit milliseconds. The exact cause cannot
be isolated from these black-box measurements, but the result is consistent
across three different graph shapes, so it is not a one-workload artifact.
Likely areas for future investigation include allocation metadata and
bookkeeping, one-element managed-array overhead, and root-frame management.
Those are hypotheses for profiling, not conclusions from this suite.

### 6.3 Managed traversal is the second-largest headroom

The traversal gaps (30x versus C on the chain, 139x versus C on the tree) are
larger than the collection gaps and cannot be explained by the collector.
They indicate that loading a managed reference and following it is substantially
more expensive in the generated Nirqoba code than a native pointer load. This is
a separate optimization target from the allocator.

### 6.4 What these results do not show

- They do not measure multi-threaded or concurrent collection.
- They do not measure short-lived allocation bursts with automatic collection
  enabled. Nirqoba runs with `gc.disable()` in this suite to isolate phases.
- They do not compare against a production C/C++/Rust GC, because those
  languages do not ship one. The native GC numbers describe the supplied
  mini-collectors.
- They do not isolate CPU frequency, cache layout, or operating-system
  scheduling effects. The host was not pinned or frequency-locked.
- They do not establish general language performance. The workload is
  deliberately GC-heavy and synthetic.

---

## 7. Conclusion

On this host, with these workloads and build flags:

1. Nirqoba's explicit mark-sweep collection is within about **3x** of Java's
   full SerialGC for a large live tree and for unreachable cycles. It is about
   **7.6x** to **24.3x** slower than the compact C/C++/Rust mini-collector,
   depending on how much marking work is required.
2. Nirqoba's managed allocation and managed pointer traversal are the dominant
   costs. Allocation is **29x** the native C chain time and **313x** the Java
   chain time; tree traversal is **139x** the C time and **311x** the Java time.
3. The suite validates correctness, not just speed: every run produced the
   expected checksum, retained every live tree node, reclaimed every cycle, and
   executed the requested number of collections.

The most useful next step is not to tune the mark-sweep loop first, but to
profile the managed allocation and reference-access paths. The collector is
already close enough to the Java control that allocation and traversal dominate
the user-visible cost in these tests.

---

## 8. Reproduction

From `benchmarks/stress_test_2`:

```powershell
python run_benchmark.py --repeats 3
```

Single-topic or single-language runs:

```powershell
python run_benchmark.py --test 01
python run_benchmark.py --test 02 --impl nirqoba
python run_benchmark.py --skip-compile
```

Artifacts:

- `benchmark_results.json` - all samples, medians, dispersion, compiler metadata.
- `results/raw/` - captured stdout and exit status for all 45 benchmark
  processes.
- `METHODOLOGY.md` - detailed design, fairness controls, and limitations.
- `README.md` - layout and quick-start instructions.

---

## Appendix A - Sample counts

| Metric class | Samples per language |
|---|---:|
| Test 01 allocation, traversal, reclamation | 15 (3 processes x 5 rounds) |
| Test 02 build and traversal | 3 (one per process) |
| Test 02 collection | 15 (3 processes x 5 rounds) |
| Test 03 build and collection | 15 (3 processes x 5 rounds) |

## Appendix B - Validation summary

- 15 benchmark tracks (3 topics x 5 implementations).
- 45 benchmark processes (15 tracks x 3 repeats).
- 225 measured benchmark rounds retained (480 per-metric samples).
- All processes exited with status 0.
- All checksums matched the specified values.
- All test 01 runs reported the expected post-collection state.
- All test 02 runs retained 2,097,151 live objects and executed exactly five
  collections per sample.
- All test 03 runs reclaimed 500,000 cycles and ended with zero live objects
  (Java: 500,000 cleared weak references).
- No measurement was fabricated, synthesized, or copied from another run.
