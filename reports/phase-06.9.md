# Phase 6.9 Report - Advanced Memory Utilities

## Status

Partial implementation. Windows x64, MSVC, LLVM/MLIR 23.1.4.

## Implemented features

- Optional on-demand reference diagnostics for managed references.
- Memory profiler: explicit-allocation current, cumulative total, peak, allocation and deallocation counters; each capture also snapshots GC live bytes, collected bytes, collection count, and live objects.
- Read/write mapping for existing non-empty files, explicit unmap, mapping length, and checked typed reads/writes.
- Explicit pin/unpin for non-moving managed objects, with scoped release using `defer`.
- Secure allocation and optimizer-resistant zeroization on Windows.
- Available physical-memory estimate.
- Lexical `defer call(...)` cleanup, with argument capture at registration and LIFO execution on block exit, return, break, and continue. `memory.use_allocator` can pair with a deferred restore for block/function scoped selection; pins can pair with deferred `memory.unpin`.
- Owned file, mutex, mapping, and TCP socket handles with deterministic cleanup; typed resource return contracts preserve destructor identity through helper/factory calls.

## Known limits

The profiler does not report allocation hotspots; allocator free-space and fragmentation values are unavailable for general and custom allocators. GC counters are snapshots; explicit allocation current/peak/total counters do not include the GC heap. Mapping access must use checked typed accessors. Pin scopes use `defer memory.unpin`. Introspection includes physical-memory estimates, runtime version, CPU capabilities, process thread count, allocator kind, process working set, and automatic-GC state; task and device details are unavailable. File, mutex, and TCP socket wrappers exist, but GPU resources do not; exception-unwind cleanup and non-Windows support are also absent.

## Validation

- Unit tests verify explicit totals and the GC snapshot fields against the GC's own counters.
- Integration tests cover profiler fields, secure memory, mapping, reference diagnostics, pinning, deferred cleanup order and exit paths, deferred frees, and scoped allocator restoration.
- Release and RelWithDebInfo builds succeeded; CTest passed 1/1 and all 23 integration tests passed in each configuration. The latest RelWithDebInfo run, including scoped pinning, passed CTest 1/1 and all 24 integration tests.
- Profiler overhead, resource pressure, and fragmentation benchmarks have not been measured.

## Acceptance snapshot

| Requirement | Status |
|---|---|
| Reference-count diagnostics | Implemented; on-demand managed reference counts |
| Memory profiler | Partial; counters, size buckets, largest allocation, allocator attribution, GC snapshots, and measurable region fragmentation; hotspots and general/custom free-space measurement are unavailable |
| Memory mapping | Partial; Windows mappings and checked typed access |
| Memory pinning | Partial; explicit pin/unpin and scoped cleanup with `defer`; repeated-pin limit and violation counters are available |
| Deterministic resource destruction | Partial; file, mutex, mapping, and TCP socket wrappers use owned handles and lexical cleanup; unwind support has no language model |
| Secure memory | Implemented; Windows zeroization |
| Runtime introspection | Partial; available memory, version, CPU count, and allocator kind |

**Verdict:** Phase 6.9 remains incomplete. Graphics/GPU resources and task/device introspection depend on absent runtime systems; exception unwinding lacks language semantics, and profiler attribution and portability remain bounded as described above.

## 30. Typed resource returns - 2026-11-02

Owned factory functions now preserve the cleanup domain in their signature with
`owned file byte*`, `owned socket byte*`, `owned mutex byte*`, or
`owned mapping byte*`. The checker rejects a resource returned through an
unqualified pointer contract and rejects a value whose destructor does not
match the declared domain. The return domain is retained in TIR and emitted as
`nirqoba.return_resource_domain` in MLIR, and owning resource parameters carry
their domain in argument metadata. Socket integration transfers an owner
through an owning helper and back to the caller; negative coverage checks
mismatched parameter and return resources.

RelWithDebInfo validation passed: CTest 1/1 and integration 108/108. The
remaining Phase 6.9 limits above still apply.

## 13. Completion pass - 2026-11-01

Implemented since the first pass:

- Profiler depth. `memory.profiler.capture()` now also reports the allocation
  size histogram (`allocations_le_64` through `allocations_gt_65536`),
  `largest_allocation`, bytes currently live per allocator kind (`general_bytes`
  through `custom_bytes`), and region occupancy (`free_bytes`,
  `largest_free_block`, `fragmentation_bytes`). Occupancy is sampled from the
  allocators that can enumerate free space, so the general-purpose and custom
  allocators report zero, meaning "not measurable".
