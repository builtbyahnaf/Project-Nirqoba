# AOR Phase 6-alpha: pool release and generation tracking

Scope: audit the Phase 5 pool release path, separate its costs in the benchmark,
find what is actually expensive, and make it cheaper without dropping a single
safety check.

Non-goal, stated because it determines every decision below: throughput was not
purchased with semantics. Generation tracking, poisoning, exact-slot validation,
double-release detection and deterministic destruction all still run on every
release.

## What the release path did

`nrq_cdmm_pool_release(pool, slot)` had four jobs:

1. validate the address — the slot must belong to this pool and sit exactly on a
   slot boundary;
2. validate the generation when the pool carries one, so a stale pointer to a
   reused slot is refused;
3. run the pool's destructor on the payload;
4. poison the slot (debug) and link it back into the free list.

Jobs 1, 2 and 4 were cheap. What was not cheap was finding the slot in the first
place: the pool kept one `std::vector<Chunk>` and located a slot's chunk by
scanning chunks in order and testing each chunk's address range. With small
objects a 200k-object pool has hundreds of chunks, and a release near the end of
the pool paid that scan on every call. Bulk release hid it; individual release
did not.

The second cost was the free list. One pool-wide head meant every release wrote
the same four words, so releases from different chunks serialized on one cache
line even when they had nothing else in common.

The third cost was reset. `nrq_cdmm_pool_reset` rebuilt every slot: clear the
bitmap, poison, rewrite the next-free link. A pool that was mostly free paid that
for slots nobody had touched.

## What changed

- **Sorted chunk index.** `chunk_order_` holds chunk pointers sorted by base
  address, and the chunk for a slot is found with `std::upper_bound` instead of a
  linear scan. The index is rebuilt on growth, which happens once per chunk, not
  once per release.
- **Per-chunk free lists.** `Chunk` gained `free_head`, `free_count` and
  `in_free_set`; the pool keeps `chunks_with_free_`. A release touches its own
  chunk's list, and reap only visits chunks that have something to give.
- **Reset visits live slots only.** Reset walks the liveness bitmap with
  `std::countr_zero` and handles the set bits (poison, unlink, destructor). Chunks
  whose `live` count is zero are skipped entirely apart from rewinding the
  allocator state they already have.
- **`nrq_cdmm_pool_scope_release_checked`.** A scope can now release one slot and
  validate its generation without taking the handle lock, so the benchmark can
  measure generation validation itself rather than handle lookup plus validation.
- **Debug generation seed.** `nrq_cdmm_pool_seed_generation` exists only under
  `NRQ_RUNTIME_DEBUG_ENABLED`. It lets a unit test walk a slot's generation past
  `UINT32_MAX` instead of allocating 2^32 objects. It is a test seam: release,
  validate and wraparound comparison are unchanged.

Nothing was removed. Double release is still refused by the liveness bit, a
foreign or interior address is still refused by exact-slot validation, generations
are still compared when present, destructors still run newest-first on reset and
destroy, and released slots are still poisoned before being linked.

## Measurements

Pool benchmark, 200 000 objects, 9 runs, median of per-run means, development
runtime (poisoning and validation enabled), i5-1235U:

| Workload | Before | After | Change |
|---|---|---|---|
| individual release, no generations | 61.98 ns/op | 16.02 ns/op | 3.9× faster |
| individual release, generations | 63.83 ns/op | 15.08 ns/op | 4.2× faster |
| individual release, generations validated in a scope | 93.28 ns/op | 14.21 ns/op | 6.6× faster |
| pool reset, whole pool | 7.69 ns/slot | 5.90 ns/slot | 1.3× faster |
| pool reset, 10 000 touched slots | 58.8 µs total | 33.0 µs total | 1.8× faster |
| pool allocation | unchanged | unchanged | — |

Variance after the change, individual release with generations, 9 runs:
14.70–20.95 ns/op. Before the change the same workload ranged 61.2–64.9 ns/op, so
the improvement is far outside the run-to-run spread.

The stale-check row is not a like-for-like comparison: before it used the
registry-locking API, after it uses the scope API. The honest reading is that
generation validation now costs about nothing *when the caller already holds a
scope*; a caller without a scope still pays the handle lookup, and that is why the
non-scope API was left as it was.

Reset-partial is the row that shows the live-only rewrite: the per-touched-slot
figure falls from 5.88 to 3.33 ns, and the total falls by the same factor because
untouched slots are no longer visited.

## What was deliberately not done

- No change to the slot metadata layout. Shrinking the liveness bit or the
  generation word would have saved bytes, not time, and would have changed the
  generation type the tests and API describe.
- No removal of the release-time poison, even though it is a store per release in
  the debug runtime, because poisoning is what turns a use-after-release into an
  immediate, located failure.
- No attempt to make the non-scope release path lock-free. Measurement showed the
  scan, not the lock, was the cost; removing the lock after fixing the scan would
  be an unmeasured change to the concurrency contract.

## Reproduction

```
nirqoba_phase5_benchmark 200000 9
```

Rows: `pool-release-plain`, `pool-release-gen`, `pool-release-stale-check`,
`pool-reset-64B`, `pool-reset-partial`.

## Limitations

- One machine, one compiler, one configuration; no cross-platform numbers.
- The development runtime is measured, so poisoning and validation are inside the
  timed regions. A release build would show different absolute numbers.
- The before figures were taken on the same machine with the same benchmark
  harness immediately before the rewrite, but in a separate process run; they are
  not paired per-iteration samples.
