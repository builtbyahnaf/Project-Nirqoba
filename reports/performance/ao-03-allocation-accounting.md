# AO-03 — Allocation accounting: exact statistics with fewer operations

**Base commit:** `c48f15b` (0.298-beta released, AO-02 closed)
**Result commits:** `f27ca8d` (AO-02/6, credited here), `7810b21`
**Configuration:** production runtime (`NRQ_RUNTIME_DEBUG_ENABLED=0`), MSVC 18,
RelWithDebInfo + `/Ob2`, Windows x64, 1,000,000-object local probe.
**Labels:** MEASURED / SOURCE / PROJECTION. No statement below is a timing result
unless labelled MEASURED with its sample count.

## 1. Scope

The roadmap for this stage is an audit with a narrow licence: reduce redundant
loads and stores *without changing what any counter means*, and never trade the
"statistics are exact immediately" contract for speed. Delaying public statistics
requires owner approval and is explicitly out of scope here. The stage therefore
splits into what the audit found (2), what was landed (3), and what was measured and
refused (4).

## 2. Audit: every counter, its writers, its readers, its exactness

| counter | storage | writers | readers | class |
| --- | --- | --- | --- | --- |
| `pending_bytes`, `pending_count` | per-mutator `std::atomic<size_t>`, relaxed | owner only, every allocation | `nrq_gc_get_stats` sums all mutators; owner resets in `publish_tlab_locked` | **exact immediately**, cross-thread visible |
| `statistics.allocated_bytes` / `live_objects` | under `gc_mutex` | `publish_tlab_locked` only | `nrq_gc_get_stats` | exact after a publication, and completed by the pending sum |
| `statistics.collected_bytes`, `collections` | under `gc_mutex` | sweep / cycle exit | stats | exact after a collection |
| page `live_objects`, `young_objects` | per page, under `gc_mutex` | publication, sweep | collector decisions | exact at every point a collector decision reads them |
| `allocation_accounting.bytes_since_collection` | `std::atomic<size_t>`, relaxed | the batch flush, the large-object path, the cycle reset — all under `gc_mutex` | `maybe_collect_automatically`, lock-free | **internal trigger state**, lagging by design |
| `unflushed_bytes` | owner-only `size_t` | owner | owner and the flush | **internal trigger estimate**, never reported |
| `allocation_accounting.collection_threshold` | atomic, under `gc_mutex` on write | `nrq_gc_set_threshold`, collection adjustment | trigger, and the batch size | configuration; exact |
| `generational_collection_enabled`, `next_cycle_epoch` | atomics | policy switch, `prepare_cycle` | header construction, trigger | configuration; exact |

The split matters: only the first row is a *public* statistic that must be exact at
all times, and it is (a statistics query from another thread sums the per-mutator
atomics under `mutator_mutex` + `gc_mutex`). Everything below it is either exact
because it is written under the lock, or internal trigger state that the collector
is allowed to see late — but never the other way round.

`unflushed_bytes` is the only deliberately approximate value, its bound is one batch
(`kAccountingBatchBytes` = 16 KiB, or the threshold when the threshold is smaller),
and it never reaches a reported statistic: `nrq_gc_get_stats` reads the per-mutator
published counters, not this field.

## 3. Changes landed

### AO-03/1 — the pending counters keep an owner-side mirror (`f27ca8d`)

`account_tlab_allocation` read each published counter before adding to it. Only the
owning mutator writes those fields, and every clear happens while the owner is parked
or by the owner itself, so each load read a value this thread already held. The store
still publishes the exact count after every allocation, so cross-thread visibility is
unchanged; only two dependent relaxed loads left the path. SOURCE: two loads removed,
no new branch, no new state. MEASURED: neutral inside the noise, as expected — a
relaxed load of a line the thread owns is close to free.

### AO-03/2 — the trigger counter is updated with a store, not a CAS loop (`7810b21`)

`flush_mutator_accounting_locked` and the large-object path both updated
`bytes_since_collection` with a compare-exchange retry loop. All three writers (those
two plus the cycle reset) run under `gc_mutex`, so the exchange could never fail; the
loop only cost a locked instruction on a line the lock-free trigger also reads. The
helper is now a saturating load + store, and the retry loop is gone from the runtime.

