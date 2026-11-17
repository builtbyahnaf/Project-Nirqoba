# Nirqoba Stress Test 4 — Cross-Language Performance Matrix

**Status:** Benchmark Specification & Harness Scaffold  
**Suite:** `benchmarks/stress_test_4`  
**Purpose:** Evaluate Nirqoba against C, C++, Rust, Java, JavaScript, and Python across 16 performance-critical workloads.

---

## 1. Overview & Objectives

Stress Test 4 is a comprehensive, objective cross-language benchmarking suite designed to measure raw computational efficiency, data structure performance, memory subsystem throughput, runtime overhead, and garbage collection / concurrency scaling.

The benchmark compares **Nirqoba** against 6 industry-standard reference languages:
1. **Nirqoba** (Deterministic ownership runtime / LLVM backend)
2. **C** (GCC 16.2.0, `-O3 -DNDEBUG`)
3. **C++** (G++ 16.2.0, `-O3 -DNDEBUG -std=c++20`)
4. **Rust** (rustc / cargo 1.99.1, `--release`)
5. **Java** (JVM production runtime, tuned heap)
6. **JavaScript** (Node.js v24.22.1, V8 JIT)
7. **Python** (CPython 3.12.7, optimized mode)

---

## 2. Workload Matrix (16 Workloads + Concurrency Extension)

Every workload must be implemented idiomatically in all 7 languages using identical input datasets, deterministic seeds, and identical validation checksums.

### Category A — CPU Computation
| ID | Workload | Specification | Validation Metric |
|---|---|---|---|
| **ST4-01** | Integer arithmetic throughput | 500 million deterministic integer ops (add, sub, mul, div, bitwise) | Checksum accumulation |
| **ST4-02** | Floating-point throughput | 100 million deterministic float ops (add, mul, div, sqrt, accum) | Tolerance-checked float checksum |
| **ST4-03** | Branch-heavy computation | 100 million deterministic inputs with nested/unpredictable branches | Output checksum |
| **ST4-04** | Matrix multiplication | 1024 × 1024 dense matrix multiply (conventional naive algorithm, no external BLAS) | Result matrix checksum |

### Category B — Data Structures & Algorithms
| ID | Workload | Specification | Validation Metric |
|---|---|---|---|
| **ST4-05** | Vector construction & reduction | Construct 50M 64-bit int vector, traverse, reduce | Final sum checksum |
| **ST4-06** | Sorting | Sort 5 million deterministic 32-bit integers using standard library | Sorted order & checksum |
| **ST4-07** | Hash map operations | 5M insertions, 5M successful lookups, 1M removals | Key-value integrity & count |
| **ST4-08** | Binary search | 20M sorted integers, 10M queries (successful + unsuccessful) | Query result checksum |

### Category C — Memory & Allocation
| ID | Workload | Specification | Validation Metric |
|---|---|---|---|
| **ST4-09** | Small-object allocation | 10M small objects (two `i64` + one reference), traverse & reclaim | Allocation & reclaim time |
| **ST4-10** | Large memory throughput | 512 MiB contiguous buffer sequential write, read, and transform | MiB/s throughput & checksum |
| **ST4-11** | Randomized pointer chasing | 10M nodes in deterministic randomized linked order | Traversal time & checksum |
| **ST4-12** | Binary tree construction & traversal | 1,048,575 nodes complete binary tree; preorder, inorder, postorder | Traversal checksums |

### Category D — Runtime & Language Overhead
| ID | Workload | Specification | Validation Metric |
|---|---|---|---|
| **ST4-13** | Recursive function overhead | Recursive merge sort on 4M deterministic integers (pure recursion) | Sorted output validation |
| **ST4-14** | String processing | 256 MiB ASCII text: char count, substring count, tokenization, case conversion | Phase times & count checks |
| **ST4-15** | CSV parsing | 5M rows × 8 columns preloaded in-memory parser (strings, escapes, empty fields) | Validated aggregates |

### Category E — Garbage Collection & Concurrency
| ID | Workload | Specification | Validation Metric |
|---|---|---|---|
| **ST4-16** | Cyclic object reclamation | 500,000 two-node cyclic pairs (1M nodes), drop roots, trigger reclamation | Reclaim count & memory drop |
| **ST4-01-C**| Concurrency Integer Arithmetic | ST4-01 scaled across 2, 4, and 8 worker threads | Multithreaded scaling efficiency |
| **ST4-02-C**| Concurrency Floating-Point | ST4-02 scaled across 2, 4, and 8 worker threads | Multithreaded scaling efficiency |

---

## 3. Directory Layout

The benchmark workspace is organized as follows:

```text
stress_test_4/
├── README.md                      # This document (suite overview & index)
├── METHODOLOGY.md                 # Experimental protocol, metrics, and fairness rules
├── BENCHMARK_REPORT.md            # Results tables, analysis, and relative speedup matrix
├── INSTRUCTIONS.md                # Benchmark specification prompt & requirements
├── runner/                        # Build and benchmark harness runner
│   └── README.md
├── datasets/                      # Deterministic generators & schemas for input data
│   ├── README.md
│   └── st4_15_schema_sample.csv   # Schema sample for 5M-row CSV workload
├── artifacts/                     # Machine-readable output templates and deliverables
│   └── stress_test_4_metrics_matrix.csv # Pre-populated 154-row metrics matrix
├── nirqoba/                       # Nirqoba track (workloads 01 to 16)
├── c/                             # C track (workloads 01 to 16)
├── cpp/                           # C++ track (workloads 01 to 16)
├── rust/                          # Rust track (workloads 01 to 16)
├── java/                          # Java track (workloads 01 to 16)
├── javascript/                    # JavaScript (Node.js) track (workloads 01 to 16)
├── python/                        # Python track (workloads 01 to 16)
└── results/
    ├── raw/                       # Raw process execution outputs per run
    ├── processed/                 # Processed summary tables and metrics
    │   └── stress_test_4_metrics_matrix.csv
    └── environment.json           # Host machine specs, compilers, and flags
```

---

## 4. Execution & Measurement Rules

- **Repetitions:** Minimum 15 measured runs per workload/language combination.
- **Reporting Statistic:** Primary results use the **median** wall-clock time; min, max, and standard deviation are recorded.
- **Warm-up:** JIT-based runtimes (Java HotSpot, Node.js V8) run dedicated warm-up iterations prior to timing.
- **Timing Boundaries:** Compilation and disk-loading are isolated from core in-memory kernel timings.
- **Machine State:** Single-user idle state, identical power profiles, identical hardware.
- **Relative Comparison Ratio:**
  $$\text{Relative Performance} = \frac{\text{Reference Execution Time}}{\text{Nirqoba Execution Time}}$$
  A ratio $> 1.0$ indicates Nirqoba executes faster than the reference; $< 1.0$ indicates Nirqoba is slower.

For detailed methodological requirements, consult [`METHODOLOGY.md`](METHODOLOGY.md).
