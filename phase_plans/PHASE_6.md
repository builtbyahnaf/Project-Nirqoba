# Phase 6 — MVP Stabilization and Regression Gate

**Status:** Planned (retrospective plan; the phase is implemented and reported in
[`reports/phase-06.md`](../reports/phase-06.md))
**Author:** Nirqoba architecture
**Plan version:** 1.0
**Scope sources:** [`project_context/ROADMAP.md`](../project_context/ROADMAP.md) §"Phase 6",
[`project_context/ARCHITECTURE.md`](../project_context/ARCHITECTURE.md)
**Target report:** [`reports/phase-06.md`](../reports/phase-06.md)

---

## 1. Purpose

Phase 6 exists to convert "the pipeline works on the examples we wrote" into "the pipeline
works on the examples we wrote *and* fails predictably on everything else". It is a quality
phase, not a feature phase: no new language surface should be introduced here. The output is a
regression gate that every later phase (6.5 memory, 6.7 allocators, 6.9 GC) is measured
against.

The gate has three obligations:

1. **Automation** — one command builds both configurations and runs everything.
2. **Coverage of the language as it actually is** — every implemented construct has at least
   one positive program, and every deliberate rejection has a negative program asserting the
   diagnostic.
3. **Honest failure** — a broken compiler, a crashed binary or a wrong exit code must fail the
   run loudly, never be skipped.

## 2. Decisions

| # | Decision | Rationale |
|---|----------|-----------|
| 6-D1 | Integration tests are **source directories**, not a bespoke test DSL. Each case is `main.nrqba` + `expect.json`. | Adding a case must require no framework work. The expectations are data, so the runner stays trivial. |
| 6-D2 | The runner compiles, links and executes every positive case for real (`tests/run_tests.py`). It never compares IR text alone. | IR-text tests break on every cosmetic lowering change and prove nothing about behaviour. |
| 6-D3 | Negative cases assert **diagnostic category and message substring** plus a non-zero exit, not the full rendered diagnostic. | Full-text assertions make wording improvements fail the suite. |
| 6-D4 | Unit tests cover compiler-internal components (lexer, parser, TIR verifier, type checker, lowering) through a single `test_harness.h`. | Seam-level testing localises a breakage to a pass instead of to "somewhere in the pipeline". |
| 6-D5 | CI runs Windows + MSVC only for now; the POSIX path is built and tested locally but not gated. | The runtime has real Windows APIs in it; a Linux job would fail for scaffolding reasons and erode trust in the gate. Tracked as a limitation, not a pass. |
| 6-D6 | `-WX` (warnings are errors) on the release preset only. | Dev iteration stays fast; shipping code cannot accumulate warnings. |
| 6-D7 | Defects found during the phase are fixed in the phase, not deferred, **unless** the fix would require a language-design decision. Those are recorded as blockers. | Matches `AGENTS.md`: implementation may not invent specification. |

## 3. Prerequisites

- Phases 1–5 complete: lexer, parser, TIR, type checker, MLIR translation, LLVM lowering,
  native linking, `nrqc` driver, and the core runtime print/args/memory entry points.
- A pinned LLVM/MLIR toolchain (`toolchain/versions.lock.json`) so CI and local builds agree.
- `scripts/build.ps1` and `scripts/test.ps1` as the only supported entry points.

## 4. Repository layout

```
tests/
  CMakeLists.txt            # unit test registration
  support/test_harness.h    # assertion + temp-file helpers shared by unit tests
  unit/                     # one file per compiler component
  integration/
    positive/<case>/main.nrqba + expect.json
    negative/<case>/main.nrqba + expect.json
  run_tests.py              # integration runner (build → run → compare)
.github/workflows/ci.yml    # configure, build both presets, ctest, run_tests.py
```

## 5. Implementation steps

1. **Harness first.** Write `test_harness.h` (a `CHECK`/`CHECK_EQ` pair, temp-directory
   helpers, a failure counter) so unit tests are cheap to add.
2. **Unit suites.** Lexer (tokens, keywords, numbers, strings, comments, error positions),
   parser (precedence, associativity, statements, recovery), TIR verifier (value numbering,
   arity, type agreement), type checker (inference, annotation mismatch, undefined names,
   call arity), lowering (op → `nrq`/LLVM op coverage).
3. **Positive integration corpus.** Every construct the spec claims is implemented gets a
   program: arithmetic and precedence, casts, control flow, functions and recursion, arrays,
   structs, tuples, strings, `defer`, `print` typing.
4. **Negative integration corpus.** One case per rule the checker enforces, including type
   mismatch, undefined identifier, wrong arity, `const` reassignment, `break` outside a loop,
   and unannotated ambiguous declarations.
5. **Runner semantics.** `run_tests.py` builds the compiler once, then per case: compile,
   link, execute with a timeout, compare stdout exactly, compare exit code, and compare
   expected stderr substrings. Any non-zero exit from a positive case is a failure.
6. **CI wiring.** Cache the toolchain, configure both presets, build with `-WX` on release,
   run `ctest`, then `run_tests.py`. Upload nothing; the log is the artefact.
7. **Triage pass.** Run the gate against the whole corpus, fix every real defect, and record
   any language-design blocker in the report's "remaining work" table.

## 6. Testing strategy

| Layer | Tool | Gate condition |
|-------|------|----------------|
| Unit | `ctest --preset windows-msvc` / `windows-msvc-release` | 100 % pass, no skips |
| Integration positive | `tests/run_tests.py` | exit 0, stdout matches, no stderr |
| Integration negative | `tests/run_tests.py` | non-zero exit, expected diagnostic substring present |
| Build hygiene | `cmake --build ... --config Release` | zero warnings (warnings are errors) |
| Diff hygiene | `git diff --check` | clean |

## 7. Acceptance criteria

- A single documented command reproduces the full gate from a clean checkout.
- Every implemented construct appears in the positive corpus; every enforced rule appears in
  the negative corpus.
- The suite is deterministic: two consecutive runs produce identical results.
- A deliberately broken runtime symbol fails the gate (verified by removing a symbol and
  confirming a red run, then restoring it).
- No test is conditionally skipped to make the gate green.

## 8. Known limitations at close

- POSIX build is not CI-gated (6-D5).
- Fuzzing is not part of the gate; the corpus is hand-written.
- The runner compares stdout exactly, so tests cannot tolerate platform newline differences —
  acceptable while Windows is the only gated target.

## 9. Risks

| Risk | Mitigation |
|------|------------|
| Corpus grows into a maintenance burden | `expect.json` is data-only; adding a case is two files |
| Negative tests become wording tests | assert substring + category, never full text (6-D3) |
| CI flakiness from toolchain download | pinned version + cache key derived from the lock file |
| Gate passes while the language is unusable in practice | the Phase 6.9 readiness audit (`docs/readiness.md`) tests whole programs, not constructs |
