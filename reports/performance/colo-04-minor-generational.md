# ColO-04 --- Minor collection and generational correctness

**Stage:** 5 of the ColO Roadmap 2 sequence
**Base / result:** test only, on top of the ColO-03 commit; production code
unchanged.
**Date:** 2026-11-09

## 1. What the investigation left open

`[SOURCE]` `reports/bottleneck_discovery.md` and the corrected note in
`docs/VERSION_LOG.md` (`4ed6d478` -> `e4cd478`) state that the minor-page skip
(`skip pages with zero young objects`) is **not exercised by the major-collection
benchmarks**, and that its cost is therefore **UNVERIFIED**. The correction also
records that minor and multithreaded *correctness* tests do exist
(`tests/unit/gc_test.cpp`); what was missing was a benchmark cost workload, not
coverage of the basic path.

The roadmap's rule for this stage is explicit: do not optimise the page skip until
a workload proves the path correct *and* a stage measurement proves it costly.

## 2. New coverage added

`tests/unit/gc_test.cpp`, `GcTest.MinorReclaimsOnlyYoungGarbageAndKeepsOldGarbage`:

1. Heap cleared, generational mode enabled.
2. Two 64-byte objects are allocated, **both rooted**, and a major collection
   promotes both to the old generation.
3. One of them (`old_garbage`) is unrooted: it is now unreachable *old* storage.
4. A rooted young object and an unrooted young object are allocated.
5. `nrq_gc_collect_minor()` runs. Assert: live count is `baseline + 3` --- the old
   root, the unreachable old object, and the rooted young object. The unrooted
   young object is gone.
6. A major collection runs. Assert: `baseline + 2` --- the old garbage is
   reclaimed, the rooted young survivor is not.
7. Both remaining roots are unregistered; a final collection returns to the
   baseline live count.

The test intentionally checks the *negative* case that the page skip could break:
an unreachable old object must survive a minor even though the page it lives on may
be swept for other reasons, and must then die in the major. It also catches the
opposite error (a minor that reclaims old storage or a major that keeps it).

## 3. Coverage against the roadmap's nine points

| # | Required property | Covered by |
| --- | --- | --- |
| 1 | allocates young and old objects | new test; `MinorCollectionPromotesSurvivorsAndRetainsOldGarbage` |
| 2 | promotes a subset | new test (rooted vs unrooted after promotion); existing minor test |
| 3 | mutates old-to-young references | `OldToYoungStoreBarrierKeepsYoungObjectAliveDuringMinorCollection` |
| 4 | performs minor collections | new test and both existing minor tests |
| 5 | verifies remembered-set coverage | `OldToYoungStoreBarrierKeepsYoungObjectAliveDuringMinorCollection` (barrier store, then minor retains the child) |
| 6 | reachable young survive | new test (rooted young object), existing minor test |
| 7 | unreachable young are reclaimed | new test (`young_garbage`), existing minor test |
| 8 | old objects are not accidentally reclaimed | new test (`old_garbage` survives the minor), existing minor test |
| 9 | repeated across promotion/collection epochs | partially: the new test runs promote -> minor -> major; `MinorCollectionPromotesSurvivorsAndRetainsOldGarbage` runs three minors and a major. No test runs many *epochs* with interleaved promotions |

Page-level young-object counters (`page->young_objects`) at zero / one / mixed
occupancy and across retirement/reuse are **not directly asserted**: the runtime
exposes no per-page young counter through the C ABI, and adding a diagnostic ABI
for a test would itself be a production API change the roadmap does not ask for.
The observable consequences (old retained, young reclaimed, page count and
recycling behaviour) are covered indirectly by points 6--8 and by
`EmptiedPagesAreRecycledAcrossCollections`.

## 4. Tests and results

```
./build269.bat
./build/msvc-dev/tests/nirqoba_unit_tests.exe    # 192 passed, 0 failed
```

Test-ordering note: the first version of this test asserted `baseline + 2` after
the minor, forgetting that the unreachable *old* object is deliberately retained.
The failure aborted the test before its cleanup, which left stale global root slots
and generational mode raised, and that poisoned later tests (including a hang in
`ConcurrentCollectionRequestsDoNotDeadlock`). The assertion was corrected to
`baseline + 3`; with it, the suite is green and the later tests pass unchanged.
This is recorded because the hang was investigated as a possible collector defect
and turned out to be test hygiene. `[MEASURED]`

## 5. Cost of the minor path

**UNVERIFIED as a performance claim.** No workload in this repository measures
minor-collection cost separately from major cost: `run_collection` is entered with
`young_only` only through `nrq_gc_collect_minor()`, and the two benchmark harnesses
explicitly collect fully. The roadmap's condition for optimising the page skip
("a stage-level measurement proves it is costly") is therefore **not met**, and
**no optimisation was made** --- consistent with the roadmap's instruction.

What is known from source (`[SOURCE]`): the skip is a per-page test of
`page->young_objects == 0` in `sweep_chunk_locked`; a skipped page costs one
page-header load and the loop advance, and no object is visited. Whether that
saves material time depends on the young/old page ratio of a real workload, which
is exactly what is unmeasured.

## 6. Recommendation

**Accept the coverage, no optimisation.** The new test closes the
"old-not-reclaimed / young-reclaimed" gap with an explicit negative case, the
correctness suite is green at 192/192, and the skip's cost stays marked UNVERIFIED
rather than being "optimised" on a hunch.
