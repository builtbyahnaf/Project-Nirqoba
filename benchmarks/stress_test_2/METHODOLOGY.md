# Stress Test 2 - Methodology

**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

This document records how the garbage-collection benchmarks were designed,
executed, validated, and interpreted. The goal is a comparison that is
reproducible and honest about what each language track actually measures.

## 1. Research questions

The suite asks three narrow questions:

1. **Managed-object allocation:** what does it cost to create one million
   managed nodes and link them into a chain?
2. **Mark and sweep:** what does a full tracing collection cost when every node
   in a large tree is reachable and nothing may be reclaimed?
3. **Cyclic graph reclamation:** can a collector reclaim one million nodes that
   exist only in 500,000 two-node cycles, and what does that collection cost?

The suite does not attempt to measure compiler throughput, JIT quality in
general, or multi-threaded GC. Every benchmark is single-threaded and
deterministic.

## 2. Workloads

| Test | Graph | Object count | Structure | Expected checksum |
|---|---|---|---:|---|
| 01 | Singly linked chain | 1,000,000 | `value = index`, `next` links | `499999500000` |
| 02 | Complete binary tree, depth 20 | 2,097,151 | two child references per node | `2199022206976` |
| 03 | 500,000 two-node cycles | 1,000,000 | each node points only at its partner | `499999500000` |

All object counts and checksums are identical across languages. A run is
rejected if any checksum differs.

### Test 01 phases

1. Build the chain and time only the build.
2. Traverse the chain and sum `value`; time only the traversal.
3. Drop the root and time reclamation.

Nirqoba runs with automatic collection disabled (`gc.disable()`), so only the
explicit `gc.collect()` at the end of the round can reclaim the chain. C, C++,
and Rust time their deterministic release (`free`, `delete`, `Drop`). Java clears
its static sink and times `System.gc()`.

### Test 02 phases

1. Build the tree once and time the build.
2. Traverse the tree once for the checksum and time the traversal.
3. Run three warm-up batches of five collections each.
4. Run five measured batches of five collections each; report the batch average.

The root remains a live managed local and is read again after the measured
batches. That use prevents the compiler from treating the tree as dead before
the collections and accidentally turning the test into a reclamation benchmark.
Every collection must retain all 2,097,151 objects.

### Test 03 phases

1. Two warm-up rounds: build 500,000 cycles and collect them.
2. For each measured round: time the build, then time one collection.
3. Verify that the live-object count falls to zero (Nirqoba and C/C++/Rust) or
   that every `WeakReference` is cleared (Java).

The cycles have no external root. A correct tracing collector must reclaim them;
a reference-counting collector without cycle detection would not.

## 3. Language tracks

### Nirqoba

- Compiler: `nirqoba build <source> -o <exe> -O3`.
- Managed nodes use `managed<Node>` fields inside a struct.
- Each node is allocated with `array.allocate<Node>(1)`, the documented
  managed-allocation API for the current language version.
- `gc.disable()` is called at start-up. Collections are explicit
  `gc.collect()` calls, which keeps allocation, traversal, and collection in
  separate phases.
- `gc.get_statistics()` supplies `live_objects` and `collections`.
- Helper functions (`make_node`, `make_one_cycle`) keep managed locals in
  short-lived frames. Declaring managed locals directly inside the million-
  iteration loops grows the caller's root frame and is not representative of
  normal Nirqoba code.
- `time.now()` has one-millisecond resolution. All measured phases are larger
  than 100 ms, and test 02 batches five collections per sample, so the timer
  resolution is a small fraction of every reported value.

### C

- Compiler: MSVC 19.51, `cl /O2 /nologo`.
- Test 01 uses one `malloc` per node and `free` for release. This is the
  no-GC native baseline.
- Tests 02 and 03 use `c/mini_gc.h`, a precise non-moving mark-sweep collector
  with:
  - an object header containing a trace callback, size, mark bit, all-objects
    link, and free-list link;
  - explicit root slots;
  - a worklist mark phase over precise reference fields;
  - a linear sweep that recycles unmarked objects into a size-uniform free list;
  - bump allocation inside large blocks.
