# Nirqoba Stress Test 4 — Cross-Language Performance Matrix

**Status:** Benchmark specification  
**Purpose:** Evaluate Nirqoba against C, C++, Rust, Java, JavaScript, and Python across 16 performance-critical workloads.

## 1. Languages and implementations

Implement every workload in:

1. Nirqoba
2. C
3. C++
4. Rust
5. Java
6. JavaScript (Node.js)
7. Python (CPython)

Use idiomatic implementations in each language. Do not deliberately handicap a language or introduce artificial advantages.

### Memory-management rules

- **Nirqoba:** Use deterministic ownership by default. Use GC explicitly in designated GC workloads.
- **C:** Use `malloc`/`free` or appropriate stack allocation.
- **C++:** Use RAII, standard containers, and appropriate smart pointers.
- **Rust:** Use ownership, borrowing, `Box`, and standard containers.
- **Java:** Use normal JVM-managed allocation and garbage collection.
- **JavaScript:** Use Node.js and normal V8-managed allocation.
- **Python:** Use standard CPython allocation and garbage collection.

Do not force every language to use the same allocator or memory-management mechanism. Record the implementation used.

---

## 2. The 16 benchmark workloads

### A. CPU computation

**ST4-01 — Integer arithmetic throughput**
- Execute 500 million deterministic integer operations.
- Include addition, subtraction, multiplication, division, and bitwise operations.
- Prevent dead-code elimination by accumulating and validating a final checksum.
- Measure total execution time.

**ST4-02 — Floating-point throughput**
- Execute 100 million deterministic floating-point operations.
- Include addition, multiplication, division, square root, and floating-point accumulation.
- Use identical inputs and numerically appropriate correctness tolerances.
- Report the final checksum and execution time.

**ST4-03 — Branch-heavy computation**
- Process 100 million deterministic integer inputs.
- Include conditional branches, nested conditions, and predictable/unpredictable branch patterns.
- Validate the resulting output checksum.

**ST4-04 — Matrix multiplication**
- Multiply two 1024 × 1024 matrices.
- Use conventional dense matrix multiplication.
- Do not use external BLAS, GPU acceleration, or third-party optimized math libraries.
- Validate the resulting matrix using a checksum and numerical tolerance.

### B. Data structures and algorithms

**ST4-05 — Vector construction and reduction**
- Construct an array/vector containing 50 million 64-bit integers.
- Perform initialization, sequential traversal, and reduction.
- Validate the final sum.
- Measure construction and traversal separately.

**ST4-06 — Sorting**
- Sort 5 million deterministic 32-bit integers.
- Use each language's standard sorting implementation.
- Validate sorted order and checksum.
- Report sorting time separately from input generation.

**ST4-07 — Hash map operations**
- Insert 5 million unique key-value pairs.
- Perform 5 million successful lookups.
- Perform 1 million removals.
- Validate the number of remaining entries and their values.
- Use each language's standard hash-map implementation.

**ST4-08 — Binary search**
- Build a sorted array of 20 million integers.
- Execute 10 million deterministic search queries.
- Include both successful and unsuccessful searches.
- Validate the result checksum.

### C. Memory and allocation

**ST4-09 — Small-object allocation**
- Allocate 10 million small objects containing:
  - Two 64-bit integers.
  - One 64-bit reference/pointer field.
- Initialize and touch every object.
- Release or make objects reclaimable after use.
- Measure allocation, initialization, and reclamation separately where possible.

**ST4-10 — Large memory throughput**
- Allocate and initialize a 512 MiB contiguous buffer.
- Perform sequential writes, sequential reads, and a deterministic transformation.
- Validate the resulting checksum.
- Report throughput in MiB/s.

**ST4-11 — Randomized pointer chasing**
- Construct a linked structure containing 10 million nodes.
- Arrange nodes in a deterministic randomized traversal order.
- Traverse the complete structure while accumulating a checksum.
- Measure traversal independently from construction.

**ST4-12 — Binary tree construction and traversal**
- Construct a complete binary tree with 1,048,575 nodes.
- Each node contains an integer value and two child references.
- Perform preorder, inorder, and postorder traversal.
- Validate node count and traversal checksums.
- Measure construction and each traversal independently.

### D. Runtime and language overhead

**ST4-13 — Recursive function overhead**
- Implement recursive merge sort on a deterministic array of 4 million integers.
- Measure recursive execution separately from input generation.
- Validate the sorted output.
- Do not replace recursion with an iterative implementation.

**ST4-14 — String processing**
- Generate a deterministic 256 MiB ASCII text input.
- Perform:
  - Character counting.
  - Substring occurrence counting.
  - Whitespace tokenization.
  - Case conversion.
- Use identical byte-level input and equivalent output semantics.
- Measure each operation separately.

**ST4-15 — CSV parsing**
- Generate a deterministic CSV file containing 5 million rows and 8 columns.
- Include integers, decimal numbers, quoted strings, empty fields, and escaped quotes.
- Parse every row and calculate validated aggregates.
- Implement the same CSV grammar in every language.
- Exclude disk reading from the primary parsing measurement by loading the input before timing.

### E. Garbage collection and concurrency

**ST4-16 — Cyclic object allocation and reclamation**
- Construct 500,000 independent two-node cyclic object pairs.
- Each pair must contain two references forming a cycle.
- Drop all external references.
- Trigger collection explicitly where supported.
- Validate that all eligible cycles become reclaimable.
- Measure construction and reclamation independently.

