# Pre-MMM Phase 7 Part 3 — Final validation

**Version:** `0.495-beta`  
**Baseline:** `0.483-beta` (`70126a6`)  
**Date:** `2026-11-15`

## Final checks

| Check | Result |
|---|---|
| Clean RelWithDebInfo build (`-Clean`) | **PASS** |
| Compiler version | `0.495-beta (Phase 7)` |
| LLVM/MLIR | 23.1.4 |
| Unit suite | **258/258 PASS** |
| Integration suite | **466/466 PASS** |
| Runtime parity check | 104 invariants |
| Artifact transaction rollback | PASS |
| String aliases across copy/return/struct/shared | PASS |
| `system.exit` code and unreachable tail | PASS |
| Exit cleanup with free registries | PASS |
| Shared-root inline and overflow offsets | PASS |
| Mutex slot reuse | PASS |
| Sanitizers | unavailable in the MSVC configuration |
| GC stress | pre-existing Phase 4 flake remains separate; no new stress harness |

The clean build was run after the final source changes. Both suites were then run
against the resulting `0.495-beta` compiler. Benchmark executables were built by
the benchmark runner, not by the test suite.

## Commit accounting

Part 3 adds **25 commits** after `70126a6`:

- 4 artifact-transaction implementation/test commits;
- 4 string-lifetime and `system.exit` implementation/test commits;
- 3 aggregate-internal and shared-root commits;
- 2 mutex registry commits;
- 2 termination-cleanup commits;
- 2 benchmark commits;
- 2 release-version commits;
- 6 documentation/validation commits.

No commit is pushed. `0.457-beta` remains the released `n-1` checkpoint.

## Version history

- `0.491-beta` records four Part 3 upgrades.
- `0.495-beta` records the next two.
- The intended `0.487-beta` intermediate label after the first pair was not
  committed; it is not fabricated retroactively.
- The earlier missing `0.480-beta` remains documented as a process gap from
  Part 2.

## Rollback points

- `70126a6` — clean `0.483-beta` Part 2 baseline, built in a disposable worktree
  for all Part 3 comparisons.
- `0.477-beta` (`30a6ae6`) and `0.473-beta` (`ae26bc1`) remain the earlier
  rollback points for Parts 1 and 2.
- The worktree is removed before handoff; no rollback state depends on it.

## Roadmap status

| Step | Status |
|---|---|
| 1. Compiler-owned temporary resources | **Complete** |
| 2. Runtime-owned buffers | **Complete for the current string model** |
| 3. Native resources | **Complete for compiled file/mapping/mutex registries** |
| 4. Container and aggregate internals | **Partial**: aggregate metadata/traversal migrated; container audit remains |
| 5. Async/task resources | Not applicable; no concurrency runtime in tree |
| 6. Tensor/device buffers | Not applicable; no tensor subsystem in tree |

**Phase 7 is not complete** because roadmap step 4's container-internals audit
remains, and publication crash recovery and bounded string release are unresolved
design work.

## Remaining limitations

1. A process crash between object and executable renames can still leave a
   partial pair and hard-link backups; there is no journal or automatic recovery.
2. Read-string memory is process-lifetime; it is not returned before termination.
3. `system.exit` abandons busy registries to OS teardown rather than waiting.
4. Container internals were not redesigned; owned arrays, views and SoA already
   use deterministic ownership/GC roots.
5. Sockets are deliberately unlinked from the runtime and are not an applicable
   migration.
6. No sanitizer run is available with the MSVC toolchain.
7. The pre-existing Phase 4 GC stress flake is outside Part 3.

## Verdict

Part 3 resolves the handled publication mismatch, the read-string lifetime
restriction, and termination cleanup for uncontended registries. It migrates
aggregate root metadata and mutex storage. It does **not** claim a crash-safe
publication transaction, bounded string release, or a complete container audit.
