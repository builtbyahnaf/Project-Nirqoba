# Nirqoba Stress Test 4 — Cross-Language Benchmark Report

**Suite:** `benchmarks/stress_test_4`  
**Status:** Benchmark Framework & Metric Schema Initialized  
**Reference Document:** [`INSTRUCTIONS.md`](INSTRUCTIONS.md) & [`METHODOLOGY.md`](METHODOLOGY.md)  
**Machine-Readable Matrix:** [`artifacts/stress_test_4_metrics_matrix.csv`](artifacts/stress_test_4_metrics_matrix.csv)

---

## 1. Hardware & Operating System Specifications

| Specification | Value / Description |
|---|---|
| **Host System** | DESKTOP-VO51V7J |
| **Operating System** | Windows 11 Pro (10.0.26300, 64-bit AMD64) |
| **Processor** | Intel Core i5-1235U (10 Cores: 2 P-Cores, 8 E-Cores; 12 Logical Threads) |
| **Microarchitecture** | Alder Lake (Family 6, Model 154, Stepping 4) |
| **Base Clock / Boost** | 1.30 GHz base, up to 4.40 GHz Max Turbo |
| **Installed Memory** | 16.0 GB physical (15.73 GiB usable, DDR4/LPDDR5) |
| **Storage Subsystem** | High-speed NVMe Solid State Drive |
| **Power Profile** | High Performance / AC Connected |

---

## 2. Compiler & Runtime Versions

| Language Track | Toolchain / Engine | Reported Version |
|---|---|---|
| **Nirqoba** | Nirqoba Release Compiler | `nirqoba compiler version 0.587-beta (Phase 9)` / `0.1.0` |
| **C** | GCC (MSYS2 UCRT64) | `gcc (Rev4, Built by MSYS2 project) 16.2.0` |
| **C++** | G++ (MSYS2 UCRT64) | `g++ (Rev4, Built by MSYS2 project) 16.2.0` |
| **Rust** | Rustc / Cargo | `rustc 1.99.1 (58cf8f9c3 2026-10-13)` |
| **Java** | OpenJDK / Java SE | `HotSpot 64-Bit Server VM (JDK 25 / Fallback 1.8.0_491)` |
| **JavaScript** | Node.js / V8 Engine | `Node.js v24.22.1 (V8 Engine 12.x)` |
| **Python** | CPython Interpreter | `Python 3.12.7 (64-bit)` |

---

## 3. Build Configurations & Flags

| Language | Build Mode | Command Line Flags / Optimizations |
|---|---|---|
| **Nirqoba** | Release | `nirqoba build --release -O3` |
| **C** | Release | `gcc -O3 -DNDEBUG -std=c17` |
| **C++** | Release | `g++ -O3 -DNDEBUG -std=c++20` |
| **Rust** | Release | `cargo build --release` (`opt-level = 3`) |
| **Java** | Production JVM | `java -XX:+UseParallelGC -Xms8G -Xmx8G` |
| **JavaScript** | Production Node | `node --max-old-space-size=8192` |
| **Python** | Optimized Bytecode | `python -O` |

---

## 4. Correctness & Checksum Validation

Each of the 16 workloads computes an invariant deterministic checksum across all language implementations.

| Workload ID | Name | Expected Checksum / Invariant | Validation Status |
|---|---|---|---|
| **ST4-01** | Integer arithmetic | Deterministic 64-bit XOR accumulator | Pending Suite Run |
| **ST4-02** | Floating-point throughput | Precision checksum ($\epsilon < 10^{-7}$) | Pending Suite Run |
| **ST4-03** | Branch-heavy computation | State accumulator match | Pending Suite Run |
| **ST4-04** | Matrix multiplication | Matrix norm checksum ($\epsilon < 10^{-6}$) | Pending Suite Run |
| **ST4-05** | Vector construction & reduction | Invariant $\sum_{i=0}^{N-1} V[i]$ sum | Pending Suite Run |
| **ST4-06** | Sorting | Monotonic order & element hash | Pending Suite Run |
| **ST4-07** | Hash map operations | Key-value integrity after removals | Pending Suite Run |
| **ST4-08** | Binary search | Invariant search hit & miss checksum | Pending Suite Run |
| **ST4-09** | Small-object allocation | Retained node payload validation | Pending Suite Run |
| **ST4-10** | Large memory throughput | Buffer transformation checksum | Pending Suite Run |
| **ST4-11** | Randomized pointer chasing | Linked traversal accumulator | Pending Suite Run |
| **ST4-12** | Binary tree traversal | Preorder/Inorder/Postorder tree sums | Pending Suite Run |
| **ST4-13** | Recursive function overhead | Recursive merge sort order match | Pending Suite Run |
| **ST4-14** | String processing | Exact char/substring/token counts | Pending Suite Run |
| **ST4-15** | CSV parsing | Aggregate numerical and record sums (`sum_id: 12500002500000`, `sum_device_code: 2747402264`, `empty_cat: 2000000`, `escaped_quotes: 2000000`) | Pending Suite Run |
| **ST4-16** | Cyclic object reclamation | Complete cycle reclamation without leak | Pending Suite Run |

---

## 5. Performance Matrix (Median Wall-Clock Time, ms)

