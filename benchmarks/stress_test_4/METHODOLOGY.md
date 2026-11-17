# Nirqoba Stress Test 4 — Methodology

**Suite:** `benchmarks/stress_test_4`  
**Purpose:** Standardized evaluation methodology for cross-language performance comparison across Nirqoba, C, C++, Rust, Java, JavaScript, and Python.

---

## 1. Principles & Objectives

Stress Test 4 is designed to evaluate Nirqoba against six major programming languages under strictly controlled, idiomatic, and reproducible conditions. The benchmark suite adheres to the following principles:

1. **Idiomatic implementations:** Every workload uses the natural, standard paradigms of each language without artificial handicaps or synthetic optimizations.
2. **Deterministic execution:** All random inputs, arrays, and graphs are generated from fixed seeds to guarantee bitwise or mathematically identical inputs across all languages.
3. **No dead-code elimination:** All operations accumulate a result or checksum validated by assertions.
4. **Isolated execution timing:** Process bootstrap, JVM/V8 initialization, disk I/O, and dataset generation are excluded from the kernel execution measurements where specified.
5. **No misleading aggregate single-score:** Results are presented per workload with clear metrics, normalized speedups, and explicit disclosure of limitations.

---

## 2. Memory-Management Rules by Language

| Language | Primary Memory Paradigm | Specific Rules for Stress Test 4 |
|---|---|---|
| **Nirqoba** | Deterministic ownership & linear types | Deterministic scope-based reclamation by default. Tracing GC enabled explicitly in GC-specific workloads (ST4-16). |
| **C** | Manual dynamic allocation & stack | Idiomatic `malloc` and `free` or stack allocation. Standard C memory layout. |
| **C++** | RAII & smart pointers | Standard library containers (`std::vector`, `std::unordered_map`), `std::unique_ptr` / `std::shared_ptr`. |
| **Rust** | Ownership, borrowing, & `Box` | Safe Rust by default. Standard containers (`Vec`, `HashMap`, `Box<T>`), explicit `Drop`. |
| **Java** | JVM-managed garbage-collected heap | Idiomatic object instantiation (`new`), collections (`ArrayList`, `HashMap`), HotSpot standard GC. |
| **JavaScript** | V8-managed garbage-collected heap | Standard Node.js objects, typed arrays where idiomatic, standard `Map` and `Array`. |
| **Python** | Reference counting + cyclic GC | Standard CPython objects, built-in `list`, `dict`, standard library collections. |

---

## 3. Workload Specifications (ST4-01 through ST4-16)

### Category A: CPU Computation

#### ST4-01 — Integer Arithmetic Throughput
- **Workload:** 500 million deterministic integer operations.
- **Operations:** Mixed addition, subtraction, 64-bit integer multiplication, integer division, bitwise shifts, and XOR.
- **Anti-Optimization:** Cumulative 64-bit XOR-checksum returned and validated.
- **Metric:** Median execution time (ms) and throughput (Mops/s).

#### ST4-02 — Floating-Point Throughput
- **Workload:** 100 million deterministic 64-bit floating-point (`f64` / `double`) operations.
- **Operations:** Addition, multiplication, division, square root (`sqrt`), and polynomial accumulation.
- **Correctness:** Numerical tolerance $\epsilon < 10^{-7}$ against reference double-precision checksum.
- **Metric:** Execution time (ms) and MFLOPS/s.

#### ST4-03 — Branch-Heavy Computation
- **Workload:** 100 million pseudo-random branch decisions (50% predictable, 50% unpredictable pattern generated via LCG).
- **Operations:** Multi-way conditional branches, state machine updates, and bounds validations.
- **Anti-Optimization:** Final state accumulator checksum.
- **Metric:** Execution time (ms) and decisions/s.

#### ST4-04 — Matrix Multiplication
- **Workload:** Dense matrix multiplication of two $1024 \times 1024$ double-precision matrices ($C = A \times B$).
- **Algorithm:** Canonical 3-loop conventional dense matrix multiplication without external BLAS or GPU libraries.
- **Correctness:** Frobenius norm checksum with tolerance $\epsilon < 10^{-6}$.
- **Metric:** Kernel time (ms) and GFLOPS.

---

### Category B: Data Structures and Algorithms

#### ST4-05 — Vector Construction and Reduction
- **Workload:** Allocate and populate a dynamic array containing 50,000,000 64-bit integers; sequential traversal and reduction (sum).
- **Measurement:** Construction time and reduction traversal time measured independently.
- **Correctness:** $\sum_{i=0}^{N-1} V[i]$ exact mathematical match.
- **Metric:** Construction time (ms), traversal time (ms), elements/s.