- The collector is embedded in the benchmark so C is not compared to a
  non-existent standard GC. Its performance describes this supplied collector,
  not a production C runtime.

### C++

- Compiler: MSVC 19.51, `cl /O2 /nologo /std:c++20 /EHsc`.
- Test 01 uses `new`/`delete` for the native baseline.
- Tests 02 and 03 use `cpp/mini_gc.hpp`, the C++20 counterpart of the C
  collector. It has the same header layout, trace contract, worklist mark phase,
  all-objects list, free list, and block allocator. The worklist uses
  `std::vector`.

### Rust

- Compiler: `rustc -C opt-level=3` (rustc 1.99.1).
- Test 01 uses `Box<Node>` for the native baseline.
- Tests 02 and 03 use `rust/mini_gc.rs`, the Rust counterpart of the C
  collector. Allocation uses `std::alloc` inside large aligned blocks; marking
  uses a `Vec<*mut Object>` worklist; sweeping uses the same all-objects list
  and free list.
- The collector is `unsafe` by necessity because it implements raw tracing, but
  the benchmark's object graph and trace callbacks are precise.

### Java

- JDK: OpenJDK 25.0.3 LTS.
- Flags: `-Xms2g -Xmx2g -XX:+UseSerialGC`.
- Test 01 uses one `Node` object per element. A static `sink` field holds the
  chain during construction and is cleared before collection. Without the sink,
  HotSpot's escape analysis could scalar-replace the chain and remove the
  allocation work.
- Tests 02 and 03 force full collections with `System.gc()`. A
  `GarbageCollectorMXBean` counter is read before and after each batch; the run
  is rejected unless the counter increases by exactly the batch size.
- Test 03 stores a `WeakReference` for every pair and counts cleared references.
  This proves reclamation but adds 500,000 weak-reference objects and an array
  that the other tracks do not allocate. The extra work is a Java-side
  observation cost and is discussed in the limitations.

## 4. Timing protocol

- All timings are taken **inside the benchmark process**. Process start-up,
  JVM start-up, dynamic-loader work, compilation, and shell overhead are not
  part of any reported phase.
- C, C++, and Java use monotonic high-resolution clocks
  (`QueryPerformanceCounter`, `std::chrono::steady_clock`, `System.nanoTime`).
- Nirqoba uses `time.now()`, which reports whole milliseconds since the Unix
  epoch. It is not monotonic, but no phase crosses a system clock adjustment in
  these runs.
- Warm-up work is performed before the measured rounds and is not recorded.
- Each process produces five measured rounds. The runner repeats each process
  three times, giving 15 rounds per language/topic for per-round metrics. For
  test 02, `BUILD_MS` and `TRAVERSE_MS` occur once per process, giving three
  samples.
- Reported statistics are median, mean, minimum, maximum, sample standard
  deviation, and sample count. Medians are used for the headline comparison
  because they are resistant to isolated host interference.

## 5. Fairness controls

The following controls keep the comparison as close as the languages allow:

1. **Same graph sizes and shapes** in every language.
2. **Same checksums**, computed by traversing the built structure.
3. **Same phase boundaries**: allocation/construction, traversal, collection.
4. **Optimized builds** for every track (`-O3`, `/O2`, `opt-level=3`).
5. **Single-threaded execution** everywhere.
6. **No I/O inside timed regions.** Output is emitted after the phases.
7. **Equal GC algorithm for C, C++, Rust, and Nirqoba** in tests 02 and 03:
   precise, non-moving, stop-the-world mark-sweep with explicit roots.
8. **Manual collection control** in every track: no automatic trigger can fire
   in the middle of a measured phase. Nirqoba disables automatic GC; the
   C/C++/Rust collectors run only when called; Java uses explicit full GCs.
9. **Root-liveness guarantee** in test 02, so the tree cannot be optimized away.
10. **Escape-analysis guard** in Java test 01.
11. **Collection-count verification** in test 02, so a JVM no-op cannot be
    mistaken for a fast collection.
