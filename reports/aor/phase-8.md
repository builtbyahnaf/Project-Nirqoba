# Phase 8 — extreme performance optimisation

Status: **incomplete**. Two of the three upgrades are partially delivered, the
third is not started, and the version has not moved. This report records what was
implemented and measured, and what is still open, so the next tranche starts from
facts rather than from a plan.

## Upgrade 8-Phi — Phase 7 closure

### Closed: the collection-timing failure

`stress_test_2` reported `COLLECT_MS=0` for a collection that reclaimed one
million objects. The cause was the measurement, not the collector: `time.now()`
is a whole-millisecond clock and the phase is shorter than one millisecond.

The fix is at the measurement layer, in both affected programs:

- The millisecond figure is kept, so the column stays comparable with the other
  tracks.
- `COLLECT_CYCLES` reports the same phase from `time.cycles()`, which is the
  runtime's `__rdtsc` counter.
- The harness now *requires* the cycle count for the Nirqoba tracks in tests 01
  and 03, rejects a non-positive one, and accepts a zero millisecond figure only
  when the cycle count proves the phase did work. The reclamation assertion
  (`LIVE_AFTER`) and the checksum are unchanged.

Nothing is inflated or hardcoded: the cycle count is the measured work, and a
zero-millisecond phase with a zero cycle count still fails validation.

Validated by running the previously failing workload and the two affected
tracks: `--test 01 --impl nirqoba` and `--test 03 --impl nirqoba` both pass
validation, with `COLLECT_MS 0` backed by a positive cycle count.

### Closed: root lifetime and the stale-address hazard

The hazard was investigated as a runtime question rather than documented as a
caller responsibility. The finding is that the runtime is already strict:

- Re-registering a live slot with a different description is fatal.
- A slot registered twice needs two releases.
- The last release drops the slot's pointer-root memo and resets its description,
  so a later registration of the same address with another layout is revalidated.

That made the hazard a *contract* question, so it is now pinned by
`tests/unit/gc_root_contract_test.cpp`: a two-word root record registered word by
word proves that the second registration scans only the word it names, and that
the first owner's description does not survive its release. The earlier test
failure that raised the hazard was a test that leaked a registration, which is the
contract violation the runtime is entitled to see; the contract is now explicit
and tested.

### Closed: multithreaded stress

`tests/unit/gc_thread_stress_test.cpp` runs the mix Phase 7 asked for: four
mutators allocating headerless chains, compact records, arrays, SoA payloads and
large objects; publishing and retiring root slots; writing an old-to-young
reference through the barrier on every iteration; each mutator owning its own
arena and pointing into it from collector storage; a collector thread running
minors and full cycles underneath; and a second test running arenas and pools
while collector threads recycle pages.

Failure detection is by observed state, not by absence of a crash: every worker
walks its chain after publishing and checks that stamps decrease strictly, and the
representations are checked once the heap is published.

Result: 451/451 unit tests pass, three consecutive full runs.

### Closed: a real collector defect the stress test found

The first runs trapped in `GC reserved runs outlived a cycle boundary`. The cause
was a defect, not a test artifact:

- The headerless refill reserves a batch of runs into the spare-run list that the
  ordinary refill also uses, but stamps only its own cursor generation.
- A later compact refill therefore saw a stale cursor stamp, concluded the TLAB
  was stale, and called the discard path with a non-empty list.
- Development builds trapped. Production builds abandoned the list, which left
  that space neither published nor free, so no page holding those runs could ever
  be recycled: a leak, and a page that could never be returned.

The fix gives the shared list its own generation, stamped by both reservers and
by the cycle boundary that flushes it, and makes the page generation atomic so the
fast path's acquire load cannot keep a value the boundary has replaced.

### Not closed

- **Marking:** the per-object cost was measured (see below), one redundant
  descriptor and payload resolution was removed, and a deferred batch flush was
  tried and reverted because it measured *slower* on chains (36.8 → 50.8 ns per
  object) instead of faster. No marking improvement is claimed.
- **H8 refill:** profiled only. At 512 B the allocator reports a 520 B span, 126
  slots per page and 16 B of page slack, so the page class is not the problem; the
  per-object cost tracks payload size, which points at zero-initialisation
  bandwidth rather than refill. That is a hypothesis, not a measurement, and no
  change was made on the strength of it.
- **Mixed-size fragmentation:** measured only. The half-survival workload reports
  49.5 % utilisation, 74 pages and 606 KiB of page metadata for 150 000 live
  objects. No page-class or reuse change was attempted.

## Measurements

Machine: the development Windows host, `RelWithDebInfo`, 300 000 objects,
5 runs per shape, medians.

| shape | alloc ns | mark ns/obj | sweep ns/obj |
|---|---|---|---|
| h0-chain-16B | 32.0 | 36.84 | 0.05 |
| h0-rooted-manual | 49.4 | 34.83 | 0.03 |
| h0-planned-16B | 35.9 | 37.40 | 0.03 |
| h8-record-512B | 59.0 | 36.42 | 0.74 |
| h16-array-4x16B | 49.1 | 40.83 | 0.26 |
| h16-large-200K | 50958.6 | 310.16 | 84.77 |
| h16-soa-ptr-column | 58.0 | 39.52 | 0.20 |

Barrier: 38.56 ns per old-to-young store. Fragmentation at half survival: 49.5 %
utilisation, 4.0 B of page metadata per live object.

Measurement limitation, stated because it decides what can be claimed: repeated
runs of the same binary on this host differ by up to 30 % on the marking column.
A change worth 3 % cannot be distinguished here, which is why the batch-flush
experiment was judged by a 38 % regression rather than by its small wins, and why
the descriptor hand-off was committed as a refactor with no performance claim.

## Upgrade 8-Chi — collector performance engineering

Not started beyond the marking investigation above. The boundary work from the
hybrid-architecture constraint is in place: representation-specific and
mixed-workload shapes exist in the Phase 7 harness, and arenas, pools and
collector storage are measured side by side in the coexistence and stress tests.

## Upgrade 8-Psi — production validation and stabilisation

Not started. No production-runtime build was measured, so no production/debug
comparison is reported. The full unit suite, the integration suite and the
`*gc*` configuration equivalence were run for the development configuration only.

## Version

Unchanged at `0.609-beta`. The rule is a bump after three meaningful upgrades; one
upgrade (Phi) is incomplete, so bumping would be a false claim.
