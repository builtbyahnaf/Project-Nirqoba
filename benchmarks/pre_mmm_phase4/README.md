# Pre-MMM Phase 4 — Owned allocation benchmarks

These workloads measure the explicit ownership runtime, not the tracing GC.
Each program times only its workload with `time.cycles()` and prints a checksum
followed by the cycle count. The runner builds with two compilers, alternates
their samples, and rejects any checksum drift.

## Workloads

| Workload | Operations | Shape |
|---|---:|---|
| `allocation_heavy` | 1,000,000 | One dynamic 64-byte `memory.allocate` / `memory.deallocate` pair per iteration, with both ends written and read. |
| `destruction_heavy` | 300,000 | Nested owned aggregates and owned arrays, an early-return helper, and a conditional defer with both explicit and automatic release paths. |
| `mixed` | 200,000 | Small explicit allocation, a 256-element owned array, periodic 64 -> 4096 reallocation, and a for-initializer owner. |

The block size in the allocation-heavy program comes from `memory.cpu_count()`
rather than a constant. A literal size was stack-placed and folded by the
optimizer, which made an earlier version measure an almost empty loop.

## Run

```bash
python benchmarks/pre_mmm_phase4/run.py \
  --baseline path/to/0.461-beta/nirqoba.exe \
  --current path/to/0.465-beta/nirqoba.exe \
  --runs 9 \
  --json benchmarks/pre_mmm_phase4/results.json
```

`results.json` stores every sample plus a Windows peak working-set measurement
for one baseline and one current run.

## Results

Windows 11 x64, MSVC 14.51, `RelWithDebInfo`, LLVM 23.1.4, 9 alternating
samples per workload.

| Workload | 0.461 median cycles | 0.465 median cycles | Change | Cycles/op 0.461 -> 0.465 |
|---|---:|---:|---:|---:|
| allocation_heavy | 182,829,660 | 131,792,268 | **-27.9%** | 182.83 -> 131.79 |
| destruction_heavy | 125,627,815 | 100,470,479 | **-20.0%** | 418.76 -> 334.90 |
| mixed | 61,573,000 | 49,748,902 | **-19.2%** | 307.87 -> 248.74 |

Checksums were identical in every sample: `249996480`, `212766730918`, and
`60027437998`.

Peak working set stayed effectively flat:

| Workload | 0.461 | 0.465 | Difference |
|---|---:|---:|---:|
| allocation_heavy | 10,362,880 B | 10,350,592 B | -12,288 B |
| destruction_heavy | 10,391,552 B | 10,395,648 B | +4,096 B |
| mixed | 10,407,936 B | 10,424,320 B | +16,384 B |

The intermediate upgrade (sharded registry with node-based shard maps) was
measured separately against 0.461:

| Workload | 0.461 median | sharded-map median | Change |
|---|---:|---:|---:|
| allocation_heavy | 190,739,144 | 184,955,344 | -3.0% |
| destruction_heavy | 134,131,623 | 136,894,695 | +2.1% |
| mixed | 62,988,287 | 64,814,252 | +2.9% |

That upgrade is a concurrency change, not a single-thread win. The final
open-addressed table supplies the single-thread improvement.

## Concurrent default allocation

Measured with `AllocatorTest.BenchmarksDefaultAllocationContentionAcrossThreads`
(10,000 allocate/write/free pairs per worker, median of five runs):

| Threads | 0.461 | sharded-map upgrade | 0.465 |
|---:|---:|---:|---:|
| 1 | 1,378 us | 1,388 us | **1,248 us** |
| 2 | 1,715 us | 1,093 us | **996 us** |
| 4 | 4,836 us | 1,934 us | **1,563 us** |

At four workers the final build is about **3.1x** faster than 0.461; the
sharded-map upgrade alone was about **2.5x** faster. The single-thread case is
neutral for the sharded-map upgrade and about 10% faster after open addressing.

## Interpretation

The registry no longer allocates a hash node to describe each live pointer, and
ordinary default allocations/releases no longer take the global allocator mutex
or an operation pin. Independent threads now contend on one of eight shards
instead of one registry mutex. The remaining serialized path is an explicit
per-allocation allocator handle, which still needs the allocator lifetime pin.
