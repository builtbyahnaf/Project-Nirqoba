# Pre-MMM Phase 7, Part 3 — Remaining limitations and roadmap step 4

**Version:** `0.495-beta`  
**Date:** `2026-11-14`  
**Baseline:** `0.483-beta` (`70126a6`)  
**Scope:** artifact publication, runtime string lifetime, termination cleanup,
aggregate internals, and the remaining native-resource registry.

## 1. Verdict

Six implementation upgrades were made:

1. Object and executable publication is a rollback transaction.
2. `file.read_text` strings have a process-lifetime contract instead of an
   entry-return invalidation rule.
3. `system.exit` performs best-effort deterministic native-resource cleanup.
4. Shared aggregate root metadata keeps its common offset layout inline.
5. Mutex handles use a generation-checked slot table.
6. Termination cleanup uses try-lock so a blocked operation cannot prevent exit.

Roadmap step 4 is partially complete: aggregate metadata and lowering paths were
migrated, but the language container surface (owned arrays and SoA) was audited
rather than changed because it already uses the ownership/GC-root path. Phase 7
is not marked complete.

## 2. Artifact publication

`ArtifactTransaction` takes ownership of the staged object and executable and
publishes them with these steps:

1. For every existing final, create a sibling hard-link backup (copy fallback).
2. Replace each final with its staged artifact.
3. On any failure, restore published finals from their backups and remove finals
   that did not exist before.
4. On success, remove the backups.

A returned failure therefore cannot leave a new object beside an old executable.
The previous single-artifact `TemporaryArtifact` path remains for one-output
uses and for `run` staging.

**Measured cost:** 9 interleaved end-to-end builds, baseline `0.483-beta` vs
current:

| Measurement | Baseline | Current | Change |
|---|---:|---:|---:|
| Median build wall | 108.433 ms | 111.931 ms | **+3.23%** |
| Median peak RSS | 30,412,800 B | 30,486,528 B | +0.24% |
| Object size | 815 B | 815 B | identical |
| Executable size | 21,504 B | 19,968 B | **-7.14%** |

The ~3.5 ms cost is two hard-link creations and their cleanup. It is a safety
cost, not a speedup.

**Unresolved:** a process crash between the two renames can still leave a
partially published pair with backups on disk. There is no journal or automatic
crash recovery yet. This is reported as a limitation; handled failures are
atomic.

## 3. Runtime string lifetime

`file.read_text` returns a raw `char*` with no compiler-tracked release point.
Copies, returns, aggregate fields and shared handles all copy that pointer. A
per-string or entry-return free is therefore unsound once any alias can outlive
the call.

The contract is now:

- the buffer is immutable after the call;
- it is valid for the entire process, including deferred cleanup and static
  destructors;
- the arena is process-owned and reclaimed by the operating system at
  termination;
- `release_process_strings()` is an internal test/forced-teardown hook, not part
  of normal shutdown.

The integration case `file_read_text_lifetime` exercises an original value, a
copy, a struct field, a shared alias, `.length`, and a returned value. The unit
suite checks validity after `nrq_runtime_shutdown`.

**Measured cost:** 20,000 × 4 KiB reads, 9 samples:

| Measurement | Baseline | Current | Change |
|---|---:|---:|---:|
| Median wall | 867.168 ms | 868.243 ms | **+0.12%** |
| Median peak RSS | 91,066,368 B | 92,069,888 B | **+1.10%** |

The wall difference is noise; the RSS difference is approximately 1 MiB and is
page/block-accounting variation. No speedup is claimed.

**Unresolved:** memory used by read strings is not returned before process exit.
A bounded or per-scope release requires an owned-string type or compiler-inserted
retain/release, which does not exist.

## 4. Termination cleanup

`system.exit` now calls `nrq_runtime_shutdown_for_exit()` before `ExitProcess`.
Normal entry return keeps the waiting shutdown path. The termination path uses
`try_lock` for the file, mapping and mutex registries: an uncontended registry is
retired normally; a registry locked by another thread is skipped and reclaimed
by the OS. This avoids a thread blocked in file/socket I/O preventing the process
from terminating.

