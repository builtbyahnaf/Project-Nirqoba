# Stress Test 4 — Test Runner Architecture

This directory houses the orchestration runner for the 16 workloads across 7 languages.

## Runner Responsibilities

As defined in `INSTRUCTIONS.md` Section 6:
1. **Compilation:** Invoke language-specific build systems in release mode (`nirqoba build`, `gcc`, `g++`, `cargo build --release`, `javac`).
2. **Correctness Validation:** Run each target executable once with correctness assertions enabled. If an invariant checksum fails, mark as `FAIL` and abort benchmark runs for that target.
3. **Warm-up:** Run 5 unmeasured iterations for JIT runtimes (Java HotSpot, Node.js V8) to allow JIT compilation and tiering.
4. **Execution Protocol:** Execute $\ge 15$ measured runs per workload in randomized order across languages.
5. **Process Safety:** Enforce process timeouts (default 600s), capture exit codes, and kill hanging processes.
6. **Telemetry Collection:** Capture execution time, peak RSS, GC duration, and CPU utilization.
7. **Persistence:**
   - Write raw execution outputs to `results/raw/<workload>/<language>_run_<idx>.log`.
   - Update `artifacts/stress_test_4_metrics_matrix.csv` and `results/processed/`.
   - Update `BENCHMARK_REPORT.md` with aggregated medians, ranges, and rankings.
