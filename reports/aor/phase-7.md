# AOR Phase 7 — Unified collector integration

**Status: in progress.** This tranche delivers the baseline audit (Mu), the
measurement harness, one measured optimization, the cross-representation
regression suite, and the architecture documentation. The remaining sections of
Phase 7 — the marking cost, the H8 refill cost, page-class fragmentation, and the
multithreaded/perf sweep — are **open**, and are listed with their reasons in
section 6 rather than being claimed as done.

Phase 8 has not been started.

## 1. What this tranche establishes

The audit in [phase-7-audit.md](phase-7-audit.md) found that the collector is
already representation-aware end to end: page classes for H0/H8/H16 plus large and
fallback pages, per-representation state location, bitmap and index discovery,
narrow-record and SoA tracing, a page-local remembered set, whole-page and
per-object sweep, and published thread-local reservations. Phase 7's work is
therefore integration and validation, not a rewrite.

Two concrete gaps were found and one of them was fixed:

| Finding | Outcome |
|---|---|
| SoA allocation built two vectors and took the layout lock per call | **Fixed**, 146.1 → 50.5 ns/object (2.9×), paired A/B |
| H0 state index divides by the slot size | **Measured slower** when replaced with a shift (+5.9–6.5%), reverted |
| `stress_test_2` `COLLECT_MS=0` | **Diagnosed** as a whole-millisecond timer against a 67.6 µs phase; corpus not modified |
| Remembered entries survive a cleared field | **Documented** conservative retention, pinned by a test |
| A root slot's address is its identity | **Test hazard found and fixed**; documented as a caller hazard |

## 2. Code changes

- `runtime/src/gc.cpp`: `SoaGeometryMemo` and a thread-local narrowing buffer.
  `soa_layout_for` takes a `(pointer, count)` view instead of a `std::vector`,
  `collect_soa_pairs` writes into the thread's buffer, and
  `soa_layout_matches` takes the same view. A hot SoA site no longer allocates or
  takes `layout_mutex`. Measured in
  [phase-7-performance.md](phase-7-performance.md).
- `runtime/src/gc.cpp`: the three planned collector entry points now refuse a
  plan carrying `NRQ_ALLOC_PLAN_DETERMINISTIC` and return null. That plan names
  deterministic storage, so honouring it would have let the allocator pick the
  program's lifetime strategy; the compiler's fallback decides instead. No
  existing caller passes that flag to a collector entry point, so nothing changed
  behaviour for them.
- Nothing else in the collector changed. In particular the rejected H0 index
  change is not present.

## 2a. Strategy boundaries (non-negotiable constraint)

The hybrid model is treated as an invariant the collector must not move, and it
was audited rather than assumed:

| Rule | Where it is enforced |
|---|---|
| Deterministic ownership is the default | Nothing is tracing-managed unless allocated through a collector entry point; `nrq_gc_pointer_is_managed` is the check |
| Regions keep their own lifetime | `nrq_cdmm_strategy_of` / `nrq_cdmm_arena_contains`; the collector's page set and the region range table are disjoint |
| A plan may choose a strategy, never change one | `cdmm_plan_is_plain` and `cdmm_layout_is_plain` refuse a representation, a header, an array or references; the collector's planned entry points refuse a deterministic plan |
| AOR is representation, not ownership | A 16-byte arena object reports no representation and no header; a 16-byte collector record is headerless |
| Cross-strategy references are visible, not merged | `nrq_gc_root_register_array` over region storage keeps a collector object alive; releasing the root returns it to the collector alone |

Both directions are now asserted by `tests/unit/cdmm_coexistence_test.cpp`
(collector pages are not region storage, regions survive collection) and
`tests/unit/strategy_boundary_test.cpp` (a deterministic plan is refused by every
collector entry point and honoured by a region; representation does not decide
ownership; a region can root a collector object without owning it). The contract
is written down in
[collector-architecture.md](../../docs/memory-model/collector-architecture.md).

No part of this tranche merges strategy semantics, and the one measured
optimization (the SoA geometry memo) is inside collector-managed allocation only:
that path cannot be reached with region or deterministic storage.

## 3. Validation actually run

| Check | Result |
|---|---|
| Unit tests (`nirqoba_unit_tests.exe`) | **446 / 446 passed** |
| Integration (`tests/run_tests.py`, RelWithDebInfo) | **624 / 624 passed** |
| Configuration equivalence (`*gc*`) | **67 agreed, 0 skipped, 0 failed** |
| Build (`nirqoba_unit_tests`, `nirqoba`, both AOR benchmarks) | clean, no warnings from the touched files |

New tests, all in `tests/unit/gc_phase7_test.cpp`:

