# Memory Model Migration — Phase 1 Report

## 1. Phase Overview

- **Phase identifier:** MMM Phase 1
- **Phase title:** Ownership Representation
- **Implementation date:** 2026-11-15
- **Status:** Complete
- **Objective:** Establish an explicit ownership representation across semantic
  analysis and TIR so ownership categories, transfers, borrows, lifetimes and
  destruction obligations are visible and verifiable, without implementing Phase 2
  move/destruction execution or Phase 3 lifetime enforcement.
- **Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

Starting version: `0.515-beta`. Final version: `0.531-beta`.

## 2. Scope

Implemented:

1. Explicit ownership categories in the semantic model.
2. First-class ownership transfer representation.
3. Immutable and mutable borrow representation with owner and path.
4. Lexical, parameter and return lifetime relationships.
5. Destruction obligation representation and discharge records.
6. Ownership metadata preserved through TIR into MLIR.
7. Verifier rules for invalid ownership states and transformations.
8. Diagnostics `NRQ4006`-`NRQ4008` with location and contextual notes.
9. Extension of the Phase 9 verifier rather than a second verifier.

Intentionally excluded: move execution semantics, deterministic destruction
ordering, path-sensitive discharge proofs, full borrow/lifetime enforcement,
arena allocation, weak references, owned strings, exception cleanup, FFI and
thread ownership. No source syntax was added.

## 3. Architecture and Design

`ValueOwnership` remains the single category type carried by TIR values,
parameters and returns. New helpers classify categories so Sema, lowering and the
verifier cannot disagree.

TIR gains four operations:

| Operation | Purpose |
|---|---|
| `tir.move` | Source, result and reason of an ownership transfer |
| `tir.borrow` | Borrowed value, owner storage, path and mutability |
| `tir.end_borrow` | End of a lexical borrow |
| `tir.drop` | Discharge of a destruction obligation, followed by its release |

Functions additionally own `LifetimeRecord` and `DestructionObligation` tables.
The operation stream stays authoritative for borrows and drops; the tables
preserve relationships for transformations and for table-wide validation.

Lowering maps a move result to its source and ignores the other markers. The
escape/stack-promotion analysis propagates descriptor identity through moves, so
the existing optimisation decisions are preserved.

## 4. Implementation Details

### 4.1 Transfer

The builder emits `Move` wherever Sema marked a transfer. The source is recorded
as moved; the existing null-store invalidation remains. The verifier rejects a
non-transferable source, type/ownership mismatch, a result that is not a distinct
value, use after move and a second move of the same value. A transfer reason must
match its consumer: `return` transfers must feed a `Return`, `argument` transfers
must feed a call argument.

### 4.2 Borrow and lifetime

`&x` emits `AddressOf` then `Borrow`. The borrowed value carries
`Borrowed`/`MutableBorrowed`, the operation records the owner storage and the
dotted path (`p.x`, `values[]`), and a lexical lifetime record is added. Scope
exit emits `EndBorrow` before cleanup; returned borrows are left open and described
by a `Return` lifetime that names the source parameter. Borrowed parameters carry
a `Parameter` lifetime.

Verifier rules: borrowed ownership must match mutability, the borrow must
originate from `address_of`, the owner may not itself be borrowed, a matching
lexical lifetime must exist, and a borrow may not be ended twice or without being
created.

### 4.3 Destruction obligations

An owning storage slot registers one obligation when its cleanup is armed; a
transfer marks it transferred instead of discharged. Cleanup emits `Drop` before
the release call. The verifier checks the obligation exists, its owner is a
storage location, its type and release match, the release family matches the type,
it is discharged at most once, it is not dropped after transfer, and the drop is
immediately followed by the named release.

### 4.4 Preservation

MLIR receives `nirqoba.ownership` on values, the existing return ownership
attributes, plus `nirqoba.lifetime_relations` and
`nirqoba.destruction_obligations` on functions. The TIR printer shows categories,
lifetime relations and obligation state.

## 5. Modified Files

