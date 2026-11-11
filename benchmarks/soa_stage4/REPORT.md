# Stage 4 — SoA vs AoS vs AoSoA, with cycles and codegen

## Method

- Compiler `0.371-beta`, existing `build/msvc-dev` configuration, unchanged flags.
- **Timing is `time.cycles()`** (`__rdtsc` deltas), added for this evaluation because the
  millisecond clock was too coarse to separate cache-resident scans: at 2–20 ms several
  results were one or two ticks. Allocation, initialisation and startup remain outside
  the measured region.
- Three representations, same values, same additions:
  - `aos.nrqba` — `memory.owned_array<Particle>`, `a[i].x`.
  - `soa.nrqba` — `memory.owned_soa<Particle>`, `a[i].x`.
  - `aosoa.nrqba` — `memory.owned_array<Block4>`, block width 4, lanes scanned per block.
- Workloads: sequential single column, sequential two columns, LCG random-row walk,
  column-local write. Sizes: 65,536 rows x 200 rounds and 2,000,000 rows x 20 rounds.
- 5 fresh-process runs per configuration; best and median recorded in `RAW.md`, including
  cycles per visited row.
- Checksums are identical across representations per workload (`429490176000`,
  `1288470528000`, `65935`, `39999980000000`, `119999940000000`, `39782300757120`,
  `2000039`), which is the evidence that the same values moved and the same arithmetic ran.

## Results (median cycles per visited row; best in parentheses)

| rows | workload | AoS | SoA | AoSoA |
|---|---|---|---|---|
| 65,536 | single column | 1.23 (1.10) | **0.47 (0.44)** | 1.10 (0.93) |
| 65,536 | two columns | 1.83 (1.74) | **0.79 (0.75)** | 1.75 (1.59) |
| 65,536 | random rows | **3.98 (3.75)** | 4.64 (4.35) | n/a |
| 65,536 | write heavy | **2.01 (1.75)** | 2.33 (2.14) | 1.86 (1.66) |
| 2,000,000 | single column | 3.87 (3.27) | **1.17 (1.10)** | 3.44 (3.32) |
| 2,000,000 | two columns | 4.01 (3.90) | **2.13 (2.09)** | 4.44 (3.94) |
| 2,000,000 | random rows | **11.35 (10.78)** | 12.07 (11.00) | n/a |
| 2,000,000 | write heavy | 6.02 (5.46) | **2.77 (2.65)** | 5.73 (5.54) |

Absolute totals (median) are in `RAW.md`; cycles per row divides by rows x rounds.

## What changed versus the millisecond pass

The coarse clock pointed the wrong way on one result. It reported SoA writes as faster at
65,536 rows (18 ms vs 12 ms); at cycle resolution **AoS is faster there** (2.01 vs 2.33
cycles/row). The millisecond numbers were 11–18 ticks apart and dominated by tick
granularity and run-to-run spread. This is the practical reason the counter was added: the
first pass could not distinguish a real 15% effect from clock noise.

## Vectorisation, measured from emitted IR

`vectorization.py` compiles each representation and counts vector operations in
`--emit=llvm` output (`VECTORIZATION.md`):

| representation | vector types | vector loads | vector reduces |
|---|---|---|---|
| AoS | 0 | 0 | 0 |
| SoA | 23 | 6 | 3 |
| AoSoA | 0 | 0 | 0 |

SoA's three reduction loops vectorised; AoS's and AoSoA's did not. That is a measured
codegen difference, not an inference from timings — though the counts are file-level, so
they include the initialisation loops, and the honest claim is "the SoA scans vectorised
and the others did not", not a per-loop attribution.

## Interpretation

- **Sequential scans: SoA wins, and the margin tracks bytes touched plus vectorisation.**
  2.6x at 65,536 rows and 3.3x at 2,000,000 for a single column. `Particle` is three
  64-bit fields, so a one-column scan touches one third of AoS's bytes; the reduction also
  vectorises, which is why the large-case ratio slightly exceeds the byte ratio.
- **Two columns narrow the gap as the byte model predicts** (1.83 vs 0.79; 4.01 vs 2.13).
- **Random access favours AoS at both sizes** (3.98 vs 4.64; 11.35 vs 12.07). Misses
  dominate, the layout saves nothing, and every SoA access still pays the header
  element-count load plus two-scale address arithmetic. Unlike the millisecond pass, the
  cycle data resolves this as a real, repeatable ~10–15% cost rather than noise.
- **Writes split by working set.** Cache-resident, AoS is faster (2.01 vs 2.33): the
  one-third byte saving does not yet repay the per-access addressing, and the store loop
  does not vectorise. Bandwidth-bound, SoA wins clearly (6.02 vs 2.77) because the saved
  traffic dominates. The same operation therefore has opposite winners at two sizes, which
  is exactly the kind of claim a single benchmark cannot support.
- **AoSoA is not a compromise at width 4.** It tracks AoS within 10% throughout (a 48-byte
  block means scanning one field still pulls the whole block) and shows no vectorisation.

## What this does not show

- One machine, one build, one allocator; no pinning, no frequency control, no cross-machine
  repetition.
- `__rdtsc` is a reference counter, not core cycles: ratios are sound, absolute cycles per
  row are not directly comparable across CPUs or turbo states. No frequency calibration was
  applied.
- Vector counts are per compilation unit, including initialisation, not per loop.
- No hardware cache-miss counters. Those need PMU access (ETW/PDH on Windows or an external
  tool such as `perf`/VTune); nothing here measures miss counts, and none are claimed. The
  cache reasoning above rests on bytes touched and the observed codegen.
- `int` is 64-bit here, so `Particle` is 24 bytes; a 32-bit-int language would keep the
  ratios and change the absolutes.
- AoSoA random access is not compared: dynamic lane selection needs a branch per access.
- No claim that SoA is universally faster: it loses on cache-resident writes and on random
  access, and ties only in the sense that both are memory-bound.

## Reproduce

    python benchmarks/soa_stage4/run.py
    python benchmarks/soa_stage4/vectorization.py

`run.py` rebuilds each variant from `aos.nrqba` / `soa.nrqba` / `aosoa.nrqba`, runs each 5
times, verifies checksums and rewrites `RAW.md`. `vectorization.py` rewrites
`VECTORIZATION.md`.
