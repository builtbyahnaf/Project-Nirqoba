# Stage 4 — SoA vs AoS vs AoSoA

## Method

- Compiler `0.371-beta`, existing `build/msvc-dev` configuration, unchanged flags.
- Timing is internal `time.now()` (milliseconds), so allocation, initialisation and
  process startup are outside every measured region.
- Three representations, same data, same operation counts, same checksums:
  - `aos.nrqba` — `memory.owned_array<Particle>`, field access `a[i].x`.
  - `soa.nrqba` — `memory.owned_soa<Particle>`, column access `a[i].x`.
  - `aosoa.nrqba` — `memory.owned_array<Block4>`, block width 4, lanes scanned per block.
- Workloads: sequential single column, sequential two columns, random rows (LCG walk),
  write-heavy (increment one column over every row).
- Sizes: 65,536 rows (200 rounds, cache-resident) and 2,000,000 rows (20 rounds,
  bandwidth-bound). Rounds differ by design; times are not comparable across sizes.
- 5 runs per configuration, fresh process each. `run.py` rebuilds each variant from the
  same source, verifies the checksum is identical across all runs, and reports best and
  median. Raw per-run values are in `RAW.md`.

Checksums equal across representations (e.g. `429490176000`, `1288470528000`, `65935`),
which is the evidence that the same values were read and the same additions performed.

## Results (median ms, best in parentheses)

| rows | workload | AoS | SoA | AoSoA |
|---|---|---|---|---|
| 65,536 | single column | 6 (5) | **2 (2)** | 6 (5) |
| 65,536 | two columns | 9 (8) | **4 (3)** | 9 (9) |
| 65,536 | random rows | **20 (19)** | 22 (21) | n/a |
| 65,536 | write heavy | 18 (13) | **12 (11)** | 13 (11) |
| 2,000,000 | single column | 48 (46) | **15 (14)** | 48 (46) |
| 2,000,000 | two columns | 57 (49) | **32 (28)** | 56 (55) |
| 2,000,000 | random rows | **167 (166)** | 169 (167) | n/a |
| 2,000,000 | write heavy | 71 (70) | **45 (39)** | 70 (68) |

## Interpretation

- **Sequential scans are where SoA wins, and the win grows with the working set.** At
  65,536 rows a single-column scan is about 3x faster (6 → 2 ms); at 2,000,000 rows it is
  about 3.2x (48 → 15 ms). The reason is bytes touched per row: `Particle` is three
  64-bit fields, so a scan of `x` reads one third of the memory an AoS scan does. In the
  bandwidth-bound case that ratio shows up almost exactly; in the cache-resident case the
  absolute times are small and the millisecond clock is coarse, so those ratios are
  indicative rather than precise.
- **Two-column scans narrow the gap** (9 → 4 ms, 57 → 32 ms) because two thirds of the
  bytes are now needed, which is what the byte-count model predicts.
- **Random access is a wash, and slightly worse for SoA** (20 → 22 ms, 167 → 169 ms).
  A random read defeats the cache either way, so the column layout saves nothing, and
  each SoA access pays for the header element-count load plus the two-scale address
  arithmetic. The difference is small at 2,000,000 rows and within run-to-run spread at
  65,536, so this should be read as parity, not as a SoA loss.
- **Writes favour SoA** (18 → 12 ms, 71 → 45 ms): incrementing one column touches one
  third of the bytes, and the bounds check and address arithmetic are already known to
  cost less than the saved traffic at these sizes.
- **AoSoA is not a compromise here.** It tracks AoS almost exactly (6, 9, 13 and 48, 56,
  70 ms). With block width 4 and a 48-byte block, scanning one field of every row still
  pulls the whole block into cache, so blocking adds indirection without reducing the
  bytes that cross the cache line. It would need wider blocks or a streaming pass that
  consumes all fields together to show a gain.

## What this does not show

- One machine, one compiler build, one allocator. No NUMA, no pinning, no frequency
  control, no cross-machine repetition.
- Millisecond resolution: the cache-resident numbers are 2–22 ms, so several of them are
  one or two clock ticks. Treat those ratios as directional.
- `int` is 64-bit in this language, so `Particle` is 24 bytes; in a 32-bit-int language
  the AoS stride would be 12 bytes and the byte-count ratios would be the same but the
  absolute times different.
- Random access for AoSoA is not compared: selecting a lane dynamically needs a branch
  per access, which would measure the branch rather than the layout.
- No profiling counters, so cache-miss counts and vectorisation are inferred from the
  byte-count model, not measured.
- SoA is not universally faster. It wins on sequential column scans and column-local
  writes, ties on random access, and pays a fixed per-access cost for the count load and
  offset arithmetic.

## Reproduce

    python benchmarks/soa_stage4/run.py

Builds every variant from `aos.nrqba`, `soa.nrqba`, `aosoa.nrqba` into `build/ao`,
runs each 5 times, checks checksums and rewrites `RAW.md`.
