# Pre-MMM Phase 7 — Incremental subsystem migration

**Version:** `0.477-beta`  
**Date:** `2026-11-14`  
**Baseline:** `0.473-beta` (`ae26bc1`)  
**Selected subsystem:** compiler-owned temporary resources (roadmap order 1)  
**Scope:** deterministic lifetime for compiler-generated artifacts and project
scaffolds; publication only after a producing stage succeeds.

## 1. Verdict

The first roadmap subsystem is migrated for its **compiler-generated file and
directory resources**. Two implementation upgrades were made:

1. Compiler output artifacts are move-only staging leases. Builds publish an
   object and executable only after their producing stages succeed; `run`
   removes the executable and object it created unless `--keep-artifacts` is
   requested.
2. `nirqoba new` writes into a move-only staging directory and renames the
   complete project into place. A failed write or publish cannot leave a
   partially scaffolded project visible.

This is not a language-level ownership migration. The compiler is a native
process, so its internal resources use C++ lifetime ownership rather than the
Nirqoba GC. No runtime collector, allocator, language syntax, cleanup rule or
default memory policy changed.

## 2. Why this subsystem, and what was not migrated

The roadmap orders Phase 7 as:

1. compiler-owned temporary resources;
2. runtime-owned buffers and internal allocations;
3. native-resource handling;
4. container/aggregate internals;
5. async/task resources;
6. tensor/device buffers.

The compiler already owns its AST, TIR, MLIR and LLVM values through
`unique_ptr`, `shared_ptr`, `OwningOpRef` and scope-bound containers. The
remaining compiler-generated resources were files and directories written
directly to final paths, plus build products that `run` never removed. Those are
the first eligible boundary.

The following were deliberately **not** migrated:

- **Runtime file-read buffers.** `nrq_file_read_text` returns a process-heap
  buffer whose lifetime follows the returned string. The language has no
  borrowing contract for that external buffer, so forcing deterministic release
  would create a dangling string. It remains on the existing runtime/GC
  fallback path and is documented as an unsupported ownership pattern.
- **Process-lifetime argument descriptor.** It is one allocation created at
  startup and has no per-operation owner; it is not a temporary resource.
- **Collector pages and allocator shards.** They are a later subsystem and are
  intentionally untouched.

## 3. Upgrade 1 — artifact leases and ephemeral run outputs

### Before

`run_pipeline` wrote the COFF object and linker output directly to their final
paths. `nirqoba run` then left both files behind, even though the executable was
only needed for that run. A failed link could also leave a new object or
executable beside an older successful build, making the output directory look
valid when it was not.

### Implementation

`TemporaryArtifact` is a move-only owner of a staging file:

- the staged path is a unique sibling of the final path;
- the destructor removes the staged file on every non-publishing exit;
- `publish()` replaces the final path with one rename;
- moving transfers cleanup responsibility and leaves the source empty;
- `discard()` releases immediately.

The driver stages the object and executable before invoking the pipeline. A
successful `build` publishes the object and then the executable. A failed build
returns before publication, so the previous artifacts remain and the new staged
files are removed. `run` executes the staged executable and removes both staged
files when the program exits unless `--keep-artifacts` is present.

The Windows publication uses `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` without
`MOVEFILE_WRITE_THROUGH`. Publication is an in-process atomicity boundary, not a
crash-durability promise; the synchronous flush added a measured multi-
millisecond cost to every build and was removed after the first benchmark run.

### Test coverage

- `TemporaryArtifactTest.DestructionRemovesOnlyTheStagedFile`
- `TemporaryArtifactTest.MoveTransfersCleanupResponsibility`
- `TemporaryArtifactTest.PublishReplacesTheFinalArtifact`
- `TemporaryArtifactTest.PublishWithoutStagedFileLeavesTheOldArtifact`
- `TemporaryArtifactTest.StagedNamesAreUniqueSiblings`
- `CliTest.RunKeepsArtifactsOnlyWhenRequested`
- `positive/run_artifact_cleanup` — `run` leaves no `.exe` or `.obj`
- `positive/build_artifact_publication` — `build` publishes both files
- `positive/hello` — `--keep-artifacts` preserves the object for the existing
  COFF-header check

## 4. Upgrade 2 — atomic project scaffolding

### Before

`nirqoba new` created `src/`, `lib/`, `README.md` and the two source files
directly in the target. A write failure could leave a directory that looked like
a project but was missing a file. A pre-existing empty directory was also used
as the write target immediately.

### Implementation

`TemporaryDirectory` is the directory analogue of the artifact lease:

- `nirqoba new` creates a unique sibling staging directory;
- all three files and both subdirectories are written there;
- `publish()` refuses a non-empty target, removes an empty target at the commit
  point, and renames the staging tree into place;
- every failure path removes the staging tree in the destructor.

The success output and the three generated files are unchanged.

### Test coverage