- Excessive-pin diagnostics. `memory.pin_count()` reports distinct pinned
  objects; a repeated pin of the same object beyond 16 is rejected and counted
  by `memory.pin_violations()`, so a forgotten `unpin` is observable without
  failing the program.
- Runtime introspection additions: `memory.total_bytes()` (installed physical
  memory) and `memory.used_bytes()` (process working set), both documented as
  operating-system estimates, and `gc.is_enabled()` for the automatic
  collection state. `memory.allocator_kind(0)` now reports the configured
  default allocator.
- Per-allocator fragmentation introspection:
  `nrq_memory_allocator_free_bytes` and
  `nrq_memory_allocator_fragmentation_bytes` in the runtime C API.
- Tests: `AllocatorTest.ReportsFreeSpaceAndFragmentation`,
  `AllocatorTest.ProfilerReportsSizeHistogramAndAllocatorAttribution`,
  `GcTest.PinDiagnosticsReportCountAndRejectExcessivePins`,
  `GcTest.AutomaticCollectionStateIsObservable`, plus the integration cases
  `memory_profiler_report` and `memory_introspection`.

Still not implemented:

- Graphics and GPU resource wrappers and cleanup during exception unwinding.
  File, mutex, mapping, and socket wrappers exist; the language has no unwind model.
- Task counts and device-memory reporting, which depend on the concurrency
  phases and GPU runtime. Process thread count is available as an estimate.
- A controlled profiler-overhead regression threshold. A 30-run comparison is
  recorded in `phase-06.9-profiler-overhead.md`; it is a host-specific baseline,
  not a general performance claim.

## 14. CPU capability query - 2026-11-01

Added `memory.cpu_features()` backed by `IsProcessorFeaturePresent` on Windows. The returned `u64` mask currently defines SSE2 (bit 0), AVX (bit 1), and AVX2 (bit 2). The operating system's usable-feature report is queried at runtime; no unsupported capabilities are inferred. Compiler semantic checking and LLVM lowering are wired, and the positive integration case checks the x64 SSE2 baseline.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and the full integration suite passed 32/32. The feature set is Windows-only, matching the runtime's current supported platform.

## 15. Deterministic file resources - 2026-11-01

Added an opaque Windows file resource API exposed as `file.open(path, writable)`, `file.write(resource, text)`, and `file.close(resource)`. Runtime handles are registered, invalid or repeated close/use returns an error status, and `defer file.close(resource)` participates in existing LIFO cleanup. Semantic analysis marks the resource invalid after close and rejects later use.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 34/34, including successful deferred close and compile-time use-after-close rejection. The initial file API currently supports text writes; read APIs, wrappers for sockets/locks/GPU objects, and error-unwind cleanup remain open work. No exception unwinding is claimed.

## 16. Owned file-resource transfer - 2026-11-01

File handles returned by `file.open` now carry ownership identity in the semantic symbol table. Initializing another binding from an owned file handle transfers the owner and invalidates the source binding; closing invalidates the current owner. Deferred cleanup checks whether any live owner remains, avoiding false double-release reports after transfer. This ownership tracking is currently limited to file resources; raw pointers and general aggregate ownership are not covered.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 35/35. Added positive ownership-transfer/deferred-close coverage and negative use-after-move coverage.

## 20. Process thread-count introspection - 2026-11-01

Added `memory.thread_count()` using a Toolhelp thread snapshot filtered by the current process ID. The API returns zero if snapshot acquisition fails and is documented as a race-prone estimate, not a synchronization guarantee. No task scheduler exists yet, so task count remains unavailable.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 38/38, including a positive nonzero process-thread count check.

## 22. Native mutex resource wrapper - 2026-11-01

Added `mutex.create`, `mutex.lock`, `mutex.unlock`, and `mutex.destroy`. Runtime handles are opaque tokens registered to mutex state; lock ownership is tracked by thread ID, recursive acquisition and foreign-thread unlock fail fast, and destroy rejects locked resources. The compiler recognizes creation as owned, inserts deterministic destruction, propagates move/use-after-destroy state, and lets a deferred unlock run before automatic destruction.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1, including a four-thread mutual-exclusion test, and integration tests passed 59/59. Positive coverage verifies deferred unlock and automatic destroy; negative coverage rejects use after destroy.

Limitation: this wrapper is Windows x64 only, matching the current runtime target. Exception-unwind cleanup remains unavailable.

## 23. Completion pass - 2026-11-02

