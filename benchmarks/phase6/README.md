# Phase 6 memory benchmark protocol

The benchmarks in this directory are standalone baselines. Run native implementations with the same compiler family/version, target CPU, optimization level, input sizes, and warmup/repetition counts. Record complete output and system details in `reports/phase-06-upgrade-benchmarks.md`. Do not compare debug builds or infer language-wide performance from one workload.

`allocation.cpp` and `allocation.rs` implement the same allocate-touch-free workload. Use 5 warmups and 30 measured runs; report median and p95. Extend with graph traversal and multithreaded cases before making broad claims. Nirqoba currently has no owned container syntax, so this baseline does not yet compare equivalent Nirqoba programs.
