# Phase 6 profiler overhead baseline

Date: 2026-11-01. Platform: Windows x64. Compiler/runtime: current Nirqoba RelWithDebInfo build (MSVC 14.51.36231, LLVM/MLIR 23.1.4). Workload: 100,000 iterations of allocate 64 bytes, touch one byte, and deallocate. 2 warmups and 5 timed process runs; wall-clock process duration.

| Mode | p50 (ms) | p95 (ms) | p99 (ms) |
|---|---:|---:|---:|
| Profiler off | 42.951 | 44.484 | 44.484 |
| Profiler on | 46.002 | 46.590 | 46.590 |

The five-run sample is a smoke baseline, not a stable performance conclusion. The observed median delta was 3.051 ms (7.1% of the off median) for this specific allocation-heavy workload. Repeat with the default 30 runs or more on a controlled idle machine before setting a regression threshold. It does not represent general application overhead.

Command: `powershell -ExecutionPolicy Bypass -File .\\benchmarks\\phase6\\run-profiler-overhead.ps1 -Compiler .\\build\\msvc-dev\\bin\\nirqoba.exe -Warmups 2 -Runs 5 -Output .\\build\\logs\\profiler-overhead.json`.
