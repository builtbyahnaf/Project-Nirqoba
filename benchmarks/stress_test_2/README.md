# Stress Test 2 - Garbage-Collection Benchmarks

**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

This directory contains a controlled, reproducible comparison of Nirqoba's
managed heap against C, C++, Rust, and Java on three garbage-collection topics:

1. **Managed-object allocation**
2. **Mark and sweep**
3. **Cyclic graph reclamation**

The measured numbers are in [`BENCHMARK_REPORT.md`](BENCHMARK_REPORT.md). The
fairness rules, timing protocol, validation, and limitations are documented in
[`METHODOLOGY.md`](METHODOLOGY.md). Machine-readable results are in
[`benchmark_results.json`](benchmark_results.json).

## Test matrix

| Topic | Nirqoba | C | C++ | Rust | Java |
|---|---|---|---|---|---|
| Managed-object allocation | `01_managed_object_allocation.nrqba` | `01_managed_object_allocation.c` | `01_managed_object_allocation.cpp` | `01_managed_object_allocation.rs` | `Benchmark01Allocation.java` |
| Mark and sweep | `02_mark_and_sweep.nrqba` | `02_mark_and_sweep.c` | `02_mark_and_sweep.cpp` | `02_mark_and_sweep.rs` | `Benchmark02MarkSweep.java` |
| Cyclic graph reclamation | `03_cyclic_graph_reclamation.nrqba` | `03_cyclic_graph_reclamation.c` | `03_cyclic_graph_reclamation.cpp` | `03_cyclic_graph_reclamation.rs` | `Benchmark03Cycles.java` |

Every track measures the same phases **inside the process**:

- allocation or construction time,
- graph/tree/chain traversal time where applicable,
- reclamation or collection time,
- a deterministic value checksum,
- a live-object or reclamation check.

Process start-up, JVM start-up, compilation, and shell overhead are excluded.

## Directory layout

```text
benchmarks/stress_test_2/
├── BENCHMARK_REPORT.md          # results and analysis
├── METHODOLOGY.md               # fairness, timing, validation, limitations
├── README.md                    # this file
├── run_benchmark.py             # compile + run + validate + summarise
├── benchmark_results.json       # latest machine-readable results
├── bin/                         # generated executables and Java classes
├── c/                           # C track and embedded mini mark-sweep collector
├── cpp/                         # C++20 track and embedded mini collector
├── java/                        # Java track (JDK 25, SerialGC)
├── nirqoba/                     # Nirqoba track
├── results/raw/                 # captured output of every process repeat
└── rust/                        # Rust track and embedded mini collector
```

## Prerequisites

The runner expects the toolchains used for the recorded results:

| Tool | Expected location / command |
|---|---|
| Nirqoba | `build/msvc-dev/bin/nirqoba.exe` |
| MSVC | Visual Studio Build Tools with `vcvars64.bat` |
| Rust | `C:\Users\USER\.cargo\bin\rustc.exe` |
| Java | JDK 25 at `C:\Users\USER\.gradle\jdks\eclipse_adoptium-25-amd64-windows.2` |
| Python | 3.10 or newer |

Paths are defined at the top of `run_benchmark.py`. Change them there if your
installation differs.

## Quick start

From this directory:

```powershell
python run_benchmark.py
```

The default run compiles every track, executes each benchmark process three
times, and writes `benchmark_results.json` plus raw logs under `results/raw/`.

Common options:

```powershell
# Re-run measurements without recompiling
python run_benchmark.py --skip-compile

# Run one topic
python run_benchmark.py --test 02

# Run one language on one topic
python run_benchmark.py --test 01 --impl nirqoba

# Use five process repeats instead of three
python run_benchmark.py --repeats 5
```

The runner exits with code `0` only when every required checksum, live-object
count, collection count, and phase field validates.

## What the tests do

### 01 - Managed-object allocation

Builds a chain of 1,000,000 nodes, traverses it for a checksum, then reclaims
it. Nirqoba allocates one managed `Node` at a time with
`array.allocate<Node>(1)`. C, C++, and Rust use `malloc`, `new`, and `Box` as a
no-GC native baseline. Java uses one `Node` object per element and a static sink
to prevent escape analysis from removing the chain. The expected checksum is
`499999500000`.

### 02 - Mark and sweep

Builds a complete binary tree of depth 20 (2,097,151 nodes) and keeps every node
reachable from one root. Each sample is a batch of five full collections, so the
whole tree is marked and the object list is swept while nothing is reclaimed.
Nirqoba calls `gc.collect()` explicitly with automatic collection disabled. The
C, C++, and Rust tracks use the supplied precise mark-sweep collector in
`mini_gc.h`, `mini_gc.hpp`, and `mini_gc.rs`. Java uses `-XX:+UseSerialGC` and
`System.gc()`. The expected checksum is `2199022206976`.

### 03 - Cyclic graph reclamation

Creates 500,000 two-node reference cycles (1,000,000 nodes) with no external
root. A tracing collector must reclaim the entire set; reference counting alone
would leak it. Nirqoba and the C/C++/Rust mini collectors report the live-object
count after collection. Java uses `WeakReference` entries to prove that every
cycle was cleared. The expected checksum is `499999500000`.

## Output protocol

Each program emits `KEY value` lines. Nirqoba emits the key and value on
separate lines because its `print` statement takes one value at a time; the
runner accepts both forms.

Common keys:

```text
TEST <topic>
IMPL <language>
OBJECTS <count>
WARMUP_ROUNDS <count> | WARMUP_BATCHES <count>
COLLECT_BATCH <count>
MEASURE_ROUNDS <count>
ROUND <index>
ALLOC_MS <ms> | BUILD_MS <ms>
TRAVERSE_MS <ms>
COLLECT_MS <ms>
CHECKSUM <value>
LIVE_AFTER <count>
COLLECTIONS_DELTA <count>
CYCLES_CLEARED <count>
```

## Notes on interpretation

- C, C++, and Rust have no built-in tracing collector. Tests 02 and 03 therefore
  use a small embedded precise mark-and-sweep collector so the comparison is
  against the same algorithm, not against `free()` or `Drop`. Those numbers
  describe the supplied collector, not a standard-library runtime.
- Test 01 deliberately uses native `malloc`/`new`/`Box` as a no-GC lower bound.
  It is not an algorithm-equivalent GC comparison.
- Java's SerialGC is generational (mark-copy young generation, mark-sweep-compact
  old generation), not a pure non-moving mark-sweep collector. It is the closest
  stop-the-world analogue available in HotSpot.
- Absolute timings are specific to the host, compiler versions, and system load
  recorded in `benchmark_results.json`. Use the medians and dispersion together;
  do not treat small differences as meaningful.
