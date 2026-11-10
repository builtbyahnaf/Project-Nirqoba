# AO-05 — TLAB fast path and the runtime-call boundary

**Base commit:** `2412e6d` (page-storage stage closed)
**Result commits:** `d3264d6`, `382df3b`, `a753375`
**Configuration:** production runtime (`NRQ_RUNTIME_DEBUG_ENABLED=0`), MSVC 18,
RelWithDebInfo + `/Ob2`, Windows x64.
**Labels:** MEASURED / SOURCE. Timing statements are MEASURED with their sample count.

## 1. Scope

The roadmap asks for a generated-code TLAB bump path, and refuses it until a list of
invariants is proven. This stage answers that list, measures what the boundary
actually costs, and lands only the changes the measurements justify.

## 2. The invariants the roadmap asks for

| question | answer in this runtime |
| --- | --- |
| Are root/publication contracts stable? | Yes. `BO` closed with the root-frame, shadow-stack and publication contracts that AO-02 exercised; nothing here changes them. |
| Can generated code touch per-thread cursor/end safely? | Only through an ABI the runtime would have to freeze: `MutatorState` is a C++ type with no layout guarantee, and the cursor set is three fields plus a generation stamp. Exposing them is a new public ABI, not a local change. |
| How is TLAB invalidation observed? | `gc_page_generation` is bumped at every cycle preparation while all mutators are parked; the mutator discards its cursor when the stamp differs. A generated fast path would have to read the stamp on every allocation or invalidate by address, which is the paging problem the stamp exists to avoid. |
| How are initialization and publication ordered? | Construction, payload zeroing, reference-layout installation, then the pending-list push and the accounting update; the object becomes visible to the collector only in `publish_tlab_locked`. |
| How does the handshake keep the collector off partial objects? | `begin_fast_barrier`/`end_fast_barrier` bracket allocation and root publication; the collector parks mutators and waits for the per-mutator depth to reach zero before snapshotting. During a cycle each allocation publishes its batch immediately, because an unpublished object is invisible to the marker. |
| How is rooted allocation and reference metadata supplied? | The compiler emits one call per allocation site with the root slot, element count/size and a per-site reference-layout descriptor; the descriptor is already hoisted per module. |
| How does the debug runtime validate the fast path? | `NRQ_RUNTIME_DEBUG_ENABLED` emits allocation tags and event records, and the rooted entry validates the root slot through the per-thread slot index. |

Every row is answerable, but two of them are only answerable *because* the work stays in
the runtime: the generated path would have to re-derive the generation stamp and the
barrier state per allocation. That is the cost the next section measures.

## 3. What the boundary actually costs (MEASURED)

Diagnostic builds of the same source, one variation each, run against the baseline
binary with 1,000,000 rooted allocations, 5 interleaved processes × 3 rounds. Steady
rounds (the first round includes page faults and is reported separately):

| variant | what it removes | steady rounds |
| --- | --- | --- |
| baseline | — | 22.2 / 23.3 / 24.8 ms |
| v1 | the two per-object cycle-state loads (generational flag, birth epoch) | 21.2 / 24.3 / 29.8 ms |
| v2 | the allocation accounting update | 21.6 / 23.9 / 28.6 ms |
| v3 | the fast-barrier protocol around allocation and root publication | 22.1 / 25.7 / 33.6 ms |
| v4 | the variable-size `memset` call for the payload | 23.0 / 25.7 / 29.7 ms |
| root-check | the per-call root-slot validation | 23.1 / 24.8 / 28.4 ms |

None of the variants moves the median. Each of these components is individually below
this machine's resolution (≈2 ns of a 22–24 ms total, i.e. under 1% per item), and
removing them does not compose into a win because the remaining time is the mandatory
work: 96 bytes of header stores, the payload's zero fill, the page's first-touch cost
on a growing heap, and the call itself.

A generated fast path could remove the call and the barrier's entry/exit, which the
measurement above bounds at less than the noise floor, and it would pay for that with a
new public ABI, a per-allocation generation check, and a second implementation of the
publication rules. **The runtime call is retained.** This is the outcome the roadmap
explicitly permits when the proofs are not simple: they are not, because the invariants
that make the current path safe are per-thread state that generated code cannot read
without a frozen layout.

