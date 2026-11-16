# Phase 8 benchmarks — manual memory

**Version:** `0.583-beta`  
**Target:** x86_64-pc-windows-msvc, RelWithDebInfo compiler  
**Date:** 2026-11-16

The runner is `benchmarks/memory-model/phase-8/run_benchmark.py`, with the
language workloads and the full JSON report alongside it. It compiles each
workload at `-O2` and `-O2 --debug-enabled`, runs seven fresh processes per
configuration, checks checksum equality across configurations, and records
cycles per operation, total cycles, process elapsed time and sampled peak RSS.

## Production medians

| Workload | Operations | Median cycles/op | Range | Peak RSS |
|---|---:|---:|---:|---:|
| ordinary allocation/release | 1,000,000 | 142.231 | 134.896–144.292 | 9,859,072 B |
| manual allocation/release | 1,000,000 | 144.432 | 136.401–165.505 | 9,863,168 B |
| manual from explicit handle | 1,000,000 | 340.480 | 322.896–344.830 | 9,871,360 B |
| manual fallback cleanup | 1,000,000 | 142.417 | 136.729–144.386 | 9,846,784 B |
| manual borrow access | 10,000,000 | 1.704 | 1.535–1.724 | 9,875,456 B |
| manual managed-root payload | 100,000 | 345.422 | 338.557–349.782 | 15,650,816 B |

Ratios: manual/ordinary `1.0155`; explicit-handle/ordinary `2.3939`; fallback/
ordinary `1.0013`. The managed-root debug/production ratio is `1.0467×`.

## Interpretation

Manual and ordinary allocation use the same runtime allocator and ownership
registry. The `1.0155` ratio is not a speedup claim; it is evidence that the new
manual category does not add a separate heap or hidden allocation path. The
explicit-handle path is slower because it resolves allocator identity through the
registry, an existing property of explicit allocator selection.

These workloads do not measure FFI (there is no FFI surface), manual
reallocation (rejected), arbitrary manual alignment (not exposed), or native
resource latency (host-dependent). Peak RSS is sampled while each process is
alive and can miss a shorter peak. Cycles are not converted to nanoseconds
because the counter frequency is platform-defined.

Full sample lists, standard deviations and checksums are in
`benchmarks/memory-model/phase-8/results.json`.
