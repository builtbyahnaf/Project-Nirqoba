# Phase 6.5 Report — Automatic Garbage Collection

## 1. Phase Overview

- **Phase:** 6.5 — Automatic Garbage Collection and Advanced Memory Management
- **Implementation date:** 2026-10-31
- **Status:** Partial implementation
- **Objective:** Provide precise, non-moving tracing collection for managed objects.
- **Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

## 2. Scope

Implemented: Windows runtime mark-and-sweep heap; typed tracing callbacks and
reference offsets; cycles; managed arrays and `managed<T>` references; compiler
root frames; default-enabled automatic triggers; enable, disable, collect,
threshold and statistics APIs.

Not implemented: owned, borrowed and shared language modes; generalized
resource wrappers; asynchronous roots; concurrent collection; or
generational/incremental collection. Explicit pin/unpin is implemented for the
non-moving collector.

## 3. Architecture and Design

The collector is non-moving and stop-the-world under a runtime mutex. Objects
are indexed by address; roots are registered by generated function frames.
Compiler-provided trace metadata identifies managed pointer offsets, so integer
payloads are not treated as references. This supports cycles without
conservative scanning. The lock and explicit frame interface leave room for
later thread and coroutine integration but do not implement it.

## 4. Implementation Details

`array.allocate<T>` creates typed objects in the GC heap. `managed<T>` values
are registered in generated root slots, and reference-bearing aggregates carry
precise field offsets. Explicit `gc.collect()` runs while automatic collection
is disabled. Statistics report allocated and reclaimed bytes, collection
count, and current live objects.

## 5. Modified Files

Runtime implementation and public interfaces are in `runtime/src/gc.cpp` and
`runtime/include/nirqoba/runtime/gc.h`. Type metadata, root emission, managed
allocation lowering, parser/type-checker support, and test fixtures are in the
compiler and `tests/` trees. User behavior is described in
`docs/memory/management.md` and `project_context/SYNTAX.md`.

## 6. Language/API Changes

Implemented `managed<T>`, `array.allocate<T>(count)`, `gc.enable()`,
`gc.disable()`, `gc.collect()`, `gc.set_threshold(bytes)`, and
`gc.get_statistics()`. The statistics fields are `allocated_bytes`,
`collected_bytes`, `collections`, and `live_objects`.

## 7. Testing

- Command: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Configuration RelWithDebInfo`
- Environment: Windows x64, MSVC, LLVM/MLIR 23.1.4.
- Actual result: build succeeded; CTest passed 1/1; the final expanded suite
  passed 24/24 integration tests after adding lexical deferred cleanup, scoped
  allocator restoration, and scoped pin release.
- GC integration cases cover controls, allocation, managed arrays and a cyclic
  graph surviving explicit collection.
- Multithreaded and async/coroutine GC tests were not run because those language
  execution features are not supported yet.

## 8. Performance

No benchmarks were run. Collection uses a global mutex and is non-moving;
performance overhead has not been characterized.

## 9. Known Issues

Thread-safe heap metadata does not imply complete mutator coordination across
threads. Async roots, finalization, native resource wrappers, unwind cleanup,
and non-GC ownership modes remain incomplete. `defer` supports explicit
function-call cleanup on lexical exits. The implementation is Windows only.

## 10. Documentation Changes

Updated `project_context/SYNTAX.md`; added `docs/memory/management.md`.

## 11. Acceptance Criteria

| Requirement | Status |
|---|---|
| Precise tracing, cycles, unreachable reclamation | Implemented; covered by tests |
| Default-on automatic GC and manual controls | Implemented; covered by tests |
| Threshold and statistics | Implemented; covered by integration tests |
| Async roots and concurrent collection | Incomplete |
| Deferred call cleanup on lexical block exits | Implemented; integration tested |
| Explicit and scoped pin/unpin for non-moving managed objects | Implemented; unit and integration tested using `defer` |
| Performance benchmark | Not run |

## 12. Final Verdict

**FAIL** — core tracing behavior builds and passes the current suite, but Phase
6.5 acceptance includes resource lifecycle, thread/async cases and broader
validation that are not implemented.

## 13. Completion pass — 2026-11-01

Added since the first pass:

- `gc.is_enabled()` reports whether automatic collection is active, so a
  program can observe the effect of `gc.disable()` without collecting.
- Bounded pin diagnostics: `memory.pin_count()` reports distinct pinned
  objects, and a repeated pin of one object beyond 16 is rejected (returns a
  null pointer) and counted by `memory.pin_violations()`. A forgotten `unpin`
  is therefore observable instead of keeping the object reachable forever.
- Benchmarks required by the phase are now executed by the suite:
  `GcTest.BenchmarksAllocationAndCollectionOverhead` measures 20,000
  allocations and one sweep and prints microseconds and nanoseconds per object.
  Timings are recorded for regression visibility; no relative performance claim
  is made.
- `GcTest.AutomaticCollectionStateIsObservable` and
  `GcTest.PinDiagnosticsReportCountAndRejectExcessivePins` cover the new
  behaviour; the existing concurrent-allocation test continues to cover
  serialized multithreaded allocation.

Still blocked or not implemented:

- Owned, borrowed, and shared memory modes. The roadmap requires their syntax to
  be established in the language specification before implementation, and
  `project_context/SYNTAX.md` defines none yet. Inventing that syntax here would
  violate the AGENTS.md rule against unapproved design decisions.
- Precise stack/register roots for coroutines and asynchronous execution, which
  depend on Phases 10–11.
- Concurrent, incremental, and generational collection remain architectural
  provisions only; the collector is a stop-the-world non-moving mark-and-sweep
  under a single mutex.
