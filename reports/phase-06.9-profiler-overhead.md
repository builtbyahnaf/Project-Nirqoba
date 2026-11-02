# Phase 6 profiler overhead baseline

## 2026-11-02 RelWithDebInfo run

Platform: Windows x64. Compiler/runtime: current `build/msvc-dev` Nirqoba
RelWithDebInfo build, MSVC 14.51.36231, LLVM/MLIR 23.1.4. Workload: 100,000
iterations of allocate 64 bytes, touch one byte, and deallocate. Five warmups
and 30 timed process runs per mode; wall-clock process duration.

| Mode | p50 (ms) | p95 (ms) | p99 (ms) |
|---|---:|---:|---:|
| Profiler off | 53.4315 | 56.7653 | 99.8433 |
| Profiler on | 54.1720 | 56.3264 | 99.0942 |

The median delta was 0.7405 ms, about 1.39% of the profiler-off median. Both
modes contain a roughly 99 ms tail sample, so the distributions overlap and the
single-run difference is not evidence of a stable overhead percentage. Keep
this as a local baseline; do not use it as a cross-machine claim or regression
threshold. Raw samples are in the ignored `build/logs/profiler-overhead-full.json`
artifact. No GC benchmark was run.

## 2026-11-01 smoke baseline

Platform: Windows x64. Compiler/runtime: Nirqoba RelWithDebInfo build (MSVC
14.51.36231, LLVM/MLIR 23.1.4). Workload: 100,000 iterations of allocate 64
bytes, touch one byte, and deallocate. Two warmups and five timed process runs;
wall-clock process duration.

| Mode | p50 (ms) | p95 (ms) | p99 (ms) |
|---|---:|---:|---:|
| Profiler off | 42.951 | 44.484 | 44.484 |
| Profiler on | 46.002 | 46.590 | 46.590 |

The five-run sample is a smoke baseline, not a stable performance conclusion. The observed median delta was 3.051 ms (7.1% of the off median) for this specific allocation-heavy workload. Repeat with the default 30 runs or more on a controlled idle machine before setting a regression threshold. It does not represent general application overhead.

Command: `powershell -ExecutionPolicy Bypass -File .\\benchmarks\\phase6\\run-profiler-overhead.ps1 -Compiler .\\build\\msvc-dev\\bin\\nirqoba.exe -Warmups 2 -Runs 5 -Output .\\build\\logs\\profiler-overhead.json`.