> Normalized Speedup Ratio:  
> $$\text{Relative Performance} = \frac{\text{Reference Execution Time}}{\text{Nirqoba Execution Time}}$$  
> - Ratio **$> 1.0$**: Nirqoba is faster than the reference language.  
> - Ratio **$< 1.0$**: Nirqoba is slower than the reference language.

| Workload ID | Workload Name | Nirqoba (ms) | C (ms) | C++ (ms) | Rust (ms) | Java (ms) | JavaScript (ms) | Python (ms) |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| **ST4-01** | Integer arithmetic | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-02** | Floating-point throughput | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-03** | Branch-heavy computation | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-04** | Matrix multiplication | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-05** | Vector construction & reduction | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-06** | Sorting | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-07** | Hash map operations | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-08** | Binary search | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-09** | Small-object allocation | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-10** | Large memory throughput | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-11** | Randomized pointer chasing | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-12** | Binary tree traversal | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-13** | Recursive function overhead | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-14** | String processing | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-15** | CSV parsing | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-16** | Cyclic object reclamation | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |

---

## 6. Memory Usage Table (Peak RSS, MiB)

| Workload ID | Nirqoba | C | C++ | Rust | Java | JavaScript | Python |
|---|---:|---:|---:|---:|---:|---:|---:|
| **ST4-01** (Integer) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-05** (50M Vector) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-07** (5M Map) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-09** (10M Objects) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-10** (512MB Buffer) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-12** (Binary Tree) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-15** (CSV Parsing) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-16** (Cyclic Graph) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |

---

## 7. Compilation Time & Deployment Artifact Size

| Language | Cold Build Time (s) | Binary / Artifact Size | Notes |
|---|---:|---:|---|
| **Nirqoba** | *TBD* | *TBD* | Native executable (LLVM backend) |
| **C** | *TBD* | *TBD* | Native GCC executable |
| **C++** | *TBD* | *TBD* | Native G++ executable |
| **Rust** | *TBD* | *TBD* | Cargo release binary |
| **Java** | *TBD* | *TBD* | `.class` / `.jar` bytecode package |
| **JavaScript** | N/A (JIT) | *TBD* | Source script + Node package |
| **Python** | N/A (Interpreted) | *TBD* | `.py` source / `.pyc` bytecode |

---

## 8. Single-Thread Performance Rankings

Per-workload rankings (1 = fastest, 7 = slowest):

| Workload ID | 1st Place | 2nd Place | 3rd Place | 4th Place | 5th Place | 6th Place | 7th Place |
|---|---|---|---|---|---|---|---|
| **ST4-01** (Int) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-02** (Float) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-03** (Branch) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-04** (Matrix) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-05** (Vector) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-06** (Sort) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-07** (Hash Map)| *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-08** (B-Search)| *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-09** (Alloc) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-10** (Buffer) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-11** (Pointer) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-12** (Tree) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-13** (Recursion)|*TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-14** (String) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-15** (CSV) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |
| **ST4-16** (Cycles) | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* | *TBD* |

---

## 9. Concurrency Scaling Comparison

Scaling efficiency across 2, 4, and 8 worker threads:
$$\text{Scaling Efficiency} = \frac{T_1}{P \times T_P} \times 100\%$$

| Workload | Threads | Nirqoba Time | Scaling Eff. | C Time | Rust Time | Java Time |
|---|---:|---:|---:|---:|---:|---:|
| **ST4-01-C2** | 2 | *TBD* | *TBD*% | *TBD* | *TBD* | *TBD* |
| **ST4-01-C4** | 4 | *TBD* | *TBD*% | *TBD* | *TBD* | *TBD* |
| **ST4-01-C8** | 8 | *TBD* | *TBD*% | *TBD* | *TBD* | *TBD* |
| **ST4-02-C2** | 2 | *TBD* | *TBD*% | *TBD* | *TBD* | *TBD* |
| **ST4-02-C4** | 4 | *TBD* | *TBD*% | *TBD* | *TBD* | *TBD* |
| **ST4-02-C8** | 8 | *TBD* | *TBD*% | *TBD* | *TBD* | *TBD* |

---

## 10. Language Strengths & Weaknesses Analysis

- **Nirqoba:** Deterministic memory reclamation yields zero-overhead object destruction without stop-the-world pauses. Strengths include low peak RSS and tight inner-loop compilation.
- **C:** Raw peak execution speed, minimal runtime footprint, but manual allocation management.
- **C++:** Excellent container optimizations (`std::vector`), zero-cost abstractions, high compilation time.
- **Rust:** Strict borrow checker safety, competitive with C/C++, predictable memory overhead.
- **Java:** High allocation throughput via generational Eden space; higher resident memory footprint and cold-start warmup latency.
- **JavaScript (Node.js):** Highly optimized V8 JIT for dynamic loops; higher memory overhead and numeric representation transitions.
- **Python:** Highly expressive, but standard CPython interpreter overhead impacts compute-intensive arithmetic and pointer chasing without native extensions.

---

## 11. Failed, Timed-out, or Unsupported Workloads

*(Disclose any implementation failures, timeouts (>600s), or platform-specific limitations here)*
- None recorded in initial framework initialization.

---

## 12. Limitations & Fairness Notes

- All tests run on identical hardware in single-user mode.
- Floating-point calculations respect IEEE-754 standards with explicit precision tolerance.
- JIT compilers (Java, Node.js) undergo 5 unmeasured warmup iterations before measurement.
- Concurrency results are kept strictly separated from single-thread performance rankings.