| Test | What it pins |
|---|---|
| `AMinorKeepsAnOldObjectsYoungReferentInEveryRepresentation` | old→young edges in H0, H8 and H16 survive a minor; the remembered retention is conservative; a full cycle reclaims |
| `ACycleThatCrossesRepresentationsIsReclaimed` | H0→H8→H16→H0 cycle live while rooted, gone when unrooted |
| `ADeeperGraphThanTheMarkWorklistHoldsIsFullyMarked` | 100,000-node chain, i.e. marking through the worklist overflow |
| `ALargeObjectKeepsItsExactBoundaries` | 200 KiB object survives while its neighbour is reclaimed, then is reclaimed itself |
| `APinnedObjectSurvivesCyclesWithoutARoot` | pin as the only root across a major and a minor; child traced through the pinned object |
| `NestedArraysStayReachableThroughTheOuterElement` | array of arrays reachable only through the outer element |
| `SoaGeometryKeepsItsIdentityAndTracesItsColumn` | geometry memo identity plus tracing a pointer in a column |
| `APlannedRecordAllocatesTheRepresentationItAskedFor` | planned record selects H0, caches the descriptor, replaces a rejected plan |

Strategy-boundary tests, in `tests/unit/strategy_boundary_test.cpp`:

| Test | What it pins |
|---|---|
| `EveryCollectorEntryPointRefusesADeterministicPlan` | a plan naming deterministic storage is refused by all three planned collector entry points, allocating and caching nothing |
| `ADeterministicPlanIsHonouredByARegionInstead` | the same plan creates a pool; a plan carrying references is refused by an arena |
| `RepresentationDoesNotDecideOwnership` | equal size and header, different ownership, and destroying the region leaves the collector object alone |
| `ExplicitStorageCanRootACollectorObjectWithoutOwningIt` | a region roots a collector object; releasing the root returns it to the collector alone |

The tests measure the live-object count relative to the count they start with and
release roots through an owner, so they do not depend on suite order and cannot
leak a root when an assertion throws. That design came out of a real failure
during bring-up (a reused stack address inherited a stale root), recorded in the
audit.

## 4. Measurements

Full tables in [phase-7-performance.md](phase-7-performance.md). The headline
numbers at one million objects, five runs:

- H0 record allocation **38.6 ns/object**; live cycle **42.05 ms**; dead cycle
  **67.6 µs**.
- Planned record **40.3 ns** vs allocate + separate root store **55.2 ns**.
- H16 large object (200 KiB) **370 ns/object** to mark, **60.7 µs** to allocate.
- Armed store barrier **36.65 ns/store**.
- Page metadata **4.0 B per live object** for H0; 8,000,000 B of headers saved
  over H16 for 500,000 live objects.
- Half-surviving heap: **49.8% utilization**, 245 committed pages.

## 5. Version

The label stays **0.609-beta**. The delivery rule bumps the beta version by
0.004 after three meaningful upgrades; this tranche lands **one** measured
upgrade (the SoA geometry lookup). Bumping now would misreport the rule, so the
next tranche's upgrades are what carry the label to 0.613-beta.

## 6. Open Phase 7 work

Recorded so the next tranche starts from the measurements rather than from a
guess:

1. **Marking cost (42–59 ns/object).** The live-cycle cost is almost entirely
   marking. The harness can split mark from sweep only at the cost of a
   collector-side counter, which is the first thing to add before optimizing
   blindly. Candidate targets, in order: the per-child page-directory probe, the
   child batch flush, and the two page-local state reads per marked object. Each
   needs a paired A/B like the one in section 2; the H0 index experiment shows
   that the obvious instruction-level win is not necessarily one.
2. **H8 512-byte allocation (145 ns/object with a wide spread).** The header is
   correct; the cost is per-page or per-refill. Not investigated, not changed.
3. **Page-class fragmentation across mixed sizes.** Only the single-geometry
   half-surviving case is measured. A mixed-size workload is needed before any
   page-reuse policy is changed.
4. **Concurrent collection.** The existing concurrency tests cover
   representations and unpublished objects, and they pass. A stress test that
   mixes representations, pins, destruction and repeated cycles across threads is
   not yet written; without it no claim about multithreaded correctness beyond the
   existing tests is warranted.
5. **Production-runtime measurements.** The production configuration builds from
   the same source and is covered by the equivalence check, but the numbers here
   are development-runtime numbers.
6. **`stress_test_2` timer.** The corpus needs one of the two fixes described in
   the audit; that decision belongs to the corpus owner and no artifact was
   modified.

Because of items 1–5, the completion criteria for Phase 7 are **not** met, and
the phase stays open.