#### ST4-06 — Sorting
- **Workload:** Sort 5,000,000 pseudo-random 32-bit signed integers generated with seed `0x12345678`.
- **Implementation:** Standard library sorting routine of each language (`qsort`/`std::sort`/`slice::sort`/`Arrays.sort`/`Array.prototype.sort`/`list.sort`).
- **Timing:** Input array generation excluded from sorting measurement.
- **Correctness:** Array strictly monotonically non-decreasing, element checksum match.
- **Metric:** Sort time (ms) and sorted elements/s.

#### ST4-07 — Hash Map Operations
- **Workload:** Standard hash table operations:
  1. 5,000,000 unique key-value insertions (`i64` key $\to$ `i64` value).
  2. 5,000,000 successful lookups.
  3. 1,000,000 removals.
- **Implementation:** Standard library hash map (`std::unordered_map`, `HashMap`, `Map`, `dict`).
- **Measurement:** Insertion, lookup, and deletion times measured separately and aggregated.
- **Correctness:** Verified 4,000,000 remaining entries and lookup consistency.
- **Metric:** Operations/s for insert, read, and delete.

#### ST4-08 — Binary Search
- **Workload:** 20,000,000 sorted 64-bit integers; execute 10,000,000 binary search queries (50% hit, 50% miss).
- **Algorithm:** Standard iterative or library binary search.
- **Timing:** Build time excluded; search phase measured.
- **Correctness:** Accumulator sum of search indices and hit flags.
- **Metric:** Queries per second (Mqueries/s).

---

### Category C: Memory and Allocation

#### ST4-09 — Small-Object Allocation
- **Workload:** Allocate 10,000,000 small heap objects (each object: two 64-bit integers and one pointer/reference).
- **Phases:**
  1. Allocation and field initialization.
  2. Traversal and touch.
  3. Deallocation / drop / garbage collection.
- **Measurement:** Allocation, traversal, and release phases measured independently.
- **Metric:** Allocation rate (Mobj/s), peak RSS (MiB), deallocation time (ms).

#### ST4-10 — Large Memory Throughput
- **Workload:** Contiguous 512 MiB memory buffer.
- **Phases:**
  1. Sequential write (initialization).
  2. Sequential read and validation.
  3. In-place transformation ($x \leftarrow (x \times 3) \oplus 0x55$).
- **Metric:** Bandwidth throughput in MiB/s for read, write, and transform.

#### ST4-11 — Randomized Pointer Chasing
- **Workload:** Linked list containing 10,000,000 nodes shuffled in memory to defeat cache prefetching.
- **Algorithm:** Sequential traversal following scrambled pointers; each node adds its payload to a checksum.
- **Measurement:** Shuffled structure build time excluded; traversal time timed strictly.
- **Metric:** Traversal latency (ns/node) and nodes/s.

#### ST4-12 — Binary Tree Construction and Traversal
- **Workload:** Complete binary tree of depth 20 (1,048,575 nodes).
- **Phases:**
  1. Recursive/iterative tree construction.
  2. Preorder traversal checksum.
  3. Inorder traversal checksum.
  4. Postorder traversal checksum.
  5. Tree deallocation.
- **Metric:** Build time (ms), traversal times (ms), deallocation time (ms).

---

### Category D: Runtime and Language Overhead

#### ST4-13 — Recursive Function Overhead
- **Workload:** Recursive merge sort on 4,000,000 integers.
- **Rule:** Strict recursive call chain; no iterative emulation or manual stack trampolines.
- **Measurement:** Input generation excluded; recursive sorting timed.
- **Correctness:** Validated sorted array order and checksum.
- **Metric:** Execution time (ms).

#### ST4-14 — String Processing
- **Workload:** Deterministic 256 MiB ASCII corpus.
- **Operations:**
  1. Character frequency counting (256 bins).
  2. Substring search (exact count of matches for target keywords).
  3. Whitespace tokenization into token stream.
  4. Case conversion (lower $\to$ upper).
- **Timing:** File generation/load excluded; operations measured independently.
- **Metric:** Processing throughput in MiB/s per operation.

#### ST4-15 — CSV Parsing
- **Workload:** 5,000,000 rows × 8 columns preloaded in memory.
- **Schema:** Mix of integer IDs, UUID strings, floating-point measurements, quoted strings with commas, empty fields, and escaped quotes.
- **Rules:** Same CSV grammar parsing in every language. Input buffer preloaded to isolate parse compute from disk I/O.
- **Correctness:** Verified aggregate sums across numeric columns and count of parsed rows.
- **Metric:** Rows parsed per second (Mrows/s) and parse time (ms).

