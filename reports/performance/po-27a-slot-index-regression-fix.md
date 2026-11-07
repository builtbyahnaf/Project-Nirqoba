# PO-27A — SlotIndex Tombstone Growth Regression Fix

**Task:** PO-27A, corrective follow-up to PO-27
**Scope:** `runtime/src/gc.cpp`, `runtime/src/slot_index.h`, `SlotIndex`
**Status:** Implemented and correctness-tested. Benchmark validation deliberately
**not executed by the coding agent** at the project owner's instruction; the
owner will run the stress-test suite.
**Date:** 2026-11-07
**Build:** Nirqoba 0.228-beta, LLVM/MLIR 23.1.4, MSVC x64 RelWithDebInfo

---

## 1. Root cause

Commit `0b2d457` replaced the per-thread `std::unordered_map<void*, SlotState>`
root-slot map with the open-addressing `SlotIndex`. Its insertion path was:

```cpp
if (entries_.empty() || size_ + tombstones_ + 1 >=
        entries_.size() - entries_.size() / 4) {
  if (!rehash(entries_.empty() ? 16 : entries_.size() * 2)) return nullptr;
}
```

Two defects combined:

1. The load threshold was computed from `size_ + tombstones_`, so dead entries
   counted against capacity.
2. Every rehash doubled the table, whether the occupancy came from live entries
   or from tombstones.

A workload that keeps one or two root slots alive while registering and
releasing a long series of short-lived slots therefore left one live entry and an
ever-growing tombstone count. Each time the occupied fraction crossed 75 % the
table doubled, even though live occupancy was near zero. After `N` churn cycles
the backing vector approached `2^ceil(log2(N))` entries. At one million cycles
this is roughly a 1 GB allocation, and every subsequent probe walks a sparse
gigabyte-sized table — the CPU-bound hang observed in PO-27.

`PageDirectory` in the same file already had the correct policy: grow only when
live occupancy is at least half the table; otherwise rehash at the same capacity
to clear tombstones. `SlotIndex` lacked that branch.

## 2. Exact changes

### 2.1 `SlotIndex` extracted to an internal header

`runtime/src/slot_index.h` now contains the table. The collector and the unit
tests include the same definition, so the white-box tests exercise the exact
implementation rather than approximating it through the public GC API.

`runtime/src/gc.cpp` includes the header and instantiates
`SlotIndex<SlotState>`; no lookup, insertion, deletion, root-tracking, overflow,
or allocation-failure semantics changed.

### 2.2 Occupancy-aware rehash policy

```cpp
State* insert_impl(void* slot, bool allow_growth) {
  if (!slot) return nullptr;
  if (entries_.empty()) {
    if (!rehash(kInitialCapacity)) return nullptr;
  } else if (allow_growth &&
             size_ + tombstones_ + 1 >=
                 entries_.size() - entries_.size() / 4) {
    size_t next = 0;
    if (!select_next_capacity(entries_.size(), size_, &next)) return nullptr;
    if (!rehash(next)) return nullptr;
  }
  ...
}
```

with the policy:

```cpp
static bool select_next_capacity(size_t capacity, size_t live, size_t* next) {
  if (!next || capacity == 0) return false;
  if (live >= capacity / 2) {
    if (capacity > SIZE_MAX / 2) return false;  // refuse to wrap
    *next = capacity * 2;                        // genuine live growth
  } else {
    *next = capacity;                            // same-capacity compaction
  }
  return true;
}
```

The invariant after any rehash is `capacity >= 2 * live_entries`, so capacity is
bounded by peak concurrent live slots instead of historical insertions.

### 2.3 Preserved semantics

- Duplicate registration counting (`State::count`) and layout validation are
  unchanged.
- Tombstone reuse during insertion and last-registration-only tombstoning during
  release are unchanged.
- `rehash()` still catches allocation failures and returns `false`; `insert()`
  still propagates failure with `nullptr`, so `add_slot_state()` still raises the
  same fatal error on a table that cannot grow.
- The overflow check is preserved and now lives in `select_next_capacity()`.
- `rehash()` additionally rejects `size_ >= capacity` before moving any entry, so
  a mis-sized rehash cannot loop or leave moved-from states behind. The policy
  never produces such a capacity.

### 2.4 Probe-loop termination

