# AOR Phase 5 — arena and pool performance

**Measured:** 18 November 2026  
**Host:** Intel Core i5-1235U, 16 GB, Windows 11 Home 26100  
**Compiler:** MSVC 19.51, x64, `RelWithDebInfo`  
**Runtime:** development runtime (`nirqoba_runtime_debug`), the library the unit
tests exercise  
**Benchmark:** `benchmarks/aor/phase5/phase5_benchmark.cpp`, 200,000 objects,
9 runs, median reported

The benchmark measures wall-clock time per object. It does not measure pause
times, and it does not derive a combined score.

## Results

| Workload | Allocate ns/op | Release ns/op | Payload B/obj | Metadata B/obj | Chunks | Committed |
|---|---:|---:|---:|---:|---:|---:|
| owned-64B | 174.69 | 135.77 | 64.0 | — | — | — |
| owned-churn-500 | — | 124.92 | 64.0 | — | — | — |
| arena-64B | 23.99 | 5.07 | 64.0 | 0.0 | 196 | 12,845,056 |
| arena-destructible | 39.90 | 14.00 | 64.0 | 32.0 | 294 | 19,267,584 |
| arena-mixed-size | 58.23 | 11.14 | 159.3 | 0.0 | 487 | 31,916,032 |
| arena-churn-500 | — | 4.13 | 64.0 | 0.0 | 1 | 65,536 |
| pool-64B | 27.95 | 66.70 | 64.0 | 0.0 | 197 | 12,857,404 |
| pool-generations | 29.89 | 63.96 | 64.0 | 4.1 | 197 | 12,857,404 |
| pool-reset-64B | — | 5.87 | 64.0 | 0.0 | 197 | 12,857,404 |
| pool-churn-500 | — | 15.88 | 64.0 | 5.0 | 1 | 65,599 |

`arena-churn-500` and `pool-churn-500` time allocation plus release together
(400 rounds of a 500-object working set, all in one row) because the frame
boundary is part of the workload. The `owned-churn-500` row is the same shape with
ordinary owned allocation.

Every run of every workload produced a destructor-call count equal to the object
count; the benchmark exits non-zero otherwise.

## What the numbers mean

**Allocation.** An arena allocation is one alignment calculation and a bump of the
chunk pointer inside a scope: no registry lookup, no pin, no lock. The 24 ns/op in
`arena-64B` is almost entirely first-touch faults on the 12.8 MB the run commits;
the `arena-churn-500` row, which reuses one 64 KiB chunk for 200,000 allocations,
is 4.13 ns/op for allocate-and-reset together. `owned-64B` pays for the
allocator registry record, its shard lock and its profiler accounting on every
object, which is what puts it at 174.69 ns/op.

**Bulk release.** `arena_reset` is O(chunks): it runs the destructor list and
releases growth chunks, so 200,000 objects cost 5.07 ns each and the work does not
grow with the object count. `pool_reset` reclaims every live slot the same way at
5.87 ns/op. Releasing a pool slot one at a time costs 66.70 ns/op, nearly all of it
touching a cold 64-byte slot 12.8 MB apart from the previous one — that path exists
for deterministic per-slot release, not for bulk teardown.

**Destructors.** `arena-destructible` adds 32 bytes per object and about 14 ns per
reset: the record costs one extra alignment+bump at allocation (16 ns/op) and one
indirect call per object at reset. That is the price of deterministic cleanup in a
region that would otherwise not touch its objects at all.

**Mixed sizes.** `arena-mixed-size` cycles 16 to 512 bytes, averaging 159.3 bytes
of payload with one alignment calculation per object; 58.23 ns/op and 31.9 MB
committed for 31.9 MB of payload is the expected result for a bump allocator with
no size classes.

**Generations.** A pool with generations allocates and releases within measurement
noise of one without (29.89 vs 27.95 ns/op allocate, 63.96 vs 66.70 release) and
costs 4.1 bytes of metadata per slot against 0.0 (the figure rounds to zero because
a liveness bit per 64-byte slot is 0.125 bytes; `facts` reports the amortized byte
count, which truncates).

## Derived comparison

From the enclosed run, not from a separate measurement:

| Comparison | Ratio |
|---|---|
| Arena allocation vs owned allocation (64 B) | 7.3× faster |
| Arena reset vs owned release (200,000 objects) | 26.8× faster |
| Pool allocation vs owned allocation | 6.3× faster |
| Pool reset vs owned release | 23.1× faster |
| Arena frame churn vs owned churn | 30.2× less time per object |
| Pool frame churn vs owned churn | 7.9× less time per object |

Byte overhead for a 64-byte object: arena 0, pool 0 (or 4.1 with generations),
destructible arena object 32. For comparison, the Phase 4 numbers put a 64-byte
managed object at 0 header bytes in H0 and 5 bytes of total overhead; the explicit
strategies reach the same zero for plain objects by keeping metadata in the chunk.

## Limitations

- One machine, one compiler, one runtime configuration. Nothing here was measured
  on the production runtime, which differs only in internal checks.
- Committed bytes include the chunk headers and alignment slack; the arena figure is
  the peak before reset, because reset releases growth chunks.
- `owned-64B` includes the runtime's allocation registry and profiler accounting,
  which is the real cost of the ordinary path but is not representative of a raw
  `malloc`.
- The benchmark does not measure contention. The concurrency path is covered by
  `CdmmStressTest.ConcurrentMutatorsShareOneArenaAndOnePool`, not timed.
- Pool release numbers are memory-bound on a 12.8 MB working set; a smaller working
  set measures far cheaper (`pool-churn-500` at 15.88 ns/op) and a larger one
  worse. Neither number generalizes beyond the working set it was measured on.
