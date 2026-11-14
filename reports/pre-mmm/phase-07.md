# Pre-MMM Phase 7 — Incremental subsystem migration

**Version:** `0.483-beta` (label advanced in one step from `0.477-beta`; no
separate `0.480-beta` release commit exists)  
**Date:** `2026-11-14`  
**Baseline:** `0.473-beta` (`ae26bc1`) for the compiler-artifact work; `0.477-beta` (`30a6ae6`) for the runtime work  
**Selected subsystems:** compiler-owned temporaries (1), runtime-owned buffers
(2), native resources (3)  
**Scope:** deterministic lifetime for compiler artifacts, escaping runtime
strings, and file/mapping/mutex registries. Publication only after a producing
stage succeeds; shutdown only after the entry function returns.

> **Part 3 update (`0.495-beta`):** `reports/pre-mmm/phase-07-part-3.md`
> supersedes the limitations below. Object/executable publication is now a
> rollback transaction; read strings are process-lifetime values rather than
> entry-return-limited; `system.exit` performs best-effort cleanup; aggregate
> root metadata and lowering paths are migrated. The two-file crash window,
> process-lifetime string memory, and partial container step 4 remain.
> Section 2's note that read buffers are unmigrated describes Part 1 only.

## 1. Verdict

Three roadmap subsystems are migrated:

1. **Compiler-generated files and directories.** Build outputs are move-only
   staging leases published only after their producing stage succeeds; `run`
   removes the executable and object it created unless `--keep-artifacts` is
   requested; `nirqoba new` publishes a complete staging tree with one rename.
2. **Runtime-owned strings.** The process argument descriptor no longer
   allocates at all, and `file.read_text` allocates its escaping string from a
   runtime-owned arena released exactly once by `nrq_runtime_shutdown`.
3. **Native resources.** File, mapping and mutex registries are cleared and
   their OS handles released at shutdown, so a live resource left at return is
   not owned by process exit. File handles use a generation-checked slot table
   instead of a per-open map node.

This is not a language-level ownership migration. The compiler and runtime are
native processes; their internal resources use C++ lifetime ownership rather
than the Nirqoba GC. No language syntax, ownership rule, cleanup ordering,
collector policy, or default memory policy changed.

## 2. Why this subsystem, and what was not migrated

The roadmap orders Phase 7 as:

1. compiler-owned temporary resources;
2. runtime-owned buffers and internal allocations;
3. native-resource handling;
4. container/aggregate internals;
5. async/task resources;
6. tensor/device buffers.

The compiler already owns AST, TIR, MLIR and LLVM values through `unique_ptr`,
`shared_ptr`, `OwningOpRef` and scope-bound containers. Its remaining generated
resources were files written directly to final paths and build products `run`
never removed. The runtime's remaining process-lifetime allocations were the
argument bridge, escaping `string` buffers, and OS-resource registries. These
were the next eligible boundaries.

Deliberately **not** migrated:

- **Collector pages and allocator shards.** They are a later subsystem and are
  intentionally untouched.
- **Thread-local GC debug rings.** Their registry is leaked on purpose: a
  thread-local destructor can run after namespace-scope statics are destroyed,
  so making the registry a static owner would introduce a shutdown race. The
  fixed site table remains the only heap object in that path.
- **Containers and aggregates.** Their internals are roadmap step 4 and were not
  touched in this phase.
- **Async/task and tensor/device resources.** No concurrency runtime or tensor
  subsystem exists in this tree, so there was nothing genuine to migrate.

## 3. Compiler-owned temporaries (roadmap 1)

### Artifact leases

`TemporaryArtifact` is a move-only owner of a unique sibling staging file. The
destructor removes a non-published staged file; `publish()` replaces the final
path with one rename; moving transfers cleanup responsibility. `build` stages
the COFF object and executable and publishes each only after its producing stage
succeeds. A failed build cleans staging and keeps the previous successful
artifacts. Publication failure is `NRQ5010`.

`run` executes the staged executable and removes the executable and object after
every program exit. `--keep-artifacts` publishes both at their old paths.

### Atomic scaffolds

`TemporaryDirectory` writes `src/`, `lib/`, `README.md` and both source files in
a unique sibling directory, refuses a non-empty target, and renames the complete
tree into place. Every failure path removes staging.

The build publishes two files sequentially, not as one filesystem transaction.
If executable publication fails after object publication, the new object and the
previous executable can coexist. This is an explicit limitation, not a claimed
atomic pair.

## 4. Runtime-owned buffers (roadmap 2)

