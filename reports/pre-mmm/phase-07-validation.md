# Pre-MMM Phase 7 — Final validation

**Version:** `0.483-beta`  
**Date:** `2026-11-14`  
**Commit range:** `30a6ae6` (`0.477-beta`) through the documentation head of this
session.

## Result

| Check | Result |
|---|---|
| RelWithDebInfo build | PASS |
| Compiler version | `0.483-beta (Phase 7)` |
| LLVM/MLIR | 23.1.4 |
| Unit suite | **252/252 PASS** |
| Integration suite | **464/464 PASS** |
| Shutdown boundary IR | exactly 1 `nrq_runtime_shutdown` call |
| Repeated `file.read_text` | PASS |
| Live file handle at shutdown | PASS, stale token rejected |
| Live mapping at shutdown | PASS, unmapped |
| Reused file slot | PASS, stale token rejected |
| Shutdown idempotence | PASS |
| Mutex registry after shutdown | PASS |
| ASan/UBSan | not available in the MSVC configuration |
| GC stress | the pre-existing Phase 4 flake remains separate; no new stress harness exists |

The build was incremental `RelWithDebInfo`, not a from-scratch rebuild. Unit and
integration suites were rerun against the final compiler binary. The benchmark
executables were built separately by `benchmarks/pre_mmm_phase7_runtime/run.py`.

## Commit accounting

The session adds 25 commits after `30a6ae6`:

- 15 implementation, test, benchmark and release commits before documentation;
- 5 subsystem documentation commits;
- 1 additional shutdown/registry test commit;
- 1 benchmark methodology commit;
- 3 final report, progress and validation commits.

No commit is pushed. `0.457-beta` remains the released `n-1` checkpoint.

The requested `0.480-beta` intermediate label was built during the session but
its commit was not created; the only version commit is `0.483-beta`. This report
does not claim an intermediate release that does not exist in history.

## Migration completeness

| Roadmap step | Status |
|---|---|
| 1. Compiler-owned temporary resources | **Complete** (`0.477-beta`) |
| 2. Runtime-owned buffers and internal allocations | **Complete** (`0.483-beta`) |
| 3. File, mapping, mutex and other native resources | **Complete** (`0.483-beta`) |
| 4. Container and aggregate internals | **Not started** |
| 5. Async/task resources | **Not applicable**, no concurrency runtime in tree |
| 6. Tensor/device buffers | **Not applicable**, no tensor subsystem in tree |

Phase 7 is therefore **partially complete**: three of four applicable subsystem
boundaries are migrated. Containers and aggregates remain on their existing
behavior.

## Performance summary

| Workload | Change |
|---|---:|
| 20,000 x 4 KiB `file.read_text`, median wall | -1.35% |
| Same, median peak RSS | -2.22% |
| 100,000 file open/close, median wall | -2.04% |
| Same, median peak RSS | +0.21% (one page, noise) |

The read result is directional and close to run-to-run variation. No speedup is
claimed for the static argument descriptor or native shutdown cleanup, which are
correctness changes.

## Explicit limitations

- `system.exit` bypasses deterministic shutdown; process teardown remains the
  fallback on that path.
- `file.read_text` strings are borrowed and valid only until entry return.
- Build object and executable publication are two sequential renames, not one
  filesystem transaction.
- The file-slot vector may reallocate; tokens are indices, not addresses, so this
  does not invalidate handles.
- No sanitizer run was possible with MSVC in this environment.
- Container/aggregate destruction and alias rules need their own design pass and
  were not started.

**Verdict:** the migrated runtime and native-resource paths are complete for
their stated boundaries and pass the full suite. Phase 7 as a whole is not
finished because roadmap step 4 remains.
