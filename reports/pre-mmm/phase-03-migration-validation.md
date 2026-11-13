# Pre-MMM Phase 3 — Migration-scale cleanup validation

**Status:** Complete for the supported language surface
**Version under test:** `0.461-beta` (Phase 4 runtime changes validated separately)
**Date:** 2026-11-13
**Baseline:** `487aa93`

## Purpose

Phase 3's regression set used many small, focused programs. This validation runs
the same destruction rules in larger programs that combine several supported
features at once, so cleanup interactions are exercised together rather than in
isolation. No production code was changed for this validation.

## Programs

### Nested aggregates with early return and conditional defer

`tests/integration/positive/cleanup_migration_scale_aggregates/`

- 1,500 iterations.
- Three owned arrays per iteration, nested two aggregate levels deep
  (`Envelope -> Payload -> owned_array<int>`).
- An early return from the helper on every eleventh seed, exercising cleanup on
  the return edge.
- A conditional `defer memory.deallocate(buffer)` in an inner scope. The branch
  that schedules the defer uses explicit cleanup; the other branch retains the
  automatic fallback. Both paths run in the same program.
- Expected output: `5513361`.

### Loop-scope destruction with `continue` and `break`

`tests/integration/positive/cleanup_migration_scale_loops/`

- A `for` initializer owns an array across condition, body, and step.
- A nested body owner is released on fall-through, `continue`, and `break`.
- The initializer is released at the loop exit edge after `break`.
- The loop performs roughly 250,000 allocation/release cycles before the
  high-water break.
- Expected output: `252200`.

## Verification

| Check | Result |
|---|---|
| Both migration-scale programs build and run | PASS |
| Nested aggregate output | PASS (`5513361`) |
| Loop-scope output | PASS (`252200`) |
| Repeated runs produce identical output | PASS |
| Full integration suite after adding both programs | 450/450 |
| Unit suite | 224/224 |
| Deferred-owner negatives remain rejected | PASS |
| Transfer/defer cleanup IR counts remain stable | PASS |
| Full Phase 3 cleanup/GC/native suites | PASS |

The programs were run through the normal integration runner, which builds each
test and compares the exact stdout. They therefore also exercise the generated
cleanup path, not only semantic acceptance.

## Residual gaps

No new supported-path defect was found. The following limitations remain the
same ones recorded by Phase 3:

- **Error/unwind cleanup is absent.** The language has no error or exception
  model. The documented integration point remains the active lexical cleanup
  stack; no landing pads or unwind tables are generated.
- **Conditional fallback cleanup is retained.** A branch-local move or defer
  intentionally keeps a null-safe fallback on the path where it did not run.
  Removing it needs path-sensitive cleanup guards at labels.
- **Partially initialized aggregates** still call typed destructors with null for
  fields that were never initialized. Null is a no-op; no generic dispatch is
  introduced.
- **Raw `&` pointers** are outside deterministic cleanup. They are lifetime
  checked in supported cases but do not carry ownership.
- **Dynamic owned arrays whose element type owns resources** remain rejected,
  because recursive element destruction is not implemented.
- **No sanitizer run** was available in the MSVC configuration. Evidence is the
  full suites, raw-IR cleanup counts, repeated runtime checks, and the stress
  cases.
- The pre-existing intermittent GC test crash documented in the Phase 3 report
  remains separate GC test-hardening work.

## Conclusion

The supported destruction rules survive migration-scale composition without a
new leak, double-free, or cleanup-order defect. The remaining gaps are explicit
language/model limitations, not regressions in the completed Phase 3 paths.
