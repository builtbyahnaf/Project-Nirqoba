# Phase 9 benchmarks — before and after

**Baseline version:** `0.583-beta`  
**Final version:** `0.591-beta`  
**Runner:** `benchmarks/memory-model/phase-9/run_benchmark.py`  
**Host:** x86_64-pc-windows-msvc, RelWithDebInfo runtime, MSVC `/O2` harness  
**Date:** 2026-11-17

---

## 1. Method

Every mode runs in a fresh process. The runner preserves every sample and reports
median, mean, minimum, maximum and population standard deviation. Checksums must
agree across samples. Peak RSS is process-wide and sampled by the harness at the
end of each measured mode.

The pre-optimization baseline used 9 samples; the final run used 15 samples. The
medians are comparable as a direction, but the different sample counts are a
limitation. The baseline's retained-allocation and release modes had much wider
spreads than the final run, so those two comparisons are weaker than the others.

No target was set before measuring. No comparison to C++, Rust or Java is made.

---

## 2. Results

### Owned and manual

| Measurement | Baseline | Final | Change |
|---|---:|---:|---:|
| Owned allocate/release | 58.499 ns/op | 58.553 ns/op | +0.1% |
| Owned retained allocation | 215.929 ns/op | 182.296 ns/op | **-15.6%** |
| Owned release of retained set | 108.320 ns/op | 129.552 ns/op | **+19.6%** |
| Manual-path allocate/release | 61.021 ns/op | 58.608 ns/op | -4.0% |
| Borrowed read | 0.073 ns/read | 0.073 ns/read | unchanged |
| Retained allocate + release | 324.249 ns/op | 311.848 ns/op | **-3.8%** |
| Retained-set peak RSS | 45,158,400 B | 41,881,600 B | **-7.3%** |

The retained allocation improvement comes from removing the alignment prefix.
The release regression is real in this run: a direct 72-byte heap block is freed
instead of a larger prefixed block, and Windows heap free/coalescing cost rose
for this size. The combined retained-set cost still improved, and peak memory
fell. Release is not claimed to be faster.

### Arena and pool

| Measurement | Baseline | Final | Change |
|---|---:|---:|---:|
| Arena allocation | 48.856 ns/op | 45.272 ns/op | -7.3% |
| Arena reset | 194.400 ns/op | 185.700 ns/op | -4.5% |
| Pool allocate/release | 152.806 ns/op | 133.137 ns/op | -12.9% |
| Arena peak RSS | 17,043,456 B | 17,006,592 B | -0.2% |
| Pool peak RSS | 9,154,560 B | 9,134,080 B | -0.2% |

The arena/pool code was not changed. These changes reflect the shared allocator
path, run-to-run variance, and host state; they are not evidence of an arena or
pool algorithm redesign.

### Shared

| Measurement | Baseline | Final | Change |
|---|---:|---:|---:|
| Shared allocate + 8 retain/release | 644.529 ns/op | 592.663 ns/op | -8.1% |
| 4-thread shared retain/release | 76.474 ns/op | 69.710 ns/op | -8.8% |
| Peak RSS | 8,986,624 B | 8,970,240 B | -0.2% |

The shard-lock/atomic protocol was not changed. The contention result is one
host scheduling sample; it is not a linearizability or real-time claim.

### GC

| Measurement | Baseline | Final | Change |
|---|---:|---:|---:|
| Managed allocation | 56.254 ns/op | 53.979 ns/op | -4.0% |
| Full collection (500k garbage) | 6,393,800 ns | 5,464,300 ns | -14.5% |
| Peak RSS | 50,921,472 B | 50,888,704 B | -0.06% |

GC code was not changed. The collection sample has substantial variance
(`±321,476 ns` standard deviation in the final run) and is a single full cycle,
not a pause distribution. No GC improvement is claimed.

### Mixed

| Measurement | Baseline | Final | Change |
|---|---:|---:|---:|
| Five-strategy loop | 109.906 ns/op | 99.784 ns/op | -9.2% |
| Peak RSS | 10,809,344 B | 10,784,768 B | -0.2% |

---

## 3. Interpretation

- **Supported:** default-aligned ordinary blocks no longer carry an alignment
  prefix; retained allocation and peak RSS improved, and the combined retained
  allocate+release cost improved.
- **Reported regression:** retained-set release alone is slower. The benchmark
  does not hide it. A workload dominated by releasing already-allocated 72-byte
  blocks should be measured independently.
- **Not claimed:** arena, pool, shared and GC algorithm improvements. Those
  measurements were taken with unchanged code and include host variance.
- **Not claimed:** superiority over another language. No equivalent cross-language
  workload was run.

The benchmark is evidence about these paths, not proof of memory safety. The
correctness evidence is the unit suite, the full runner, the Phase 9
O0/O2/production/debug matrix and the Phase 7 isolation script.

---

## 4. Reproduction

```text
python benchmarks/memory-model/phase-9/run_benchmark.py --runs 15 \
  --out benchmarks/memory-model/phase-9/results.json
```

Raw samples, standard deviations, checksums, CPU/OS metadata and the compiler
version are in `results.json` and `results-baseline.json`. Build artifacts stay
in the ignored `build/` directory.