The native resource surface has since expanded: deterministic Windows file resources support bounded reads and writes, and mutex handles support owned creation, lock/unlock validation, automatic destruction, and deferred unlock. Owned resources move between bindings, reject use after move/close/destroy, and cannot be released through a mismatched generic destructor. Cleanup runs in LIFO order on scope exit, return, break, and continue. The language has no exception or error-unwind model, so cleanup during unwinding is still undefined.

Profiler capture now reports allocation-size buckets, largest allocation, allocator-kind attribution, and measurable region free-space and fragmentation values in addition to explicit allocation totals and GC snapshots. General and custom allocator free space is reported as not measurable. Scoped pin cleanup uses `defer`; pin-count diagnostics reject excessive repeated pins. Introspection includes physical-memory estimates, runtime version, CPU features, process thread count, configured allocator kind, process working set, and automatic-GC state. Task and device-memory counts remain unavailable because those subsystems do not exist. Opaque file, mutex, and mapping pointers cannot be indexed; mapped data uses checked accessors.

Secure release zeroes the full tracked allocation before invoking its owning allocator. The allocator callback runs outside the global registry lock while the allocation owner is marked in flight and the allocator is pinned. A custom allocator regression verifies the bytes are zero at the callback boundary. Final shared release uses the same in-flight protocol and reserves its reference count at zero until the callback completes; concurrent retains and releases cannot claim the allocation during that interval.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration passed 85/85. The profiler-on/off allocation workload has a 30-run host baseline in `phase-06.9-profiler-overhead.md`. No GC benchmark was run. Phase 6.9 remains partial because task/device introspection depends on absent subsystems, exception-unwind cleanup has no language semantics, profiler thresholds need controlled hardware, and the runtime supports Windows only.

## 24. Stable file-resource tokens - 2026-11-02

Owned file handles now use monotonically issued opaque tokens instead of
metadata addresses that the system allocator could reuse. This prevents a
stale closed handle from accidentally resolving to a later file resource.
Runtime coverage verifies reopening does not reuse the old token and that stale
write/close operations fail. RelWithDebInfo validation passed: CTest 1/1 and
integration 85/85.

## 25. Opaque resource access validation - 2026-11-02

The type checker rejects indexing of file, mutex, and mapping handles. Mapped
bytes must use the checked mapping accessors, while explicit allocations and
secure buffers remain indexable. New negative integration tests exercise file
and mapped handle indexing; the existing secure-buffer program remains a
positive regression. RelWithDebInfo validation passed: CTest 1/1 and
integration 87/87.

## 26. Per-file operation ownership - 2026-11-02

File operations now retain a shared resource record after looking up its unique
token, then serialize reads, writes, and close with that file's own mutex. The
global file registry lock covers only lookup and token removal, so a slow I/O
operation on one file does not block unrelated file handles. Close removes the
token before waiting for an active operation, preventing new work from entering
while allowing an operation that already acquired the resource lock to finish.
A four-thread write/close regression checks the race and stale-token rejection.
RelWithDebInfo validation passed: CTest 1/1 and integration 87/87.

## 27. Per-mapping operation ownership - 2026-11-02

Mapping registry lookup now retains a shared mapping record and releases the
global registry mutex before size queries, checked copies, and unmapping. A
per-mapping mutex serializes access with close, so a large operation on one
mapping does not block unrelated mapping handles. Unmap removes the registry
entry only after active access has completed; concurrent later operations see
the mapping as closed. RelWithDebInfo validation passed: CTest 1/1 and
integration 87/87.

## 28. Mapping query/unmap race coverage - 2026-11-02

A temporary-file runtime test races four threads calling `mapping_size` against
unmap. Each call observes either the original mapping length or zero, and calls
after unmap return zero or an invalid-unmap result. RelWithDebInfo validation
passed: CTest 1/1 and integration 87/87.

## 29. Owned TCP socket resources - 2026-11-02

Added Windows TCP connect, text send, bounded receive, and deterministic close.
Socket tokens are monotonic opaque handles with independent send and receive
locks; full-duplex operations can progress concurrently. Close removes the
token, shuts down both directions to wake blocked operations, then waits for
both locks before releasing the socket. Stale handles are rejected. The compiler tracks socket ownership,
inserts lexical close, permits borrowed handles for I/O helpers, rejects use
after close, and blocks indexing opaque socket handles. Loopback tests cover
data flow, close during a blocked receive, and stale close; integration tests
cover lowering, cleanup, and ownership diagnostics. RelWithDebInfo validation
passed: CTest 1/1 and integration 100/100.