| File | Role |
|---|---|
| `compiler/include/nirqoba/Common/Type.h`, `lib/Common/Type.cpp` | Category enum, classification helpers, stable spelling |
| `compiler/include/nirqoba/TIR/IR.h`, `lib/TIR/IR.cpp` | Transfer/lifetime/obligation records, new operations, tokens |
| `compiler/include/nirqoba/TIR/Builder.h`, `lib/TIR/Builder.cpp` | Move/borrow/end_borrow/drop emission, borrow paths, obligation registration |
| `compiler/lib/TIR/Verifier.cpp` | Ownership, borrow, lifetime and destruction verification |
| `compiler/lib/TIR/Printing.cpp` | Ownership-aware TIR dump |
| `compiler/lib/Lowering/TirToMlir.cpp` | Move aliasing, marker no-ops, metadata attributes, escape-scan aliasing |
| `tests/unit/ownership_test.cpp` | Positive and negative representation tests |
| `tests/unit/mlir_pipeline_test.cpp` | Metadata-preservation test |
| `tests/integration/cli/ownership_tir/` | `--emit=tir` integration check |
| `docs/memory/ownership-representation.md` | User-facing representation guide |
| `docs/compiler/cli.md` | New diagnostic codes |
| `docs/architecture/02-typed-ir.md` | TIR architecture update |
| `README.md`, `project_context/MMM_PLAN.md`, `project_context/PROGRESS_REPORT.md`, `docs/VERSION_LOG.md`, `reports/memory-model/target-architecture.md` | Status, progress and version records |

## 6. Language/API Changes

No new syntax, no changed syntax, no new runtime API. Internal TIR gains four
operations and two function tables. Compiler diagnostic codes `NRQ4006`,
`NRQ4007` and `NRQ4008` are added for internal ownership failures. All changes are
compiler-internal; valid source programs are unaffected.

## 7. Testing

Environment: Windows x64, MSVC 14.51, RelWithDebInfo, LLVM/MLIR 23.1.4.

Commands:

```text
build\msvc-dev\tests\nirqoba_unit_tests.exe
python tests\run_tests.py --compiler build\msvc-dev\bin\nirqoba.exe --configuration RelWithDebInfo
```

Results:

- Unit: 294 tests, 294 passed.
- Integration: 502 tests, 502 passed, 0 failed, 0 skipped.
- New unit coverage: category classification; return and argument moves; borrow
  plus lifetime plus end_borrow; borrowed parameter lifetime; obligation
  registration/discharge/transfer; borrow paths; printing; and negative cases for
  move from a borrow, use after move, false transfer reason, mutability mismatch,
  missing lifetime, borrow origin, unknown/double/dropped obligation, obligation
  owner not storage, missing release, reserved return category and missing return
  lifetime source.
- Integration coverage: CLI `--emit=tir` dump requires move, borrow,
  end_borrow, drop, lifetime and obligation records.
- Regression coverage: the full existing suite, including GC, secure storage,
  owned aggregate/array/field moves, SoA field moves and stack-promotion cases.

## 8. Performance

No throughput benchmark was run; Phase 1 adds no runtime work. The markers lower
to no instructions, a move is an SSA alias, and the stack-promotion escape scan
treats moves as aliases. The integration suite confirms the previous stack-vs-heap
decisions for the existing MLIR golden cases (`stack_placed_*`, `escaping_*`).

## 9. Known Issues

- The verifier is flow-insensitive: it does not prove that every obligation is
  discharged on every control-flow path (Phase 2).
- Drops are represented but destruction order is not enforced (Phase 2).
- Borrow exclusivity and escape rules are still Sema's; the TIR representation does
  not add new lifetime inference (Phase 3).
- `ArenaBound` and `Weak` are represented but never produced (Phases 5-6).
- One full-suite unit run produced a segfault in an existing GC stress test while
  other runs of the same binary passed, including the focused test; the GC
  implementation was not modified by Phase 1 and the final full run passed.
- No benchmark measurement was taken.

## 10. Documentation Changes

New `docs/memory/ownership-representation.md`; updated
`docs/architecture/02-typed-ir.md`, `docs/compiler/cli.md`, `README.md`,
`project_context/MMM_PLAN.md`, `project_context/PROGRESS_REPORT.md`,
`docs/VERSION_LOG.md` and `reports/memory-model/target-architecture.md`.

## 11. Acceptance Criteria

| Criterion | Status |
|---|---|
| Semantic model distinguishes ownership categories | PASS |
| Ownership transfer has an explicit internal representation | PASS |
| Immutable and mutable borrowing are distinguishable | PASS |
| Lifetime relationships can be represented | PASS |
| Destruction obligations are represented | PASS |
| Ownership metadata survives TIR transformations | PASS |
| Invalid ownership transformations are detectable | PASS |
| Diagnostics identify ownership violations | PASS |
| Existing GC functionality remains operational | PASS (GC tests and integration suite) |
| Existing and new relevant tests pass | PASS (294 unit / 502 integration) |

## 12. Final Verdict

**PASS**

Evidence: the full unit and integration suites pass, the new representation is
exercised positively and negatively, metadata survives to MLIR, and no source or
runtime behaviour changed. Phase 2 and Phase 3 remain explicitly unstarted.