All three probe loops (`find`, `insert`, `release`) iterate at most
`entries_.size()` times. `find()` on a table with no empty entry returns
`nullptr` after a complete wrap; `insert()` returns `nullptr` when it can neither
place the entry nor consume a tombstone. This is unchanged from the original
bounded loops and is now covered by an explicit full-table test.

### 2.5 Diagnostics

`SlotIndex::Diagnostics` exposes: capacity, live entries, tombstones, rehash
count, same-capacity compaction count, capacity-growth count, and maximum probe
length. `reset_diagnostics()` clears only the event counters for isolated
measurements. `insert_for_testing(slot, allow_growth)` lets a test fill a table
to capacity without provoking an impossible allocation; production calls
`insert(slot)` only.

Overhead: four `size_t` event counters (updated only on a rehash or on a probe
chain completion) plus one compare-and-store per completed operation for the
maximum probe length. There is no allocation, logging, or additional table walk,
so no expensive diagnostics are left enabled in release builds.

## 3. Before/after capacity traces

Measured with a standalone harness compiled against `runtime/src/slot_index.h`
and the exact production template. The "old" column is the previous
always-doubling policy simulated arithmetically: allocating the real old table
would exhaust memory before the trace completed.

One live entry, repeated insert/release:

| Cycles | Old capacity | New capacity |
|---:|---:|---:|
| 16 | 32 | 16 |
| 64 | 64 | 16 |
| 256 | 256 | 16 |
| 1 024 | 1 024 | 16 |
| 4 096 | 4 096 | 16 |
| 16 384 | 16 384 | 16 |
| 65 536 | 65 536 | 16 |
| 262 144 | 262 144 | 16 |

New policy, one million churn cycles (one live entry throughout):

```text
cycle 1        capacity=16  live=1  tombstones=1   rehashes=1      compactions=0      growths=0  maxprobe=1
cycle 16       capacity=16  live=1  tombstones=3   rehashes=2      compactions=1      growths=0  maxprobe=3
cycle 64       capacity=16  live=1  tombstones=7   rehashes=5      compactions=4      growths=0  maxprobe=5
cycle 256      capacity=16  live=1  tombstones=9   rehashes=16     compactions=15     growths=0  maxprobe=6
cycle 1 024    capacity=16  live=1  tombstones=4   rehashes=66     compactions=65     growths=0  maxprobe=7
cycle 4 096    capacity=16  live=1  tombstones=4   rehashes=261    compactions=260    growths=0  maxprobe=10
cycle 16 384   capacity=16  live=1  tombstones=10  rehashes=1 043  compactions=1 042  growths=0  maxprobe=10
cycle 65 536   capacity=16  live=1  tombstones=8   rehashes=4 143  compactions=4 142  growths=0  maxprobe=11
cycle 262 144  capacity=16  live=1  tombstones=5   rehashes=16 499 compactions=16 498 growths=0  maxprobe=11
cycle 1 000 000 capacity=16 live=1  tombstones=9   rehashes=62 886 compactions=62 885 growths=0  maxprobe=11
```

Capacity never leaves the 16-entry minimum and capacity growth is exactly zero.
The `2^ceil(log2(N))` growth from the old trace is gone.

Genuine live growth (400 concurrent entries):

| Live entries | Capacity | Growths | Max probe |
|---:|---:|---:|---:|
| 1 | 16 | 0 | 1 |
| 12 | 32 | 1 | 4 |
| 24 | 64 | 2 | 13 |
| 48 | 128 | 3 | 13 |
| 96 | 256 | 4 | 20 |
| 192 | 512 | 5 | 20 |
| 384 | 1 024 | 6 | 20 |
| 400 | 1 024 | 6 | 20 |

Capacity follows the live set, as required.

## 4. Probe statistics

| Scenario | Capacity | Max probe length | Result |
|---|---:|---:|---|
| One live entry, 1 000 000 churn cycles | 16 | 11 | Bounded by capacity |
| 400 live entries | 1 024 | 20 | Load factor ≤ 75 % |
| 11 addresses colliding in one 16-entry bucket | 16 | 11 | All lookups found |
| Full 16-entry table, absent lookup | 16 | 16 | Returned `nullptr` after one full wrap |
| Full 16-entry table, absent release | 16 | 16 | Returned `false` after one full wrap |

