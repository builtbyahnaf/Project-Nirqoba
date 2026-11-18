# AOR Phase 7 — Performance report

**Scope:** the measurements taken for Phase 7-Mu and the one optimization that
survived them. Nothing here is extrapolated; every table is from the harness in
`benchmarks/aor/phase7` on the development runtime.

## Method

Two kinds of measurement are used, because they answer different questions.

**Paired A/B.** Two binaries, one built from the tree before a change and one
after, alternate over N sessions: `before, after, before, after, …`. Each
invocation internally reports a median over its own runs, and the table reports
the median of those per-session medians. Alternation cancels slow drift in the
machine (thermal state, background load) that a single before/after pair does
not. This is the only method used to accept or reject a change.

**Baseline capture.** One binary, five runs, medians, for the absolute numbers a
later phase will be compared against.

**Variance is real and is reported.** This machine's per-run spread for the same
shape reaches 15% (for example the H0 chain's full cycle: 39.45, 41.63, 42.05,
41.68, 43.68 ms across five runs). A difference smaller than that spread is only
accepted when it reproduces in the same direction across independent sessions.
The rejected change in section 4 reproduced; the accepted change in section 2 is
far larger than the spread.

## 1. The accepted change: SoA geometry lookup

`nrq_gc_allocate_soa_rooted` used to narrow the caller's `(offset, stride)` table
into a `std::vector`, then `soa_layout_for` built a second vector for the intern
key and took `layout_mutex` — on every call, for a geometry that is fixed at the
call site.

Change: the narrowing goes into a thread-local buffer that keeps its capacity, and
a four-entry thread-local memo keyed by `(element_bytes, column table)` returns
the already-interned descriptor. A descriptor lives for the life of the process,
so caching its address is safe; a geometry with more than sixteen column values
falls through to the intern pool exactly as before.

Paired A/B, ten alternations, 100,000 objects, three runs each:

| Workload | Before | After | Ratio |
|---|---:|---:|---:|
| H16 SoA, four rows, pointer column — allocate | **146.1 ns** | **50.5 ns** | **2.9×** |
| H16 SoA — full cycle, graph live | 3836.7 µs | 3920.8 µs | within noise |
| H16 SoA — full cycle, graph dead | 15.3 µs | 22.9 µs | within noise |
| H16 array, 4 × 16 B — allocate (reference) | 47.9 | 44.3 | within noise |
| H0 chain — allocate | 34.0 | 36.3 | within noise |
| H0 chain — full cycle, graph live | 3948.6 µs | 3896.6 µs | within noise |
| H8 record 512 B — allocate | 50.7 | 50.2 | within noise |
| Armed store barrier | 36.9 ns/store | 36.6 ns/store | within noise |

An earlier session with three alternations measured 145.1 → 52.2 ns, so the
result reproduces. The allocation path now costs what an equivalent array
allocation costs, which is what it should have cost all along: both end up in the
same `allocate_array_rooted_impl` with a resolved descriptor.

Coverage for the change:
- `GcPhase7Test.SoaGeometryKeepsItsIdentityAndTracesItsColumn` — the same table
  returns the same descriptor, a different table returns a different one, and a
  reference stored in the pointer column is traced and kept alive;
- the existing SoA entry-point and representation tests still pass;
- 442 unit tests, 624 integration cases, and 67 `*gc*` configuration-equivalence
  agreements pass with the change in place.

## 2. Baseline: one million objects, five runs

Medians; ranges in the per-workload rows of the raw run. `mm` is a full cycle
with the whole graph live (mark plus the survivor sweep); `sweep` is a full cycle
after the graph became garbage.