The integration case `system_exit_shutdown` verifies the explicit exit code and
that no statement after `system.exit` runs. The unit suite verifies uncontended
exit cleanup retires a live file handle.

**Unresolved:** busy registries are intentionally abandoned to process teardown.
Asynchronous cancellation and signal-safe cleanup remain out of scope.

## 5. Aggregate and container internals

`SharedRootMetadata` now stores up to eight managed-reference offsets inline and
uses a vector only for deeper layouts. Most shared aggregates no longer allocate
a separate offsets vector at the shared-allocation site. The unit test exercises
both the inline and overflow paths.

Aggregate drop/retain lowering uses `llvm::SmallVector<int64_t, 8>` for the field
path instead of a heap `std::vector`, removing an allocation from ordinary owned
aggregate destruction and shared-aggregate copying.

Owned arrays, array views and SoA already use the deterministic ownership and
GC-root registration paths. No separate runtime container allocator exists to
migrate, so step 4 is recorded as **partially complete** rather than claiming a
container redesign that was not made.

## 6. Mutex registry

Mutex handles now use an inline slot table with a 32-bit index and generation in
each token. Destroy clears the slot; reuse increments the generation, so a stale
token cannot name a new mutex. This removes one map node per create/destroy and
keeps stale-token rejection.

**Measured:** 100,000 create/lock/unlock/destroy cycles, 9 samples:

| Measurement | Baseline | Current | Change |
|---|---:|---:|---:|
| Median wall | 47.396 ms | 42.319 ms | **-10.71%** |
| Median peak RSS | 9,768,960 B | 9,781,248 B | +0.13% |

## 7. Verification

| Check | Result |
|---|---|
| RelWithDebInfo build | PASS |
| Compiler version | `0.495-beta` |
| Unit suite | 258/258 |
| Integration suite | 466/466 |
| Artifact transaction rollback | PASS (unit) |
| String copies/returns/aggregate/shared aliases | PASS |
| `system.exit` code and unreachable tail | PASS |
| Exit cleanup with free registries | PASS |
| Shared-root inline and overflow offsets | PASS |
| Mutex slot reuse | PASS |
| Existing aggregate and shared-pointer cases | PASS |

No ASan/UBSan run is available in the MSVC configuration. The pre-existing
Phase 4 GC stress flake remains separate. No dedicated stress harness exists for
the new transaction; its failure paths are unit-tested by forcing the second
rename to fail.

## 8. Version history

- `0.483-beta` — Part 2 baseline.
- `0.491-beta` — four Part 3 upgrades recorded.
- `0.495-beta` — two more upgrades recorded.

The intended intermediate `0.487-beta` label after the first two upgrades was not
committed. It is not fabricated retroactively. This is the second such process
gap, after the missing `0.480-beta`; future milestone commits must be made at the
upgrade boundary rather than after a batch.

## 9. Remaining limitations

1. Publication has no crash journal; a crash between renames can leave a partial
   pair and backup files.
2. Read-string memory is process-lifetime and not released before termination.
3. `system.exit` skips busy registries instead of waiting.
4. Aggregate/container step 4 is partial; no container ownership redesign was
   attempted.
5. The socket runtime is deliberately unlinked from the runtime and is not an
   applicable migration in this phase.
6. Async/task and tensor/device subsystems do not exist in this tree.
7. No sanitizer run is available with this MSVC configuration.

## 10. Assessment

**Implementation completeness:** Publication and string lifetime are resolved
for the current language model; termination is best-effort by design. Aggregate
internals are migrated; container internals are partial. Phase 7 is not complete.

**Confidence:** High for the rollback transaction, string aliasing, exit code,
slot generation, and aggregate root metadata. Medium for crash behavior and
termination with a busy registry, both documented.

**Performance:** Publication regresses 3.23% for safety; mutex registry improves
10.71%; read workload is neutral within noise. No universal speedup is claimed.
