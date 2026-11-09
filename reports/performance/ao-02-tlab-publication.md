# AO-02 — TLAB publication and allocation path

**Base commit:** `e38a44f` (BO stage closed, 0.294-beta open)
**Result commits:** part 1 `73de9c5`, `329a5a8`, `b0e2d81`; part 2 `e3ab513`,
`a830dfc`, `f27ca8d`, `de03170`
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

### AO-02/4 — GC page storage is committed and released per page (`e3ab513`)

Pages came from `malloc`/`free` in `kGcPageBytes` units and were recycled through a
pool. The allocator keeps committed memory behind a freed block: a page returned to
the pool and later abandoned still held its commit charge for the rest of the
process. Each page is now reserved and committed with `VirtualAlloc` and released
with `VirtualFree`, so the commit charge follows the live page count.

MEASURED, private commit at 1M objects, taken from `GetProcessMemoryInfo`
(`PROCESS_MEMORY_COUNTERS_EX.PrivateUsage`) at the end of the run:

| round | pages | before | after |
| --- | --- | --- | --- |
| 1 | 1,833 | 238 MB | 122 MB |
| 2 | 1,833 | 474 MB | 242 MB |

Page count and every object count are identical, so this is memory accounting only:
no timing claim is made, and `ALLOC_MS` moved inside its noise.

### AO-02/5 — the allocation-size guard multiplies instead of dividing (`a830dfc`)

Every array entry point proved that `count * element_size` fits by dividing
`SIZE_MAX` by the element size. `SOURCE`: the `divq` appeared once per array
allocation, and a 64-bit divide is tens of cycles on this core. The high half of the
product (`_umul128` on x64, a portable fallback elsewhere) answers the same question
with one multiply.

Same products are rejected; the fatal path and its message are unchanged. MEASURED:
`ALLOC_MS` unchanged inside the machine's noise, so this is claimed as removed
instructions, not as a speedup.

### AO-02/6 — the pending counters keep an owner-side mirror (`f27ca8d`)

`account_tlab_allocation` read each published counter before adding to it. Only the
owning mutator writes those counters, and every clear happens either while the owner
is parked or by the owner itself, so the load always read a value the thread already
had. The store still publishes the exact count after every allocation — which is what
a statistics query from another thread observes — and the overflow checks are
unchanged. SOURCE: two dependent relaxed loads removed from the allocation path.
MEASURED: neutral inside the noise.

### AO-02/7 — pages with tail space are indexed instead of rescanned (`de03170`)

`reserve_tlab_locked` and the single-object path both walked `gc_pages` backwards to
find a page whose bump cursor had not reached the end. In a churn workload every page
is filled to capacity by the TLAB that takes it, so the walk never found anything and
still cost 1,833 page visits per refill.

Diagnostic first (MEASURED, temporary `getenv` gate, since removed): making the walk
unconditional-free — treating "no candidate" as known — took 1M rooted allocations
from min 32.5 to 26.1 ms and median 64.2 to 58.5 ms over 15 interleaved samples each,
which priced the walk at ~6 ms per million objects.

The walk is now replaced by an index of pages with unused tail space, maintained where
the cursor moves (page creation, bump, TLAB reserve, rollback) and dropped when the
page fills or retires. Selection order is preserved: allocation takes the first
indexed page that fits (as the forward scan did), a refill takes the newest (as the
reverse scan did).

MEASURED, 1M rooted allocations, 18 interleaved samples each:

| | min | median | max |
| --- | --- | --- | --- |
| before | 30.0 ms | 66.5 ms | 99.2 ms |
| after | 24.9 ms | 60.9 ms | 81.7 ms |

Pages (1,833) and private commit (241.5 MB) are identical on both sides, so the index
changed only how a page is found, not which page is used.

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
- **128 KiB TLABs** (four times the current reservation, to quarter the refill
  count): collection time 23 → 133 ms on the same workload. A larger reservation
  leaves a tail too small to serve the next request in every page, so no page is
  ever fully used and pages stop being recyclable — the refill saving is real and
  irrelevant next to the reclamation loss. Rejected, and the roadmap's stop
  condition on TLAB size stands.
- **Replacing the zeroing `memset` with an inline store loop** on the payload:
  measured neutral, and it added a branch and a loop to a path that already uses the
  library's vectorised fill. Rejected as unproven complexity.

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
- AO-02/4: **accept** (memory measured, no timing claim).
- AO-02/5, AO-02/6: **accept** on SOURCE grounds — removed instructions with no added
  branches, complexity or state transition. Neither is claimed as a speedup.
- AO-02/7: **accept** (measured on both the diagnostic and the landed form, with the
  page and commit counts unchanged).
- The rejected experiments: **revert** (done), kept out of the tree.

Net effect of the stage on 1M rooted allocations, minimum of an interleaved run at
the start of part 1 and at the end of part 2: **45.1 ms → 24.9 ms (−45%)**, with
publication of the same million objects at 9.75 → 8.45 ms minimum and the commit
charge for 1,833 pages at 238 → 122 MB. The stage is at its floor: what remains on
the allocation path is the TLAB bump, the header/layout initialisation, the payload
zeroing and the barrier region, each of which the roadmap either sizes as a
requirement or already prices at its instruction cost.
