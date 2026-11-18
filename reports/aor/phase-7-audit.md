# AOR Phase 7 — Baseline audit (Mu)

**Status:** audit complete, one measured change landed, the rest of Phase 7 is
open and listed in [phase-7.md](phase-7.md).
**Runtime:** development configuration (`nirqoba_runtime_debug`), RelWithDebInfo,
MSVC 19.4x, Windows 11, 12 logical processors.
**Harness:** `benchmarks/aor/phase7/gc_collection_benchmark.cpp`.

## 1. What was audited

The audit read the collector as it actually is, not as the earlier investigation
described it. `runtime/src/gc.cpp` is 6,539 lines; the parts that decide
representation-aware behaviour are:

| Concern | Location |
|---|---|
| Page classes and per-page metadata | `GcPage`, `PageKind`, `PageDirectory` |
| Object state, per representation | `object_state`, `set_object_state`, `h0_state_index` |
| Discovery | `find_object_locked`, `ObjectIndex`, `test_object_start` |
| Descriptor identity | `layout_from_id`, `object_layout`, `LayoutMemo` |
| Tracing | `trace_children`, `enqueue_narrow_children_locked` |
| Generational state | `remember_young_locked`, `drain_remembered_locked`, `add_remembered_locked` |
| Publication | `publish_tlab_locked`, `account_new_object_locked` |
| Sweep | `sweep_chunk_locked`, `release_span_locked` |
| Cycle | `prepare_cycle_locked`, `begin_cycle`, `finish_cycle_locked` |
| Allocation | `allocate_locked`, `allocate_tlab`, `reserve_h0_tlab_locked` |

The collector is already representation-aware: H0/H8/H16 page classes, per-class
discovery, per-representation state location, arrays, SoA columns, large objects,
pinned roots, arena/pool coexistence and a page-local remembered set are all
present. Phase 7 is therefore an integration-and-validation phase rather than a
rewrite, and the audit's job was to find where that integration leaks.

## 2. `stress_test_2` `COLLECT_MS=0` — reproduced and diagnosed

**Reproduction.** The failing artifact is
`benchmarks/stress_test_2/benchmark_results_failed_1794985319.json`, with twenty
validation failures of the form `round N: COLLECT_MS=0 is not positive` for tests
01 and 03 of the Nirqoba track. Rebuilding that program and running it directly
reproduces it:

```
ROUND 2
ALLOC_MS     73
TRAVERSE_MS   3
COLLECT_MS    0
CHECKSUM      499999500000
LIVE_AFTER    0
```

`LIVE_AFTER 0` is the important half: the collection ran and reclaimed all one
million objects. The phase is not missing; the number is.

**Cause.** `benchmarks/stress_test_2/nirqoba/01_managed_object_allocation.nrqba`
times the phase with `time.now()`, which lowers to `nrq_time_now_ms` and returns
whole milliseconds. The program's own comment states the assumption: "Every phase
here is deliberately sized above 100 ms so the 1 ms resolution stays
negligible." That assumption no longer holds for the collection phase. The
object graph is a chain of one million 16-byte headerless records, so when the
root goes away the sweep finds pages with no live object and retires them without
reading a single header.

**Evidence at a resolution that can represent it.** The Phase 7 harness runs the
same shape — one million H0 records of 16 bytes, chained, then unreachable — and
times the collection with `steady_clock`:

| Metric | Median of 5 runs | Range |
|---|---:|---:|
| Full cycle, graph garbage | **67.6 µs** | 50.5 – 104.9 µs |
| Per object | 0.07 ns | 0.05 – 0.10 ns |

One millisecond is about fifteen times the median. The corpus can only ever print
`0` for this phase.

**Which layer is wrong.** The collector is not wrong and the measurement is not
falsified by this report: the *timer* is the wrong layer. Two honest fixes exist
and neither was taken here, because both edit a frozen benchmark corpus:

1. time the phase with `time.cycles` (`__rdtsc`), which the language already
   exposes, and report a cycle count next to the millisecond figure;
2. accept a zero-millisecond figure when a companion cycle metric proves the
   phase did work.

Until one of those is chosen by the corpus owner, the failure should be read as
"the harness cannot represent a sub-millisecond collection", not as "the
collector did not collect". This report does not alter `benchmarks/stress_test_2`
or its artifacts.

## 3. Planned record allocation — validated and measured

Phase 6 added `nrq_gc_allocate_planned` for records but compared it against the
*unrooted* typed path, which is not the same work. The audit added a fair
baseline: allocate a record, then publish it into a registered root with a
separate `nrq_gc_root_store`, which is exactly what the planned entry point does
in one call.

One million 16-byte H0 records, median of five runs:

| Path | ns/object | Range |
|---|---:|---:|
| Typed record, unrooted (`nrq_gc_allocate_layout`) | 38.6 | 35.2 – 40.4 |
| Allocate + separate root store | 55.2 | 54.1 – 57.3 |
| Planned record (`nrq_gc_allocate_planned`) | 40.3 | 38.5 – 41.6 |

The planned entry point is **14.9 ns per object (27%) cheaper** than doing the
same two operations by hand, because it allocates and publishes the result inside
one barrier region instead of two call boundaries. Against the unrooted path it
costs 1.7 ns, which is the price of actually rooting the object.

