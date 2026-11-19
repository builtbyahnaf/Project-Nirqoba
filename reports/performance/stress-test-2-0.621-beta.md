# Nirqoba — Stress Test 2 Results at 0.621-beta

**Scope:** Final measurement of the four Stress Test 2 targets on `0.621-beta`
**Status:** Complete. All four targets met; all 45 benchmark processes validated.
**Date:** 2026-11-20
**Compiler:** `nirqoba compiler version 0.621-beta (Phase 9)`, LLVM/MLIR 23.1.4, RelWithDebInfo (`build/msvc-release/bin/nirqoba.exe`)
**Harness:** `benchmarks/stress_test_2/run_benchmark.py --test 01 --test 02 --test 03 --impl nirqoba --repeats 15`
**Artifact:** `benchmarks/stress_test_2/benchmark_results.json` (`"valid": true`, `"validation_failures": []`)

---

## 1. Result against the targets

| Test | Metric | Target | Median | Mean | Min–Max | Samples | Verdict |
|---|---|---:|---:|---:|---|---:|---|
| 01 managed-object allocation | `ALLOC_MS` | < 5 ms | **4.0** | 4.36 | 3–7 | 75 | met |
| 02 mark and sweep | `BUILD_MS` | < 22 ms | **20.0** | 20.27 | 17–22 | 15 | met |
| 02 mark and sweep | `COLLECT_MS` | < 35 ms | **27.0** | 27.33 | 26–29 | 75 | met |
| 03 cyclic graph reclamation | `BUILD_MS` | < 6 ms | **4.0** | 4.23 | 3–7 | 75 | met |

Each process self-validates before its samples are accepted: every repeat has to report the
expected checksum and the expected live-object count after collection.

| Test | Phase | Checksum | Live after |
|---|---|---|---|
| 01 | allocation + traversal + collection | `499999500000` | `0` |
| 02 | mark and sweep over a 2,097,151-node tree | `2199022206976` | `2097151` |
| 03 | cycle build + reclamation | `499999500000` | `0` |

Test 02 reports 15 build samples and 75 collection samples because `BUILD_MS` is measured once
per process (the tree is built once) while every measurement round re-runs the collection over
that tree. `repeats = 15` therefore gives 15 independent build samples, which is the sample
count the target needed.

## 2. Host and toolchain

Recorded by the harness into the artifact's `metadata` block:

| Field | Value |
|---|---|
| Host | `DESKTOP-VO51V7J`, Windows 11 Pro `10.0.26300`, AMD64 |
| CPU | 12th Gen Intel(R) Core(TM) i5-1235U, 12 logical processors |
| Memory | 16,890,978,304 bytes |
| Compiler | `nirqoba compiler version 0.621-beta (Phase 9)` |
| MSVC | 19.51.36260 x64 |
| rustc | 1.99.1 |
| Java | openjdk 17.0.20.1 |
| Git revision | `1030bd57dea0a3ee8d9c77b17a8885eba75e0762` (`dirty: true`, the round's own edits) |

The machine is a laptop on the Windows `Balanced` power plan with Defender active, so single
samples wander (see the min/max columns). Every number above is a median over the sample count
in its row, which is the same convention the earlier rounds used.

## 3. What changed since 0.617-beta

The round landed one lowering change and rejected one experiment, both measured interleaved
A/B on the same host (10 runs per arm):

- **Root-slot coalescing (`4d0f6c8`):** a fresh `var x = array.allocate<T>(n)` publishes
  through the local's own rooted slot instead of a synthetic slot, so the frame carries one
  slot instead of two and the reference is stored once. Measured neutral: test 02
  `BUILD_MS` 21.5 -> 21.5 ms, `COLLECT_MS` 32.0 -> 31.5 ms. It is kept because it removes a
  store and a barrier from every initialising allocation, not because it moves the median.
- **`HotColdSplittingPass` in `OptimizerLastEP`:** reverted. The same A/B returned identical
  build medians (25.0 vs 25.0 ms), so the extra pass bought nothing.

The gap between the 0.617-beta report's older absolute numbers and this table is the workload
harness itself (process-per-measurement, 15 repeats, validated samples), not a workload change;
the workloads and their checksums are identical.

## 4. Where the remaining build time goes

Disassembly of `@build_tree` in test 02 shows the build median is dominated by the
~1M leaf calls and the ~1M internal calls, not by allocation anymore: the inline TLAB bump
already covers the fast path, and roughly 6 ms of the 20 ms is the fresh-page zeroing of the
64 MB the tree occupies at ~11 GB/s. Further build gains therefore require cutting
per-node instructions rather than tuning the allocator, which is the direction recorded for the
next round.