Proof obligation added and documented at the definition: **every writer of
`bytes_since_collection` holds `gc_mutex`**. A future writer outside the lock would
have to reintroduce a read-modify-write. Nothing observable changes — same
saturation, same reset points, same flush boundary — and one locked instruction per
16 KiB batch is far below this machine's timing resolution, so no speedup is claimed.

## 4. Measured and refused

**Removing the trigger reads from the fast path.** The entry point reads
`collection_phase` (twice), `generational_collection_enabled`, `next_cycle_epoch`,
`collection_threshold` (twice) and `bytes_since_collection` per allocation, so the
obvious direction is to evaluate the trigger only when the batch flush actually moves
accounting. A diagnostic build did exactly that — the trigger became a no-op, the
publication check was skipped and the batch size was cached — and it was measured
against the same binary with the switch off, so the only difference was the work:

MEASURED, 1M rooted allocations, 18 interleaved samples each:

| | min | median | max |
| --- | --- | --- | --- |
| per-object checks | 25.52 ms | 59.25 ms | 71.92 ms |
| checks skipped | 26.80 ms | 60.30 ms | 65.33 ms |

The skipped-work variant is not faster. These are relaxed loads of read-only lines
that stay resident in L1, and the branch the diagnostic added to reach the gate costs
about as much as the loads it avoided. **Rejected**: there is no cost to recover, so
there is no reason to change when the trigger is evaluated — and with it, no reason to
accept the trigger-latency consequences such a change would carry.

**Making the pending counters non-atomic.** Refused on correctness, not performance:
`nrq_gc_get_stats` reads another mutator's counters while that mutator is allocating,
which is precisely the concurrent read the atomic is for. On x64 a relaxed atomic load
or store compiles to the plain instruction anyway, so the atomic costs nothing to
keep.

**Deduplicating the overflow checks.** No dominating proof exists: the two checks
guard different fields, and one suppresses a real wrap on an allocation that could
otherwise corrupt the published count. Left alone.

## 5. Invariants preserved

- `nrq_gc_get_stats` continues to report exact, immediately current values: the
  per-mutator pending pair is written on every allocation and summed on query, so an
  unpublished TLAB batch is never invisible.
- Page counters, the start bitmap and `statistics` still end publication at exactly
  the values the per-object loop produced; nothing about publication moved.
- The collection trigger observes the same values at the same boundaries as before
  this stage. Its lag behind allocation is still one batch (≤ 16 KiB), unchanged.
- `unflushed_bytes` remains owner-only and unreported; no public statistic became
  approximate.
- The stop-the-world handshake, park-before-scan protocol, root publication and
  pin-transfer accounting were not touched.

## 6. Tests (MEASURED)

- `build/msvc-dev/tests/nirqoba_unit_tests.exe`: 194/194.
- `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe`: 263/263,
  including the statistics-boundary and threshold cases.
- Run without the diagnostic patch in the tree; the tree is at `7810b21` for both.

## 7. Limitations

- The probe is single-threaded, so the cross-thread trigger latency argument above is
  reasoned from the lock discipline (SOURCE) and covered only by the suite's
  multi-threaded tests, not by a dedicated concurrent measurement.
- The per-object-load experiment used a `getenv` gate evaluated once per process
  through a function-local static; the call itself is part of what was measured, so
  the negative result is an upper bound on the savings available (a variant that
  inline-eliminated the gate would move both columns by the same amount).
- Timing resolution on this machine is roughly ±5% on the median; both landed changes
  are labelled SOURCE and neither is claimed as a speedup.

## 8. Recommendation

- AO-03/1: **accept** (already in the 0.298-beta release, credited here).
- AO-03/2: **accept** as a locked-instruction removal with a documented proof
  obligation and no observable change.
- The stage is **complete and at its floor**: the only redundant work the audit found
  on the accounting path has been removed, and the remaining reads are read-only lines
  whose elimination measured slower. Continuing to trade exactness for speed here
  would need owner approval for a semantic change, which the roadmap does not grant.