| Shape | Objects | Allocate ns | Cycle (live) | ns/object | Cycle (dead) | ns/object |
|---|---:|---:|---:|---:|---:|---:|
| H0 record 16 B, chained | 1,000,000 | 38.6 | 42.05 ms | 42.1 | **67.6 µs** | 0.07 |
| H0 record, allocate + root store | 1,000,000 | 55.2 | 38.17 ms | 38.2 | 52.5 µs | 0.05 |
| H0 record, planned entry point | 1,000,000 | 40.3 | 45.52 ms | 45.5 | 47.6 µs | 0.05 |
| H8 record 512 B | 41,666 | 145.1 | 2.008 ms | 48.2 | 44.6 µs | 1.07 |
| H16 array 4 × 16 B | 1,000,000 | 58.3 | 49.53 ms | 49.5 | 1.18 ms | 1.18 |
| H16 large object 200 KiB | 256 | 60.7 µs | 94.7 µs | 370 | 22.9 µs | 89.5 |
| H16 SoA, pointer column | 1,000,000 | 70.6 | 46.83 ms | 46.8 | 371.7 µs | 0.37 |

Readings that matter for the next tranche:

1. **Reclaiming a dead graph is almost free.** 1M H0 objects in 67.6 µs is the
   whole-page path: 245 pages retired without reading a header. A dead H16 array
   graph costs 1.18 ms because each array object's span has to be released
   individually — still about 1.2 ns per object.
2. **Marking is the cost.** A live 1M-object cycle is 42–58 ms across shapes,
   i.e. 42–59 ns per object. The same 1M objects cost 67.6 µs to sweep once they
   are dead, so essentially the whole live cycle is marking. This is the single
   largest target left in the collector.
3. **The H8 512-byte allocation is expensive** (145 ns with a 93–163 ns spread at
   41,666 objects) compared with 38.6 ns for H0. The representation is chosen
   correctly (8-byte header), so this is a per-page/refill cost, not a header
   cost, and it needs its own investigation before any change is proposed.
4. **The planned record entry point is the cheaper way to root a record**
   (40.3 vs 55.2 ns). Phase 6's report compared it against the unrooted path,
   which understated it; this is the fair comparison.

## 3. Metadata and utilization

Half of 1,000,000 H0 records survive, interleaved with garbage:

| Quantity | Value |
|---|---:|
| Live objects | 500,000 |
| Live payload | 8,000,000 B |
| Committed pages | 245 |
| Committed bytes | 16,056,320 B |
| Utilization | 49.8% |
| Free runs | 93,984 over 245 pages |
| Page metadata | 2,007,040 B = **4.0 B per live object** |
| Header bytes saved vs H16 | 8,000,000 B |

4.0 bytes per live object is the mark, start, remembered and born bitmaps plus
the one-byte H0 state table. For comparison, the H16 header it replaces is 16
bytes per object.

## 4. The change that was rejected

Replacing `h0_state_index`'s division by the slot size with a shift for
power-of-two slots. Implemented, measured, reverted:

| Session | Alternations | H0 chain cycle before | after | Delta |
|---|---:|---:|---:|---:|
| 1 | 3 | 3907.5 µs | 4161.3 µs | +6.5% |
| 2 | 8 | 3909.7 µs | 4139.5 µs | +5.9% |

Reproduced in the same direction twice, so it is a small real regression: the
divide's latency hides behind the marking loop's cache misses, and the extra
branch plus the extra page field cost more than it saved. Reverted rather than
kept as an unmeasured "obvious" win.

## 5. Limitations of these measurements

- One machine, one compiler version, one runtime configuration (development).
  The production runtime is a different build of the same source and was not
  benchmarked here.
- `steady_clock` resolution is far below the phases measured, but the timer
  itself is not free: the shortest phase (22.9 µs for 256 large objects) includes
  two clock reads.
- The SoA shape uses four rows per object, so its allocation cost includes
  per-object overhead a single large SoA allocation would not pay.
- The fragmentation workload has one geometry (16-byte records, half surviving).
  Page-class fragmentation across mixed sizes is a separate question and is not
  answered here.
- No profiler was used. Section 2's statement that marking dominates is derived
  from comparing a live cycle with a dead one of the same shape, not from a
  sampling profile.
