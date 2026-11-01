# Phase 6 memory benchmark protocol

`run.ps1` builds optimized native baselines and records repeated samples with compiler and machine metadata. It deliberately separates native baseline data from Nirqoba results because Nirqoba does not yet provide owned contiguous containers or equivalent concurrency APIs. Do not interpret this as a language comparison until equivalent workloads are available.

Examples:

```powershell
.\run.ps1 -Iterations 1000000 -Warmups 5 -Runs 30
```

The allocation kernels include 16-byte small requests, 4-KiB large requests,
an alternating mixed workload, and the original 64-byte kernel. Each touches
the first and last 64-bit words and contributes to a checksum. Scalar,
contiguous-array, pointer-rich graph, and four-thread allocation workloads use
matching C++ and Rust algorithms. Compile with MSVC `/O2 /std:c++20` and Rust
`rustc -C opt-level=3`; use the same target CPU setting when available. The
script checks per-run checksum stability and cross-language equality and reports
median, p95, and p99 wall-clock duration. Keep output and environment details
with the phase report. GC pause benchmarking is a separate deferred workload.
The benchmark suite includes `run-profiler-overhead.ps1`, which compares the same 100,000 explicit allocation/deallocation loop with profiler instrumentation disabled and enabled. It builds both workloads once, warms them up, records repeated wall-clock samples, and reports p50/p95/p99. The initial five-run baseline is recorded in `reports/phase-06.9-profiler-overhead.md`; it is exploratory and not a performance claim.

## Phase 6 Upgrade benchmark harness

`run.ps1` now defines matched C++ and Rust workloads for scalar arithmetic, contiguous arrays, 64-byte allocation, pointer-rich object graphs, and four-thread allocation. It uses warmups, repeated runs, p50/p95/p99 timings, compiler flags/version metadata, CPU/OS metadata, and checksum equality to reject mismatched algorithms. Builds and outputs are isolated in a temporary directory. A Rust compiler is not installed in the current environment, so this matrix is implemented but no C++/Rust comparison result is claimed. Profiler on/off measurements are recorded separately.