12. **Reclamation verification** in test 03, so a fast run that leaks cycles is
    rejected.
13. **Three process repeats**, with every measured round retained in
    `benchmark_results.json`.

## 6. Validation

`run_benchmark.py` checks every run before accepting it:

| Check | Test 01 | Test 02 | Test 03 |
|---|---|---|---|
| `TEST` name | yes | yes | yes |
| `IMPL` name | yes | yes | yes |
| `OBJECTS` count | 1,000,000 | 2,097,151 | 1,000,000 |
| Measured rounds | 5 | 5 | 5 |
| Checksum | per round | once per process | per round |
| Positive phase times | yes | yes | yes |
| Live objects after collection | 0 (Java: `-1`, not exposed) | 2,097,151 | 0 |
| Collection count | — | exactly 5 per sample | — |
| Cycles cleared | — | — | 500,000 where reported |

A failed compile, non-zero exit, missing field, wrong checksum, wrong live-object
count, or wrong collection count causes the runner to report the failure and
exit with status `1`.

## 7. Statistics

For each topic and language:

- collect every measured round from every process repeat;
- compute the median, mean, minimum, maximum, and sample standard deviation for
  each phase;
- retain the raw per-run values in `benchmark_results.json` and the complete
  process output in `results/raw/`.

The reported median therefore represents 15 samples for per-round metrics and
three samples for test 02's build/traverse metrics.

## 8. Limitations and threats to validity

1. **The C, C++, and Rust GC results are not standard-runtime results.** Those
   languages do not ship a tracing collector. Tests 02 and 03 use the embedded
   `mini_gc` implementations supplied here. They are useful for algorithm
   parity, but they are not mature production collectors and their tuning
   affects the comparison.
2. **Test 01 is not algorithm-equivalent for reclamation.** C, C++, and Rust
   free a chain deterministically; Nirqoba and Java trace and collect. The
   allocation and traversal columns are comparable managed-versus-native costs,
   while the collection column measures each language's native reclamation path.
3. **Java's SerialGC is generational.** It uses a copying young generation and a
   mark-sweep-compact old generation, whereas Nirqoba and the mini collectors are
   non-moving mark-sweep. `-XX:+UseSerialGC` is the closest stop-the-world
   configuration in HotSpot, not an identical algorithm.
4. **Java test 03 pays an observation cost.** Every pair has a `WeakReference`
   and the array itself is live during collection. The other tracks observe
   reclamation through a live-object counter. This biases Java's test 03
   collection time upward.
5. **Nirqoba's timer is one millisecond.** Phases are large enough that the
   resolution is below one percent of the reported value, but the Nirqoba
   numbers are coarser than the microsecond-resolution tracks. Test 02 uses a
   five-collection batch for the same reason.
6. **JIT and process-level variance.** Java's first rounds include JIT
   compilation and class loading even after the benchmark's internal warm-up.
   One Nirqoba test 01 process repeat showed a clearly disturbed stretch
   (2,371 ms allocation in one round). All samples are retained and the median
   is reported, so the disturbance is visible rather than hidden.
7. **No CPU pinning or frequency locking.** The host is a laptop-class Intel
   Core i5-1235U. Background activity, thermal state, and turbo frequency affect
   absolute times. Comparisons within this run are more reliable than
   comparisons to numbers recorded at another time.
8. **Absolute portability.** These results describe the recorded Windows 11
   host, toolchain versions, and build flags. They are not a general claim about
   every Nirqoba, C, C++, Rust, or Java program.

## 9. Reproduction

From `benchmarks/stress_test_2`:

```powershell
python run_benchmark.py --repeats 3
```

To reproduce a single topic or language:

```powershell
python run_benchmark.py --test 02
python run_benchmark.py --test 03 --impl nirqoba
```

The exact compiler and runtime versions, host description, timestamps, and raw
per-run outputs are stored with the results. Do not compare a fresh run's
absolute times to the recorded run without accounting for machine state and
system load.