### Upgrade 1 — static argument bridge

`runtime/src/args_windows.cpp` previously built an `NrqStringArray` descriptor
with `HeapAlloc` for every process. The CRT argv pointers already live for the
process, so the bridge now refreshes one process-owned static descriptor. This
removes the startup allocation and needs no shutdown hook.

### Upgrade 2 — process string arena

`file.read_text` returns a borrowed `string` whose lifetime the language cannot
express: there is no per-string destruction contract, and freeing it after the
call would leave a dangling language string. The runtime therefore owns every
escaping read buffer for the process.

`runtime/src/runtime_buffers.cpp` is a mutex-protected bump arena:

- 1 MiB blocks serve ordinary strings; an allocation larger than a block gets
  its own block;
- allocation never moves earlier strings;
- `release_process_strings()` frees every block and is idempotent;
- the committed block size was raised from 64 KiB to 1 MiB after the first
  benchmark showed 64 KiB blocks at +3.4% wall and +3.4% peak RSS. The 1 MiB
  layout removes enough block-management and free overhead to recover both.

`nrq_runtime_shutdown` releases the arena after the entry function's last
statement. Lowering emits the call before **every** `tir::OpKind::Return` in the
TIR function named `main`, including early returns and the argument wrapper's
callee. Previously it was emitted only in the argument wrapper, so a no-argument
`main` never retired anything.

`system.exit` does not return; that path still relies on process teardown. The
report does not claim deterministic cleanup there.

### Pipe deadlock found by the benchmark

The old `nrq_runtime_shutdown` flushed stdout with `FlushFileBuffers`. Output is
written with unbuffered `WriteFile`, and flushing an anonymous pipe blocks until
the reader drains it. A parent that waited for the process before reading stdout
therefore deadlocked once shutdown became reachable. The flush was removed;
shutdown now retires buffers only.

## 5. Native resources (roadmap 3)

### Registry release at shutdown

`nrq_runtime_shutdown` clears and closes live file, mapping and mutex resources:

- the file registry is detached under its lock, then each handle is closed under
  the resource's operation lock;
- mappings are detached, then each view, section and file handle is released;
- mutex resources are detached and marked closing, then their condition
  variables are notified.

Clearing before closing makes a stale token invalid instead of resolving a
half-closed resource. Explicit language close remains the normal path; shutdown
is the deterministic fallback for anything still live at return.

### File-handle slot table

The file registry used `std::unordered_map<void*, shared_ptr<FileResource>>`
with a monotonic integer token. Every open allocated a map node, and a closed
token could never be distinguished from a future token if a counter wrapped.

The registry is now an inline `std::vector<FileSlot>`:

- a token carries a 32-bit slot index in its low half and a 32-bit generation in
  its high half;
- closing clears the slot; reusing it increments the generation;
- lookup rejects a generation mismatch, so a stale token cannot reactivate a
  reused slot;
- lookup still returns a `shared_ptr`, preserving an operation's resource
  lifetime after the registry lock is dropped.

The vector may reallocate as slots are added, but tokens address indices, not
element addresses, so growth does not invalidate live handles.

## 6. Boundary and fallback rules

| Resource | Owner | Failure behavior | Fallback |
|---|---|---|---|
| COFF object during build | `TemporaryArtifact` | staged file removed; previous object kept | none needed |
| executable during build | `TemporaryArtifact` | staged file removed; previous executable kept | none needed |
| executable/object during `run` | `TemporaryArtifact` | removed after every program exit | `--keep-artifacts` publishes them |
| project scaffold | `TemporaryDirectory` | staging tree removed; target untouched | target must be absent or empty |
| argument descriptor | runtime static storage | none | process lifetime |
| `file.read_text` string | process string arena | empty string on failure | released at shutdown |
| live file/mapping/mutex | runtime registry | invalid token after close/shutdown | explicit close; shutdown fallback |

`system.exit` bypasses deterministic shutdown. A string returned by
`file.read_text` is valid until `nrq_runtime_shutdown`; it must not be retained
across that boundary. The language has no syntax to call shutdown manually, so
this is an entry-return contract, not a user-visible API.

## 7. Verification

| Check | Result |
|---|---|
| Build (RelWithDebInfo) | PASS |
| Compiler version | `0.483-beta` (Phase 7) |
| Unit suite | 252/252 |
| Integration suite | 464/464 |
| Runtime arguments | static descriptor; no `HeapAlloc` |
| Repeated `file.read_text` | PASS, 3 reads, expected checksum |
| Entry shutdown boundary | PASS, exactly 1 `nrq_runtime_shutdown` call |
| Live file handle at shutdown | closed; stale token rejected |
| Live mapping at shutdown | unmapped; registry cleared |
| Reused file slot | stale token rejected |
| GC/ownership stress | existing GC unit set passes |

