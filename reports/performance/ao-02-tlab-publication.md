# AO-02 — TLAB publication and allocation path (part 1)

**Base commit:** `e38a44f` (BO stage closed, 0.294-beta open)
**Result commit:** `b0e2d81`
**Configuration:** production runtime (`NRQ_RUNTIME_DEBUG_ENABLED=0`), MSVC 18,
RelWithDebInfo + `/Ob2` (see AO-02/2), Windows x64, 1,000,000-object probe.
**Labels:** MEASURED / SOURCE / PROJECTION. Nothing below is claimed as a timing
result unless it is labelled MEASURED with its sample count.

## 1. What was measured, and with what

One local probe (`build/ao/publish_probe.cpp`, not part of the build and not
committed) drives the real managed entry points and times two regions:

| phase | calls | what it contains |
| --- | --- | --- |
| `ALLOC_MS` | `nrq_gc_allocate_array_typed_rooted` × N | root-slot validation, barrier region, TLAB bump, header/layout init, payload zeroing, per-object accounting |
| `COLLECT_MS` | `nrq_gc_collect` | publication of the pending batch, mark, sweep of N dead objects |

`publish_tlab_locked` was timed inside the runtime with **temporary local
instrumentation** (not in any commit) to isolate publication from mark/sweep.

The probe reports one sample per round; every comparison below ran the two
binaries **interleaved** (A/B/A/B alternation) and reports min/median/max, because
this machine's run-to-run spread is large (identical binaries differ by 30% on
individual samples; minima are the stable statistic).

Decomposition before any change (MEASURED, min of 12 samples, 1M objects):

| path | ALLOC min |
| --- | --- |
| rooted typed allocation | 45.1 ms |
| rooted, barrier region skipped (diagnostic build) | 38.2 ms |
| rooted, root-slot probe skipped (diagnostic build) | 44.5 ms |
| rooted, both skipped | 34.6 ms |
| unrooted typed allocation | 33.6 ms |

So the barrier region was ~7 ns/object and the root probe ~0.6 ns/object. The
diagnostic switches were local experiments and were removed; nothing about them is
in the tree.

Publication of 1,000,000 objects cost 9.8–14.3 ms (MEASURED, temporary
instrumentation), i.e. ~10–14 ns/object — a third of a full collection.

## 2. Changes

### AO-02/1 — publication no longer dirties published headers (`73de9c5`)

The loop cleared the batch link and the published link of every object. Neither
clear is needed: the pending list is dropped as a whole, only a page without a
start bitmap links its objects into `unindexed_objects`, and a header is
constructed again when its storage is handed out. Clearing them made publication
dirty one header cache line per object, so the traversal paid a write-back for
every line it read.

MEASURED, publication of 1M objects, 24 interleaved samples each:

| | min | median | max |
| --- | --- | --- | --- |
| before | 9.75 ms | 13.87 ms | 15.49 ms |
| after | 8.45 ms | 12.54 ms | 13.47 ms |

### AO-02/2 — the runtime is built with the full inliner (`329a5a8`)

`RelWithDebInfo` compiles with `/O2 /Ob1`. `/Ob1` only inlines functions it is
told about explicitly, and the GC hot path is built from tiny accessors: relaxed
atomic loads and stores over the per-mutator state, the `_Atomic_address_as`
helpers behind them, the address helpers, the slot-index probes. `SOURCE`: the
disassembly of `begin_fast_barrier` at `/Ob1` contains three out-of-line calls
into the C++ library and a stack frame; at `/Ob2` it is 14 instructions, no calls,
no stack frame, no fence instruction.

`runtime/CMakeLists.txt` now replaces `/Ob1` with `/Ob2` for this directory only,
in the `RelWithDebInfo` configuration. Optimization level, debug information,
every check and every diagnostic are unchanged; `MinSizeRel` keeps its
size-oriented setting on purpose.