`GcPhase7Test.APlannedRecordAllocatesTheRepresentationItAskedFor` pins the
behavioural half: the plan selects H0, the site cache is populated and reused, and
a plan carrying the rejected bit falls back to the allocator's own decision.

## 4. Fragmentation and utilization baseline

Half of one million H0 records survive a full cycle, with survivors interleaved
with garbage:

| Quantity | Value |
|---|---:|
| Live objects | 500,000 |
| Live payload | 8,000,000 B |
| Committed pages | 245 |
| Committed bytes | 16,056,320 B |
| Utilization (payload / committed) | 49.8% |
| Free runs recorded | 93,984 over 245 pages |
| Page metadata | 2,007,040 B (4.0 B per live object) |
| Header bytes saved against H16 | 8,000,000 B |

50% utilization for a heap where 50% of the objects survived is the expected
result, not a defect: every page keeps its interleaved dead spans as recorded
runs and can serve a later allocation from them. The interesting number is the
metadata: 4.0 bytes per live object for the bitmaps and the H0 state table, against
16 bytes per object that the H16 header would have cost.

Page slack, from `nrq_gc_get_allocation_profile`:

| Payload | Span | Per page | Slack | Per refill |
|---:|---:|---:|---:|---:|
| 16 | 16 | 4096 | 0 | 4096 |
| 64 | 64 | 1024 | 0 | 1024 |
| 256 | 256 | 256 | 0 | 256 |
| 512 | 520 | 126 | 16 | 126 |
| 4096 | 4104 | 15 | 3976 | 15 |
| 65536 | 65544 | 1 | 0 | 1 |
| 204800 | 204808 | 1 | 0 | 1 |

The 512-byte and 4096-byte slack is the 8-byte span alignment, not a regression:
H8 adds 8 bytes of header, so a 512-byte payload becomes a 520-byte span.

## 5. Store-barrier baseline

An armed old-to-young store, measured after a full cycle has created an old
generation:

| Session | ns per store |
|---|---:|
| 100k objects, 10 alternations | 36.9 (before) / 36.6 (after) |
| 1M objects, 5 runs | 36.65 median (35.7 – 49.0) |

The armed barrier is a pointer store plus an append to the thread's own referent
buffer, not a hash lookup under a lock.

## 6. Findings

### 6.1 Fixed: the SoA geometry lookup allocated twice per call

`nrq_gc_allocate_soa_rooted` narrowed the caller's column table into a
`std::vector`, then `soa_layout_for` built a second vector for the intern key and
took `layout_mutex` — on every allocation, for a geometry that is fixed per site.
The measured cost was **146.1 ns per object against 47.9 ns for an equivalent
array**.

Fixed by narrowing into a thread-local buffer and answering a recently seen
geometry from a four-entry thread-local memo, so a hot site neither allocates nor
takes the layout lock. Paired measurement over ten alternations:
**146.1 → 50.5 ns per object (2.9×)**. See
[phase-7-performance.md](phase-7-performance.md).

### 6.2 Rejected: replacing the H0 state-index division with a shift

`h0_state_index` divides by the page's slot size on every state read, which
looks like an obvious win to replace with a shift for power-of-two slots. It was
implemented and measured with a paired A/B over two independent sessions:

| Session | H0 chain cycle, before | after |
|---|---:|---:|
| 3 alternations | 3907.5 µs | 4161.3 µs |
| 8 alternations | 3909.7 µs | 4139.5 µs |

Consistently about 5% slower, not faster: the division's latency was already
hidden behind the cache misses that dominate marking, and the extra branch and
page field cost more than the divide. The change was reverted. An
instruction-count reduction is not an optimization, which is why it was measured
rather than assumed.

### 6.3 Found: a root slot's address is its identity

While bringing up the new tests, a case that reused a stack address for a root
slot inherited a root an earlier case had registered at that address and never
released. The collector was right to honour the registration; the test was wrong
to reuse the address. The tests now give every root slot its own storage. This is
worth stating because it is a real hazard for any caller that registers a slot on
a reused stack frame without releasing it: the unwound frame's slot address can
name a live root later.

### 6.4 Documented: the remembered set retains a referent conservatively

Clearing an old object's field does not remove the young object from the
remembered set, so the young object stays reachable for the minor generation even
though nothing references it. A full collection reclaims it. This is deliberate
(the alternative is scanning every remembered container on every minor) and is
now pinned by `GcPhase7Test` and documented in
[collector-architecture.md](../../docs/memory-model/collector-architecture.md).

## 7. Benchmarks captured

`benchmarks/aor/phase7/gc_collection_benchmark.cpp`, target
`nirqoba_phase7_benchmark`, prints:

- allocation ns/object, full-cycle µs and ns/object for a live graph, and
  full-cycle µs and ns/object for the same graph as garbage, for H0, H8, H16
  arrays, H16 large objects, SoA and two record entry points;
- armed store-barrier ns per store;
- fragmentation: utilization, committed pages, free runs, metadata bytes and
  header bytes saved for a half-surviving heap;
- the representation facts and the allocation profile as constants.

Every timed cycle asserts that a cycle ran, that the mark phase kept the graph
alive, and that the sweep phase reclaimed it, so a fast number cannot come from a
collection that did nothing.