**Concurrency extension (separate results, not a replacement for the single-thread tests):**
- Repeat ST4-01 and ST4-02 with 2, 4, and 8 worker threads.
- Divide the same total workload among workers.
- Validate identical results.
- Report scaling efficiency and total execution time.
- Do not mix concurrency results into single-thread rankings.

---

## 3. Benchmark execution requirements

### Build configuration

| Language | Required configuration |
|---|---|
| Nirqoba | Release build, production runtime |
| C | GCC/Clang `-O3 -DNDEBUG` |
| C++ | GCC/Clang `-O3 -DNDEBUG` |
| Rust | `cargo build --release` |
| Java | Optimized JVM, fixed heap configuration |
| JavaScript | Node.js production execution, V8 JIT enabled |
| Python | Standard CPython, optimized execution |

Record exact compiler, runtime, and standard-library versions.

Do not silently enable LTO, PGO, native CPU-specific tuning, alternative Python interpreters, or third-party acceleration for only selected languages. If additional configurations are tested, publish them as separate benchmark profiles.

### Execution procedure

1. Build all implementations before timed execution.
2. Verify correctness before measuring performance.
3. Run a warm-up phase for JIT-based languages.
4. Execute at least 15 measured runs per workload and language.
5. Randomize language execution order between repetitions.
6. Use the median as the primary result.
7. Also report minimum, maximum, and standard deviation.
8. Keep the machine idle during measurements.
9. Run every language on the same machine and operating-system environment.
10. Use a high-resolution monotonic timer.

Each workload must take long enough to produce meaningful measurements. Increase its input size if execution is too short, but use the same workload size for all languages.

### Timing boundaries

- Exclude compilation from execution-time measurements.
- Exclude dataset generation unless dataset generation is explicitly the workload.
- Include required object initialization and allocation.
- Measure construction, traversal, and reclamation independently when specified.
- For GC workloads, report both explicit collection time and total workload time.
- For JIT languages, report warm-up separately and never include it in the steady-state median.

---

## 4. Metrics to collect

Every language must report the following where measurable:

| Metric | Description |
|---|---|
| Execution time | Median wall-clock time |
| Minimum / maximum | Observed timing range |
| Standard deviation | Measurement variability |
| Peak RSS | Maximum resident memory |
| Memory allocated | Total allocation volume, if measurable |
| Allocation count | Number of allocations, if measurable |
| GC time | Time spent in garbage collection |
| GC count | Number of collection cycles |
| CPU utilization | Average and peak utilization |
| Throughput | Operations/s or MiB/s |
| Correctness | Checksum/assertion result |
| Compilation time | Reported separately |
| Binary/artifact size | Executable or deployment artifact size |

Unavailable metrics must be marked `N/A`, never estimated or fabricated.

---

## 5. Fairness and correctness rules

- Identical input datasets and deterministic random seeds.
- Equivalent algorithms and output semantics.
- No external libraries that provide pre-optimized implementations of the measured algorithm.
- Standard-library implementations are permitted.
- No GPU acceleration.
- No manual assembly or language-specific benchmark shortcuts.
- No dead-code elimination of the measured work.
- No benchmarking with debug builds.
- No mixing results from different machines or hardware configurations.
- No comparing a cold JVM run against a warmed-up native executable.
- No reporting only the best run.
- No claiming a performance win when the difference is within measurement noise.

For floating-point workloads, define acceptable numerical tolerances. For integer and data-structure workloads, require exact correctness.

---

## 6. Required benchmark output

Create:

`benchmarks/stress_test_4/`

Suggested structure:

```text
stress_test_4/
├── README.md
├── METHODOLOGY.md
├── BENCHMARK_REPORT.md
├── runner/
├── datasets/
├── nirqoba/
│   ├── 01_integer_arithmetic/
│   └── ...
├── c/
├── cpp/
├── rust/
├── java/
├── javascript/
├── python/
└── results/
    ├── raw/
    ├── processed/
    └── environment.json
```

Each workload must have the same logical implementation across all seven languages.

The runner must:
- Build each implementation.
- Run correctness validation.
- Execute warm-ups and measured runs.
- Apply timeouts and terminate hung processes.
- Capture exit codes and errors.
- Record environment information.
- Save raw measurements in machine-readable JSON or CSV.
- Generate a consolidated Markdown report.

Never overwrite previous benchmark results without preserving the original raw data.

---

## 7. Final report format

The report must contain:

1. Hardware and operating-system specifications.
2. Compiler and runtime versions.
3. Build configurations.
4. Correctness validation results.
5. A performance table for all 16 workloads.
6. A memory-usage table.
7. A compilation-time table.
8. A single-thread performance ranking.
9. A separate concurrency comparison.
10. A per-language strengths and weaknesses analysis.
11. A list of failed, timed-out, or unsupported workloads.
12. Limitations and fairness notes.

Use normalized performance ratios:

`Relative performance = Reference execution time / Nirqoba execution time`

A value greater than 1 means Nirqoba is faster than the selected reference. A value below 1 means it is slower.

Show comparisons against every language, not just the fastest competitor.

Do not produce one misleading universal winner score. Report workload-specific rankings and, optionally, a clearly labeled geometric mean of normalized ratios.

---

## 8. Completion criteria

- All seven languages have implementations for all 16 workloads.
- Every implementation passes correctness checks.
- All measured workloads have at least 15 valid runs.
- Raw results are retained.
- The report includes all required metrics and environmental metadata.
- Failed workloads are disclosed rather than silently omitted.
- No benchmark source changes the Nirqoba compiler or runtime.
- The benchmark directory does not introduce unrelated changes to the project.

**This is a measurement project, not an optimization project. Do not modify Nirqoba's implementation to improve results while building the benchmark suite.**