- `TemporaryDirectoryTest.PublishesACompleteTreeOverAnEmptyTarget`
- `TemporaryDirectoryTest.RefusesANonEmptyTargetAndRemovesStaging`
- `TemporaryDirectoryTest.MoveTransfersDirectoryCleanup`
- Manual `nirqoba new` against a missing directory, an existing empty directory
  and a non-empty directory

## 5. Boundary and fallback rules

| Resource | Owner | Failure behavior | Fallback |
|---|---|---|---|
| COFF object during build | `TemporaryArtifact` | staged file removed; previous object kept | none needed |
| executable during build | `TemporaryArtifact` | staged file removed; previous executable kept | none needed |
| executable/object during `run` | `TemporaryArtifact` | removed after every program exit | `--keep-artifacts` publishes them |
| project scaffold | `TemporaryDirectory` | staging tree removed; target untouched | target must be absent or empty |
| runtime file-read string | runtime/GC fallback | unchanged | documented unsupported ownership |

The build publishes two files sequentially, not as one filesystem transaction.
If publication of the executable fails after the object was published, the new
object and the previous executable can coexist. Publication failures are
reported as `NRQ5010`; the staging file is cleaned. A crash between the two
renames has the same narrow window. This is an explicit limitation, not a
claimed atomic pair.

`--keep-artifacts` is the compatibility fallback for users who relied on
`nirqoba run` leaving its build products. It publishes the same paths the old
implementation used.

## 6. Verification

| Check | Result |
|---|---|
| Clean build (RelWithDebInfo) | PASS |
| Compiler version | `0.477-beta` (Phase 7) |
| Unit suite | 237/237 |
| Integration suite | 462/462 |
| Artifact unit cases | PASS |
| `run` artifacts removed | PASS |
| `build` artifacts published | PASS |
| `--keep-artifacts` preserves object | PASS |
| `nirqoba new` missing/empty target | PASS |
| `nirqoba new` non-empty target | rejected, unchanged |
| Generated executable hash | identical to 0.473-beta |
| GC/ownership stress | full unit GC set passes |

The runtime, allocator, lowering and optimizer were not changed. The executable
from `hello` is byte-identical between 0.473 and 0.477
(`6d4d6ff0763c68a54d4b57ca227ac7c4674f2b8cae3f3aaaae95696515e8d7a9`), which
confirms that the migration changed artifact lifetime rather than code
generation.

No ASan/UBSan run is available in the MSVC configuration. The Phase 4 GC stress
flake remains a separate pre-existing issue.

## 7. Benchmark

`benchmarks/pre_mmm_phase7/`, 41 interleaved samples, Windows 11 x64, MSVC
14.51, `RelWithDebInfo`:

| Measurement | 0.473-beta | 0.477-beta | Change |
|---|---:|---:|---:|
| Build wall time | 99.298 ms | 102.779 ms | **+3.51%** (+3.48 ms) |
| Run wall time | 157.214 ms | 155.831 ms | **-0.88%** (-1.38 ms) |
| `new` wall time | 33.933 ms | 36.325 ms | **+7.05%** (+2.39 ms) |
| Compiler peak RSS | 30,298,112 B | 30,371,840 B | **+0.24%** |
| Artifacts left by `run` | 2 files | **0 files** | **-2** |
| Bytes left by `run` | 10,300 B | **0 B** | **-100%** |
| Generated executable | 9,728 B | 9,728 B | identical SHA-256 |

The build and scaffold costs are small in absolute terms but are real
publication overhead; they are reported rather than hidden. The first
benchmark run measured a larger build regression because publication used a
synchronous disk flush. Removing that flush reduced the overhead; a 21-sample
run measured build at -0.68%, while the final 41-sample run measured +3.51%, so
run-to-run variation is of the same order as the effect. The `new` command still
pays roughly 2 ms for creating a staging directory and renaming it, a one-time
cost per project.

## 8. Commits

- `driver: give compiler artifacts deterministic ownership`
- `test: cover compiler artifact leases and run cleanup`
- `driver: publish project scaffolds atomically`
- `test: cover atomic project-directory publication`
- `driver: publish artifacts without a synchronous disk flush`
- `driver: restore an empty scaffold target when publication fails`
- `benchmark: measure Phase 7 artifact lifetime and publication cost`
- documentation, roadmap, version, and progress commits follow.

## 9. Assessment

**Implementation completeness:** The compiler-owned temporary-resource boundary
is migrated for build artifacts, run products and project scaffolds.
**Verification confidence:** High for lifetime, early-exit cleanup, move
transfer, pre-existing-file safety and scaffold publication. The sequential
two-file publication window is documented and not claimed atomic.
**Performance:** Build/run parity within measurement noise; `new` has a small,
explicit staging cost; artifact leakage from `run` is eliminated.
**Remaining work:** runtime-owned buffers (including file-read strings), native
resource internals, container internals and async resources remain on their
existing paths. They are not partially migrated by this phase.