No probe loop can exceed the table capacity, and the capacity itself is bounded
by `O(peak live slots)`.

## 5. Tests

New white-box suite: `tests/unit/slot_index_test.cpp` (`SlotIndexTest`), compiled
against `runtime/src/slot_index.h`. It covers every required case:

| Requirement | Test |
|---|---|
| Repeated insert/release, one live entry | `SingleLiveEntryChurnKeepsCapacityBounded` |
| Repeated insert/release, several live entries | `SeveralLiveEntriesChurnKeepsCapacityBounded` |
| Genuine live-entry capacity growth | `GenuineLiveEntryGrowthGrowsCapacity` |
| Collision-heavy insertion and lookup | `CollisionHeavyInsertAndLookup` |
| Repeated slot-address reuse | `RepeatedSlotAddressReuse` |
| Lookup after same-capacity compaction | `LookupAfterSameCapacityCompaction` |
| Lookup after capacity growth | `LookupAfterCapacityGrowth` |
| Empty-table behaviour | `EmptyTableOperationsAreSafe` |
| Full-table unsuccessful lookup termination | `FullTableUnsuccessfulLookupTerminates` |
| Capacity-overflow handling | `CapacityOverflowIsRejected` |

The existing API-level churn suite in `tests/unit/gc_slot_index_test.cpp`
(`GcSlotIndexTest`, seven tests) remains and also passes. Its stale "fix not yet
applied" note was updated, and its timeout assertions were rewritten without
unsupported `<<` streaming so the file compiles against the project harness.

Execution evidence (RelWithDebInfo, MSVC x64):

```text
[==========] 182 tests ran.
[  PASSED  ] 182 tests.
```

Selected suite:

```text
[ RUN      ] GcSlotIndexTest.SingleSlotRepeatedRegisterUnregisterStaysBounder
[       OK ] ...
...
[ RUN      ] SlotIndexTest.CapacityOverflowIsRejected
[       OK ] ...
[==========] 182 tests ran.
[  PASSED  ] 17 tests.   (filtered run: 7 GcSlotIndexTest + 10 SlotIndexTest)
```

Full build of compiler, runtime, and unit tests succeeded. The complete
CLI/integration suite also passed:

```text
Total: 259 | Passed: 259 | Failed: 0
```

No collection-correctness regression was observed: all GC unit tests and the
integration tests that exercise GC roots, frames, and collection passed.

## 6. Benchmark results

**Not run by the coding agent.** The task owner explicitly reserved benchmark
execution. The required PO-27A benchmark steps — rebuilding the compiler and
runtime for the stress executables, regenerating all three stress executables,
timing independent runs, checksum/object-reclamation validation, repetition, and
the complete Stress Test 2 suite — remain pending owner validation. No
benchmark completion or performance claim is made in this report.

## 7. Remaining limitations

- Capacity does not shrink after a peak; same-capacity compaction only removes
  tombstones. This matches the required implementation and the `PageDirectory`
  reference policy, and keeps the table at the high-water mark of the live set.
- Same-capacity compaction is `O(capacity)` and occurs when tombstones push
  occupied entries past 75 %. With the 16-entry minimum this is a small,
  bounded cost (about one compaction per 11 tombstone-producing operations in
  the one-live-entry trace), not a growth path.
- Diagnostics remain compiled in all configurations because their cost is a few
  integer counters and one comparison per completed probe chain. If a future
  profile ever shows measurable overhead, they can be moved behind a dedicated
  build option without changing table behaviour.
- The fix does not change `ObjectIndex`, `PageDirectory`, marking, sweeping, or
  any other GC subsystem, per the task's no-optimization constraint.

## 8. Acceptance criteria status

| Criterion | Status |
|---|---|
| No unbounded tombstone-driven capacity growth | Verified by trace (capacity 16 after 1 000 000 cycles) |
| Capacity tracks peak live entries, not insertion count | Verified by growth trace and tests |
| No infinite probe loops | Bounded loops verified by full-table tests |
| No memory corruption or lost root registrations | Full GC unit suite and integration suite pass |
| No regression in collection correctness | 182 unit + 259 integration tests pass |
| All three stress-test executables complete | **Pending owner benchmark validation** |
| Performance results are reproducible | **Pending owner benchmark validation** |
