# Phase 6.9 Report â€” Advanced Memory Utilities

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

## Known limits

The profiler does not provide fragmentation estimates, allocator-specific attribution, allocation-size histograms, or hotspots. GC counters are snapshots; the profiler's explicit allocation current/peak/total counters do not include the GC heap. Raw mapping pointer indexing cannot be checked; use the typed mapped access APIs. Pin scopes use `defer memory.unpin`; excessive-pin diagnostics are absent. Introspection reports available physical memory, runtime version, logical CPU count, and built-in allocator kind; thread/task/device details are absent. The runtime lacks native resource wrapper types, exception-unwind cleanup, and non-Windows support.

## Validation

- Unit tests verify explicit totals and the GC snapshot fields against the GC's own counters.
- Integration tests cover profiler fields, secure memory, mapping, reference diagnostics, pinning, deferred cleanup order and exit paths, deferred frees, and scoped allocator restoration.
- Release and RelWithDebInfo builds succeeded; CTest passed 1/1 and all 23 integration tests passed in each configuration. The latest RelWithDebInfo run, including scoped pinning, passed CTest 1/1 and all 24 integration tests.
- Profiler overhead, resource pressure, and fragmentation benchmarks have not been measured.

## Acceptance snapshot

| Requirement | Status |
|---|---|
| Reference-count diagnostics | Implemented; on-demand managed reference counts |
| Memory profiler | Partial; explicit counters plus GC snapshots, without fragmentation or attribution |
| Memory mapping | Partial; Windows mappings and checked typed access |
| Memory pinning | Partial; explicit pin/unpin and scoped cleanup with `defer`; excessive-pin diagnostics absent |
| Deterministic resource destruction | Partial; `defer` call cleanup and scoped allocator restoration, no native wrapper types or unwind support |
| Secure memory | Implemented; Windows zeroization |
| Runtime introspection | Partial; available memory, version, CPU count, and allocator kind |

**Verdict:** Phase 6.9 remains incomplete. Generalized resource integration, full profiler attribution, scoped pinning, and broader introspection remain required.
## 13. Completion pass â€” 2026-11-01

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

- Native resource wrapper types (files, sockets, locks) and cleanup during
  exception unwinding. The roadmap leaves the exact shape of these types to the
  language specification, which still defines none, so only the existing
  `defer call(...)` mechanism implements deterministic destruction today.
- Thread/task counts and device-memory reporting in introspection, which depend
  on the concurrency phases and the GPU runtime.
- Profiler overhead measurement. Allocation benchmarks exist, but the cost of
  running with the profiler enabled versus disabled has not been measured.

## 14. CPU capability query — 2026-11-01

Added `memory.cpu_features()` backed by `IsProcessorFeaturePresent` on Windows. The returned `u64` mask currently defines SSE2 (bit 0), AVX (bit 1), and AVX2 (bit 2). The operating system's usable-feature report is queried at runtime; no unsupported capabilities are inferred. Compiler semantic checking and LLVM lowering are wired, and the positive integration case checks the x64 SSE2 baseline.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and the full integration suite passed 32/32. The feature set is Windows-only, matching the runtime's current supported platform.

## 15. Deterministic file resources — 2026-11-01

Added an opaque Windows file resource API exposed as `file.open(path, writable)`, `file.write(resource, text)`, and `file.close(resource)`. Runtime handles are registered, invalid or repeated close/use returns an error status, and `defer file.close(resource)` participates in existing LIFO cleanup. Semantic analysis marks the resource invalid after close and rejects later use.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 34/34, including successful deferred close and compile-time use-after-close rejection. The initial file API currently supports text writes; read APIs, wrappers for sockets/locks/GPU objects, and error-unwind cleanup remain open work. No exception unwinding is claimed.

## 16. Owned file-resource transfer — 2026-11-01

File handles returned by `file.open` now carry ownership identity in the semantic symbol table. Initializing another binding from an owned file handle transfers the owner and invalidates the source binding; closing invalidates the current owner. Deferred cleanup checks whether any live owner remains, avoiding false double-release reports after transfer. This ownership tracking is currently limited to file resources; raw pointers and general aggregate ownership are not covered.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 35/35. Added positive ownership-transfer/deferred-close coverage and negative use-after-move coverage.
