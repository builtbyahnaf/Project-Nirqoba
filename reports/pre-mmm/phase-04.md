# Pre-MMM Phase 4 — Owned allocator optimization

**Status:** Complete
**Version:** `0.465-beta`
**Started from:** `0.461-beta`, commit `487aa93`
**Date:** 2026-11-13

## Scope

Phase 4 optimizes the explicit deterministic ownership runtime. It does not
change the tracing GC's default status, ownership transfer rules, exactly-once
destruction, or the allocation/deallocation API. The work was limited to the
registry and the default-allocation fast path; no language or cleanup semantics
were changed.

## Bottleneck found before modification

The owned allocation registry used one process-wide `allocator_mutex` and one
`std::unordered_map<void*, AllocationOwner>`. Every default allocation and
deallocation:

1. took the global allocator mutex,
2. took an operation pin so the allocator could not be destroyed,
3. allocated or freed an `unordered_map` node to describe the pointer, and
4. re-read the same global state.

The thread-local small-allocation cache already removed the underlying
`HeapAlloc`/`HeapFree` traffic for repeated 64-byte blocks, but it did not remove
the registry node or the global mutex. The benchmark therefore isolated the
bookkeeping path rather than the general heap.

A second issue was profiling: allocation counters were updated on every
allocation and profiler snapshots needed a consistent view across threads.

## Upgrade 1 — Sharded registry and default fast path

Commit: `9eebb88` (`allocator: shard ownership registry and bypass the default global path`)

### Implementation

- The registry is split into eight `AllocationShard` objects, selected by the
  pointer's address bits. Each shard has its own mutex and condition variable.
- Records are deleted or replaced under the shard lock, never under the global
  allocator mutex.
- Ordinary process-default allocations and releases skip both
  `allocator_mutex` and the operation pin. The default general allocator is
  process-owned and cannot be destroyed or reset, so the pin is unnecessary on
  that path.
- Explicit per-allocation handles still take `allocator_mutex` and the pin,
  because those allocators can be reset or destroyed.
- Reallocation finds the old shard, allocates the replacement, then moves the
  ownership record between shards with ordered shard locks.
- The profiler has an atomic enable flag and a dedicated profile mutex. Counters
  remain exact while a snapshot is taken.
- `AllocationOwner` records whether an allocation is shared, so deallocation can
  choose the shard path without re-reading mutable global state.

### Tests added

- `RegistryHandlesBulkChurnAndKeepsProfilingExact` — 20,000 live blocks,
  reverse release, exact allocation/deallocation counts.
- `RegistryMovesReallocationsAcrossShardsWithoutLosingOwnership` — 4,096
  cross-shard reallocations with payload checks.
- `DefaultRegistrySupportsCrossThreadBulkOwnership` — 8 allocating threads and 8
  different releasing threads, 16,000 blocks, exact profiler totals.
- `BenchmarksDefaultAllocationContentionAcrossThreads` — 1/2/4-worker default
  allocate/write/free benchmark used for the measurements below.

### Measured effect

Single-thread workload medians were essentially neutral and slightly negative
on two workloads: allocation -3.0%, destruction +2.1%, mixed +2.9%. The change is
a concurrency improvement, and that is where it was measured:

| Threads | 0.461 | Upgrade 1 | Speedup |
|---:|---:|---:|---:|
| 1 | 1,378 us | 1,388 us | 0.99x |
| 2 | 1,715 us | 1,093 us | 1.57x |
| 4 | 4,836 us | 1,934 us | 2.50x |

The single-thread regression was not hidden: the second upgrade removes the
node allocation that caused it and produces a net single-thread win.

## Upgrade 2 — Inline open-addressed ownership records

Commit: `3af03be` (`allocator: store ownership records inline in open-addressed shards`)

### Implementation

- Each shard now owns a compact linear-probing table instead of an
  `unordered_map`.
- Records are stored inline in the table, so a live pointer no longer causes a
  separate hash-node allocation and release.
- Erasure leaves a tombstone. Insertion reuses the first tombstone on the probe
  path, which keeps one-at-a-time churn from growing the table.
- The table grows only when live records exceed 70% of capacity. When a mostly
  empty table would otherwise keep probing a large tombstone field, insertion
  compacts it to the smallest capacity that holds the live set. This bounds
  high-water memory after bulk release.
- Probing, growth, compaction, and iteration remain under the owning shard's
  mutex. No lock-free publication or borrowed pointer escapes a shard.

### Tests added/covered

The bulk-churn, cross-shard reallocation, and cross-thread tests from Upgrade 1
run against the new table. The migration-scale cleanup programs and the full
integration suite exercise the table through normal language allocation and
destruction paths.

## Final measurements

Windows 11 x64, MSVC 14.51, `RelWithDebInfo`, LLVM 23.1.4, 9 alternating
baseline/current samples per workload.