One related measurement did show a real difference and is recorded for honesty: a single
non-generational run measured 28.0 ms against 23.6 ms for the generational default
(+19%), which suggested the per-object locked counter update. The follow-up interleaved
A/B could not reproduce it (22–25 ms on both sides), so no speedup is claimed for the
fix below; the fix stands on the policy defect, not on timing.

## 4. Changes landed

### AO-05/1 — the size is validated once, at the boundary (`d3264d6`)

The entry points computed and checked the payload size and then the thread-local bump
path repeated the emptiness and overflow checks. Validation now happens where the size
is already in a register, and the bump path only requires that the thread is a mutator.
The untyped entry gained the check it had been relying on the callee to make, so no path
lost a guard. MEASURED: unchanged (three redundant compares and one repeated overflow
guard removed per allocation).

### AO-05/2 — allocation no longer re-arms the write barrier (`382df3b`)

An object that is not young raised the store barrier and updated the old-object count
per allocation. The raise undid `nrq_gc_set_generational(0)` at the next allocation, so
a non-generational program paid the write-barrier path on every store with no young
object for a remembered entry to point at. The barrier is now raised only by
`begin_cycle` and by the policy switch, which are the only places that know a minor can
run. The count moved into the publication walk, which already reads `young` for the page
counters, so counting a non-young object no longer costs a locked instruction on the
allocation path.

Publication is the exact point for the count: a cycle probes it in `finish_cycle`, and
`prepare_cycle` publishes every mutator before marking starts, so the value a policy
decision sees is current. When generations are off the probe is short-circuited, which
is the only state in which allocations add to the count.

### AO-05/3 — the cycle disarms the barrier when generations are off (`a753375`)

A cycle armed the barrier for the duration of marking and lowered it at the end only if
no old objects existed. A non-generational program therefore re-armed it on every cycle
and never disarmed it: the same defect, one level up. The cycle now lowers it whenever
no minor can run or no old object exists. `nrq_gc_store_barrier_active` exposes the
state, and a unit test pins the whole policy: down after disabling generations, still
down after allocating and after a full cycle, armed again when they are re-enabled.

## 5. Invariants preserved

- One runtime call per allocation, and exactly one slow-path call (refill, large object,
  or cycle-time publication) on the paths that need it.
- Root publication still happens inside the fast-barrier region, so the collector cannot
  snapshot between an allocation and the store that makes it reachable.
- Publication still assigns each object's start bit, page counters and statistics in the
  same walk; the old-object count joins that walk without changing what is published.
- No data race is added: the old-object count is still an atomic updated by whoever
  publishes, and the barrier state is only written by policy code.
- TLAB invalidation, generation stamps and the parking protocol are untouched.

## 6. Tests (MEASURED)

- `build/msvc-dev/tests/nirqoba_unit_tests.exe`: 197/197, including the new barrier
  policy test.
- `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe`: 263/263,
  which covers TLAB generation invalidation, concurrent collection during allocation,
  rooted publication and the debug-tag path.

## 7. Limitations

- The diagnostic variants change semantics (they are not landable), so they bound the
  cost of each component but cannot show a composition of them.
- The boundary audit is single-threaded. Multi-mutator contention on the barrier counter
  and the publication lock is covered by the suites, not by these timings.
- The noise floor of this machine (≈5% on a median, with round-to-round swings larger
  than that) means anything under roughly 1 ns per object is unresolvable here; several
  of the items above are in that range individually.

## 8. Recommendation

- The generated fast path is **refused on evidence**: the work it would remove is below
  the noise floor, and the invariants it needs are per-thread state that would have to
  become a frozen public ABI.
- AO-05/1, AO-05/2, AO-05/3: **accept**. One is a boundary simplification with no
  timing claim, one removes a locked instruction plus a policy defect, and one is a
  correctness fix for the non-generational mode with a test to keep it fixed.
- The allocation path is at its floor for this machine and this workload; the remaining
  cost is memory that must be written and faulted, not bookkeeping that can be removed.