---

### Category E: Garbage Collection & Concurrency

#### ST4-16 — Cyclic Object Allocation and Reclamation
- **Workload:** Allocate 500,000 independent 2-node cyclic graphs (1,000,000 nodes total).
- **Lifecycle:** Allocate $\to$ establish mutual cyclic reference $\to$ sever root reference $\to$ trigger reclamation.
- **Requirement:** Tracing collector or explicit cycle reclamation must reclaim 100% of cycle nodes without leaking.
- **Metric:** Cycle creation time (ms), cycle reclamation time (ms), memory delta before and after collection.

#### Concurrency Extension (ST4-01-C & ST4-02-C)
- **Workloads:** ST4-01 (Integer throughput) and ST4-02 (Float throughput) parallelized over 2, 4, and 8 worker threads.
- **Rules:** The total workload is kept invariant (e.g., 500M ops divided equally among workers).
- **Reporting:** Scaled execution time and parallel speedup factor ($S_p = T_1 / T_p$). Concurrency metrics are reported separately from single-threaded rankings.

---

## 4. Build Configurations & Standards

| Language | Compiler / Runtime | Flags / Arguments | Optimization Standard |
|---|---|---|---|
| **Nirqoba** | Nirqoba Release Backend | `--release -O3` | Production LLVM backend, no debug assertions |
| **C** | GCC 16.2.0 | `-O3 -DNDEBUG -march=native` (or standard baseline) | Release, max optimization, disabled assertions |
| **C++** | G++ 16.2.0 | `-std=c++20 -O3 -DNDEBUG` | Release, inline expansion, standard containers |
| **Rust** | rustc / cargo 1.99.1 | `cargo build --release` | `opt-level = 3`, LTO matching default release profile |
| **Java** | Java SE / OpenJDK | `-XX:+UseParallelGC -Xms8G -Xmx8G` | Pre-allocated fixed heap, warmed-up JIT |
| **JavaScript** | Node.js v24.22.1 | `--max-old-space-size=8192` | Production execution, V8 JIT enabled |
| **Python** | CPython 3.12.7 | `python -O` | Bytecode optimization, standard interpreter |

---

## 5. Metrics Collected

Every workload reports the following standard metrics:

| Metric | Field in Matrix CSV | Description / Unit |
|---|---|---|
| **Execution time** | `median_time_ms` | Median wall-clock duration of measured phase (ms) |
| **Timing dispersion** | `min_time_ms`, `max_time_ms`, `std_dev_ms` | Range and standard deviation across $\ge 15$ runs |
| **Relative performance** | `relative_performance_ratio` | $\text{Time}_{\text{Ref}} / \text{Time}_{\text{Nirqoba}}$ |
| **Peak RSS** | `peak_rss_mib` | Peak Resident Set Size in MiB |
| **Memory Allocated** | `memory_allocated_mib` | Total allocated memory volume (if measurable) |
| **Allocation Count** | `allocation_count` | Number of heap allocations made |
| **GC Time** | `gc_time_ms` | Duration spent inside garbage collection phases |
| **GC Cycles** | `gc_count` | Number of GC collection cycles triggered |
| **CPU Utilization** | `cpu_utilization_pct` | Average CPU load during workload (%) |
| **Throughput** | `throughput_value`, `throughput_unit` | Ops/s, Mops/s, or MiB/s |
| **Correctness** | `correctness_status`, `checksum_value` | Deterministic verification status (`PASS`/`FAIL`) |
| **Compilation Time** | `compilation_time_ms` | Total cold build time from source to binary (ms) |
| **Artifact Size** | `binary_artifact_size_bytes` | Size of executable binary or deployment package (bytes) |

---

## 6. Execution Protocol & Timing Isolation

1. **Host Quiescence:** Close background applications; lock CPU governor to high performance.
2. **Warm-up Phase:** JIT languages execute 5 unmeasured iterations to achieve steady-state bytecode compilation.
3. **Execution Rounds:** 15 recorded repetitions per workload.
4. **Randomized Interleaving:** Order of language runs is shuffled between runs to eliminate thermal and cache bias.
5. **Monotonic Clocks:** Timing uses high-precision OS monotonic clocks (`std::chrono::steady_clock`, `clock_gettime(CLOCK_MONOTONIC)`, `Instant::now`, `System.nanoTime`, `process.hrtime.bigint`, `time.perf_counter_ns`).
6. **Result Integrity:** Raw execution logs, exit codes, and timestamps are written to `results/raw/` prior to metric extraction.
