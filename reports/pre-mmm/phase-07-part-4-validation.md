# Pre-MMM Phase 7 Part 4 — Final validation

**Version:** `0.499-beta`  
**Baseline:** `0.495-beta` (`bb99ffd`)  
**Date:** `2026-11-15`

## Final checks

| Check | Result |
|---|---|
| Clean RelWithDebInfo build (`-Clean`) | **PASS** |
| Compiler version | `0.499-beta (Phase 7)` |
| LLVM/MLIR | 23.1.4 |
| Unit suite | **259/259 PASS** |
| Integration suite | **474/474 PASS** |
| Runtime parity check | 104 invariants |
| SoA field rooting | PASS |
| SoA field release on aggregate drop | PASS |
| SoA field in tuple | PASS |
| SoA field moved/reinitialized | PASS |
| SoA field in returned aggregate | PASS |
| SoA and shared fields in one drop | PASS |
| Empty SoA field in aggregate | PASS |
| `uninit` then SoA field assignment | PASS |
| Unit lowering check | PASS, frame + register + allocate + unregister |
| Sanitizers | unavailable in the MSVC configuration |
| GC stress | pre-existing Phase 4 flake remains separate; no new harness |

The clean build was run after the final source changes. Both suites ran against
the resulting `0.499-beta` compiler. The benchmark executable was built by the
benchmark runner, not by the test suite.

One intermediate unit run during the session reproduced the pre-existing GC
stress flake (`GcTest.MinorReclaimsOnlyYoungGarbageAndKeepsOldGarbage`); a
rerun and the final clean run both passed. It is reported rather than hidden.

## Commit accounting

Part 4 adds **25 commits** after `bb99ffd`, including this validation report:

- 1 lowering implementation commit for the two SoA ownership fixes and the
  no-heap aggregate traversal;
- 7 focused test commits (integration and unit coverage);
- 3 benchmark commits (harness, result, README);
- 1 release-version commit;
- 13 documentation, comment and reconciliation commits.

No commit is pushed. `0.457-beta` remains the released `n-1` checkpoint.

## Version history

- `0.480-beta` — retrospective Part 2 milestone: argument bridge and string
  arena; shipped in `0.483-beta`.
- `0.483-beta` — Part 2 release: every `main` return, native registries, file
  slots.
- `0.484-beta` — retrospective Part 3 milestone: artifact transaction and
  process-lifetime strings; shipped in `0.491-beta`.
- `0.487-beta` — retrospective Part 3 milestone: exit cleanup and inline shared
  roots; shipped in `0.491-beta`.
- `0.491-beta` — aggregate paths and mutex slot table.
- `0.495-beta` — best-effort termination cleanup (one implementation upgrade).
- `0.499-beta` — Part 4 container audit and aggregate SoA fixes.

Only `0.499-beta` carries the working-build label. The 0.480, 0.484 and 0.487
entries are explicitly marked as retrospective; no release commit is fabricated
for them.

## Benchmark

`benchmarks/pre_mmm_phase7_part4/`, baseline `bb99ffd`, 15 alternating builds of
5,000 aggregate drop sites:

| Measurement | Baseline | Part 4 | Change |
|---|---:|---:|---:|
| Compiler wall | 7,454.763 ms | 7,507.598 ms | **+0.71%** |
| Compiler peak RSS | 417,304,576 B | 417,480,704 B | +0.04% |
| Object size | 1,362,995 B | 1,362,995 B | identical |
| Executable size | 622,592 B | 622,592 B | identical |

The result is neutral. No speedup is claimed for removing the per-site
`std::function`. The SoA correctness cases cannot run on the baseline because it
aborts on them.

## Rollback points

- `bb99ffd` — clean `0.495-beta` Part 3 baseline, built in a disposable
  worktree for the Part 4 benchmark.
- `d63bb9a` — the `0.499-beta` release commit; revert it alone to return the
  label to `0.495-beta` while keeping the reviewed Part 4 implementation.
- `e0ed8d7` (`0.491-beta`) and `70126a6` (`0.483-beta`) remain the earlier
  Part 3 and Part 2 rollback commits.
- The Part 4 worktree is removed before handoff; no rollback state depends on
  it.

## Roadmap status

| Step | Status |
|---|---|
| 1. Compiler-owned temporary resources | **Complete** |
| 2. Runtime-owned buffers | **Complete for the current string model** |
| 3. Native resources | **Complete for compiled file/mapping/mutex registries** |
| 4. Container and aggregate internals | **Complete**: audit and SoA aggregate fixes |
| 5. Async/task resources | Not applicable; no concurrency runtime in tree |
| 6. Tensor/device buffers | Not applicable; no tensor subsystem in tree |

**Phase 7 is complete** for the subsystems present in this tree. Publication
crash recovery, bounded read-string release, and busy-registry termination
cleanup remain documented hardening work for a later phase; they are not
unmigrated container ownership paths.

## Remaining limitations

1. A process crash between object and executable renames can leave a partial
   pair and hard-link backups; there is no journal or automatic recovery.
2. Read-string memory is process-lifetime; it is not returned before
   termination.
3. `system.exit` abandons busy registries to OS teardown rather than waiting.
4. SoA storage is collector-owned: `soa.owned_release` unregisters the root but
   does not synchronously free the object.
5. Sockets are deliberately unlinked from the runtime and are not an applicable
   migration.
6. No sanitizer run is available with the MSVC toolchain.
7. The pre-existing Phase 4 GC stress flake is outside Part 4.

## Verdict

Part 4 completes the container audit. It fixes the two real gaps found in
aggregate SoA ownership, keeps the collector-backed SoA contract explicit, and
shows the traversal refactor is performance-neutral. Phase 7 is complete for
roadmap steps 1-4; steps 5-6 have no subsystem to migrate.