| Workload | 0.461 median cycles | 0.465 median cycles | Change | Cycles/op |
|---|---:|---:|---:|---:|
| allocation_heavy | 182,829,660 | 131,792,268 | **-27.9%** | 182.83 -> 131.79 |
| destruction_heavy | 125,627,815 | 100,470,479 | **-20.0%** | 418.76 -> 334.90 |
| mixed | 61,573,000 | 49,748,902 | **-19.2%** | 307.87 -> 248.74 |

Means agree with the medians: allocation 183.44M -> 133.01M (-27.5%),
destruction 126.26M -> 101.91M (-19.3%), mixed 61.85M -> 49.89M (-19.3%).
Every sample produced the same checksums, so the speedup is not a skipped-work
artifact.

### Upgrade 2 isolation

Measured against the sharded-map intermediate commit:

| Workload | Sharded map median | Open-addressed median | Change |
|---|---:|---:|---:|
| allocation_heavy | 184,681,696 | 136,529,196 | **-26.1%** |
| destruction_heavy | 137,278,136 | 108,437,977 | **-21.0%** |
| mixed | 65,238,609 | 53,033,660 | **-18.7%** |

This isolates the inline records: they remove the node allocation that made the
sharded-map upgrade slightly slower than 0.461 on single-thread destruction and
mixed workloads.

### Memory

Peak working set, one run each:

| Workload | 0.461 | 0.465 | Difference |
|---|---:|---:|---:|
| allocation_heavy | 10,362,880 B | 10,350,592 B | -12,288 B |
| destruction_heavy | 10,391,552 B | 10,395,648 B | +4,096 B |
| mixed | 10,407,936 B | 10,424,320 B | +16,384 B |

The table's eight shard arrays add a small fixed cost; the final runs stayed
within 16 KB of the baseline in both directions. The initial
tombstone-heavy implementation caused a temporary 10 MB -> 73 MB growth because
freed slots counted toward the load factor; that was found by the memory probe
and fixed with tombstone reuse plus compaction before the final measurement.
The regression is not present in the committed version.

## Correctness and stress verification

| Check | Result |
|---|---|
| Unit suite | 225/225 |
| Integration suite | 450/450 |
| Migration-scale Phase 3 programs | PASS (`5513361`, `252200`) |
| Cross-thread bulk ownership | PASS |
| Bulk churn with exact profiler totals | PASS |
| Cross-shard reallocation payload preservation | PASS |
| Concurrent explicit allocator tests | PASS |
| Default allocation contention benchmark | PASS |
| Repeated output/checksum stability | PASS |

The public allocator tests cover general heap concurrency, stack/region lifetime
rules, secure zeroing, native/runtime mappings, and profiler accounting. The new
tests extend the same checks to the default registry and shard transitions.

## Residual limitations

- **Explicit per-allocation handles remain globally serialized.** They take
  `allocator_mutex` and an operation pin because the owning allocator can be
  reset or destroyed. Removing that needs a refcounted allocator lifetime, which
  is outside Phase 4's measured scope.
- **Thread-local caches are unchanged.** The existing 64-entry / 1,024-byte
  cache still serves small default allocations; larger blocks go directly to the
  general heap. No evidence justified changing the thresholds.
- **Table capacity follows the high-water live set.** Compaction handles the
  mostly-empty case, but a workload that keeps many records live retains the
  corresponding shard arrays until they are released.
- **Measurements are single-machine.** Cycle counts are useful for relative
  comparison only; the workload runner records raw samples so other machines can
  reproduce them.
- **No sanitizer run.** The MSVC configuration has no ASan pass. Evidence is the
  full suites, exact profiler assertions, cross-thread stress, and migration
  programs.
- **The known pre-existing GC stress flake remains.** During final clean
  verification, `GcTest.MinorReclaimsOnlyYoungGarbageAndKeepsOldGarbage`
  segfaulted in 2 of 4 direct full-suite runs. The final direct run passed
  225/225, the test passes in isolation, and five full runs under gdb passed. It
  is the same test recorded in Phase 3 and predates this work; no allocator test
  or ownership path is involved. It is not fixed here and remains separate GC
  test-hardening work.
- **No multi-thread language-level allocation workload.** Concurrency is
  measured through the focused C++ allocator benchmark; the language integration
  programs are single-threaded. This is called out rather than extrapolated.

## Conclusion

Two independent upgrades produced a measured single-thread improvement of
19-28% across allocation-heavy, destruction-heavy, and mixed workloads, plus a
2.5-3.1x improvement in concurrent default allocation, with flat peak memory.
Deterministic ownership, exactly-once destruction, checksums, and the full
correctness suites are unchanged. The next allocator step, if taken, is the
refcounted lifetime needed to remove the explicit-handle global pin.