New tests: `runtime_args_test`, `runtime_buffers_test`,
`runtime_shutdown_test`, `positive/file_read_repeated`, and
`positive/runtime_shutdown_boundary`. The shutdown boundary test checks emitted
IR, so a future lowering change that drops the call fails even if a program
happens to produce the same output.

No ASan/UBSan run is available in the MSVC configuration. The Phase 4 GC stress
flake remains a separate pre-existing issue.

## 8. Benchmark

`benchmarks/pre_mmm_phase7_runtime/`, Windows 11 x64, MSVC 14.51,
`RelWithDebInfo`, baseline worktree at `0.477-beta` (`30a6ae6`). Both compilers
built the same source; runs alternated order. The workload checks stdout on every
run.

### Repeated `file.read_text`

20,000 reads of a 4 KiB file, every returned string kept alive. 9 interleaved
samples:

| Measurement | 0.477-beta | 0.483-beta | Change |
|---|---:|---:|---:|
| Median wall time | 1,059.59 ms | 1,045.33 ms | **-1.35%** |
| Median peak RSS | 93,106,176 B | 91,041,792 B | **-2.22%** |
| Output checksum | `81920000` | `81920000` | identical |

The gain is small and close to run-to-run variation; it is reported as a
directional result, not a guaranteed speedup. The important change is ownership:
before, 20,000 read buffers were unreachable allocations at exit; now the arena
releases them at shutdown.

### File-handle round trip

100,000 `file.open("NUL", true)` / `file.close` pairs. 5 interleaved samples:

| Measurement | 0.477-beta | 0.483-beta | Change |
|---|---:|---:|---:|
| Median wall time | 303.85 ms | 297.66 ms | **-2.04%** |
| Median peak RSS | 9,752,576 B | 9,773,056 B | +0.21% |

The wall-time improvement is consistent with removing one map-node allocation
per open. The RSS difference is one page and is treated as noise.

### Arguments and native cleanup

The argument bridge removes one allocation per process; at process start this is
below the resolution of a wall-clock benchmark and is not claimed as a measured
speedup. Native shutdown cleanup adds work proportional to live resources at
exit. It is a correctness guarantee, not a performance optimization; no speedup
is claimed.

## 9. Commits

Runtime session:

- `runtime: make the argument descriptor process-owned static storage`
- `test: cover the static runtime argument bridge`
- `runtime: add a process-lifetime arena for borrowed strings`
- `runtime: allocate file-read buffers from the process string arena`
- `runtime: retire string buffers at shutdown without pipe flushes`
- `lowering: retire runtime buffers at every main return`
- `test: cover runtime buffer ownership, release, and concurrency`
- `test: verify repeated file reads and the entry shutdown boundary`
- `benchmark: measure the process string arena against 0.477-beta`
- `runtime: release live native registries at shutdown`
- `test: cover native-resource release at runtime shutdown`
- `runtime: store file handles in a generation-checked slot table`
- `test: reject a stale file token after its slot is reused`
- `benchmark: measure file-handle registry round trips`
- `release: bump to 0.483-beta after native-resource migration`

Compiler-artifact session at `0.477-beta`: artifact leases, scaffold
publication, flush removal, empty-target restoration, benchmark, and its tests.

## 10. Assessment

**Implementation completeness:** Roadmap steps 1–3 are migrated. Containers and
aggregates (step 4) are untouched. Async and tensor/device subsystems do not
exist in this tree, so steps 5–6 remain not applicable.

**Verification confidence:** High for argument lifetime, repeated reads, early
returns, pipe output, live-resource shutdown and stale-token rejection. The
`system.exit` path and the two-file publication window are explicit limitations.
The read benchmark gain is small enough to be directional rather than proven.

**Performance:** Repeated reads -1.35% wall and -2.22% peak RSS; file-handle
round trips -2.04% wall with RSS parity. Argument allocation removal and native
shutdown cleanup are correctness changes with no claimed speedup.

**Deferred:** container/aggregate internals; async/task resources (no runtime);
tensor/device buffers (no subsystem); collector pages and allocator shards;
ASan/UBSan (unavailable in the MSVC configuration); the pre-existing Phase 4 GC
stress flake.