MEASURED, 1M rooted allocations, 18 interleaved samples each, same source:

| | min | median |
| --- | --- | --- |
| `/Ob1` runtime | 41.4 ms | 79.6 ms |
| `/Ob2` runtime | 28.2 ms | 68.2 ms |

MEASURED end to end, Stress 03 (`03_cyclic_graph_reclamation`, 8 iterations of
1,000,000 nodes), 30 samples: BUILD median 128 → 89 ms, COLLECT median 36 → 31 ms.

*(Cross-check of the median against the pre-change 30-sample baseline: the baseline
median is from the fixed build before this stage, not an interleaved run, so it is
the weaker of the two numbers; the interleaved probe comparison above is the
primary evidence.)*

### AO-02/3 — the barrier region and root probe are inlined (`b0e2d81`)

A barrier region is two relaxed stores and one acquire load; the pointer-root probe
is a direct-mapped compare against the slot a construction loop reuses. Out of line
they cost a call and a register save on both sides of every rooted allocation.
`SOURCE`: the rooted entry point now makes four calls per allocation instead of six,
and `begin_fast_barrier` has no out-of-line symbol left.

MEASURED, 1M rooted allocations, 15 interleaved samples each: min 28.4 → 27.4 ms
(−3.5%), median 66.6 → 66.9 ms (unchanged inside the noise). Only the removed
call/return pair is claimed.

## 3. Investigated and rejected (measured, not landed)

Both were implemented, measured and reverted; neither is in the tree.

- **Aggregating page counters and global statistics over a page run**
  (`publish_tlab_locked`): publication median 12.49 → 10.41 ms in a first
  sequential comparison, then 10.15 (before) vs 12.04 (after) in an interleaved
  one — the first difference was drift, not the change. The per-object counter
  writes share cache lines that the loop touches anyway, so there was nothing to
  win. Rejected.
- **Word-batched object-start bitmap writes**: 8.38 → 8.96 ms (min, interleaved).
  The bitmap words stay hot in L1, and the extra branch and word-transition test
  cost more than the read-modify-write it replaced. Rejected.

## 4. Invariants preserved

- Publication still runs under `gc_mutex`, before the collector can scan anything;
  every pending object is published exactly once and none is omitted.
- Object-start bits, page live/young counters, `unindexed_objects` and the global
  statistics end publication at exactly the values the per-object loop produced.
- TLAB generation invalidation, the park-before-scan handshake, root-slot
  validation, barrier region accounting, layout assignment and payload zeroing are
  unchanged.
- No runtime attribute, protocol or accounting contract was weakened; the
  development runtime keeps every check and diagnostic (`/Ob2` only changes
  inlining).

## 5. Tests (MEASURED)

- `build/msvc-dev/tests/nirqoba_unit_tests.exe`: 194/194.
- `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe`: 263/263.
- Stress 02/03 built and run in production configuration; every semantic
  `KEY/VALUE` line (excluding `*_MS`) identical to the BO-00 baselines.

## 6. Limitations

- The probe measures one rooted allocation shape (1 element, 24-byte payload, one
  reference offset) and one object mix. Wide layouts, arrays and mixed sizes are
  not covered by the numbers above.
- Timing noise on this machine limits resolution to roughly ±5% on the median;
  changes smaller than that are reported as SOURCE-only.
- The `/Ob2` change alters every runtime baseline in the project. It is a one-line,
  directory-scoped, revertible commit, but earlier measurement reports were taken
  with `/Ob1` and are not directly comparable to numbers from `329a5a8` onward.

## 7. Recommendation

- AO-02/1: **accept** (measured, no semantic change).
- AO-02/2: **accept**, with the caveat that it resets the runtime baseline; if the
  owner prefers, revert only this commit and re-measure.
- AO-02/3: **accept** as a small, evidence-labelled inlining change.
- The two rejected experiments: **revert** (done), kept out of the tree.
