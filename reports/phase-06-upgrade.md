# Phase 6.9 and 6 Upgrade implementation status

## Phase 6.9

The repository already implements optional reference diagnostics, profiler counters and histograms, Windows file mappings with checked typed access, GC pinning with scoped `defer`, secure allocation/zeroization, memory and runtime introspection, and deterministic lexical `defer` cleanup. Existing implementations are described in `docs/memory/management.md`, `runtime/src`, and the earlier sections of this report.

Remaining work: native resource wrapper types and unwind cleanup require a specified error/unwind model; task/device introspection depends on later runtime subsystems; profiler enabled/disabled overhead has not been measured. These are not considered complete by the presence of adjacent APIs.

## Phase 6 Upgrade

The governing decision is ownership-based memory as the language default, with opt-in shared and tracing-GC domains and an unsafe raw boundary. See `project_context/PHASE6_MEMORY_MODEL.md` for the decision record. Existing automatic GC default remains a compatibility behavior until owned values, borrowing, deterministic drop, and migration semantics are implemented together.

Compiler ownership analysis, lifetime checking, ownership preservation through TIR/MLIR, thread-local allocator fast paths, GC TLAB/safepoints, contiguous tensor/view storage, and comparative benchmark results remain incomplete. No C++/Rust competitiveness claim is made.

Status: **FAIL / incomplete**. This report does not claim phase acceptance.

## 17. Move tracking for explicit allocations — 2026-11-01

Extended unique ownership tracking from file resources to `memory.allocate`, `memory.secure_allocate`, `memory.reallocate`, and `memory.map_file` results. Binding an owned allocation to another name transfers the allocation identity and invalidates prior aliases. GC-managed arrays and pins are intentionally excluded because they have distinct lifetime rules. Deferred cleanup now recognizes an allocation as already consumed only when a still-tracked alias exists and all aliases are invalid, avoiding false positives for an allocation whose original name was rebound.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 37/37, including explicit allocation transfer and use-after-move rejection. The analysis remains lexical and pointer-based; aggregate fields, closure capture, interprocedural ownership contracts, and borrowing are not yet implemented.

## 18. Automatic collection default — 2026-11-01

Changed the tracing collector's initial automatic-collection state to disabled, consistent with the Phase 6 Upgrade decision. GC allocations remain supported in their explicit managed domain; programs may call `gc.enable()` to enable automatic triggers, while explicit `gc.collect()` remains available. Updated the superseded Phase 6.5 roadmap wording and memory documentation.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 37/37, including updated initial `gc.is_enabled()` output. This policy change does not implement owned arrays or borrowing and therefore does not complete the ownership default by itself.

## Benchmark harness scope

Phase 6U:G now has a repeatable source and runner matrix for scalar, contiguous-array, small allocation, pointer-rich graph, and multithreaded allocation workloads in C++ and Rust. The runner records warmups, repeated measurements, p50/p95/p99, environment/compiler metadata, and checksums, and isolates artifacts in a temp directory. The current machine has no `rustc.exe`; the runner parsed successfully but the comparative matrix could not be executed. No comparative performance claim or G acceptance pass is made.

## 21. GC object storage co-allocation — 2026-11-01

The collector now stores each aligned object header and payload in one allocation rather than separate header and payload allocations. Precise reference-offset metadata remains in the header, object payload remains zero-initialized, and collection still uses the existing global stop-the-world mutex and pointer index. This removes one allocator call per GC object; it does not add TLABs, generational collection, or concurrent collection.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 38/38, including cycle tracing, pinning, roots, and managed arrays. A five-process smoke sample of the existing 20,000-object benchmark measured 3,117, 3,029, 3,243, 2,988, and 3,055 µs (median 3,055 µs, about 153 ns/object). An earlier single run before the storage change was 4,999 µs, but that is too weak a baseline for a speedup claim. Repeat paired runs with controlled machine load before quantifying the improvement.

## 22. Function-boundary ownership and borrowing — 2026-11-01

Implemented pointer ownership transfer through declarations, pointer parameters, and pointer returns. Added `borrow T* name` and `borrow mut T* name` parameter modifiers. The checker rejects use after move, borrowed pointer escape through returns, deallocation/close of borrows, transfer of borrowed values into owners, immutable sources for mutable borrows, and conflicting call-site aliases. Explicit memory/resource producers are tagged as owning in semantic symbols.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and all 49 integration tests passed. Positive tests cover owned argument/return transfer, borrowed file access, and mutable borrowing; negative tests cover use after move, borrow escape, freeing a borrow, immutable mutable-borrow source, and alias conflicts.

Limitations: ownership and borrow categories are now carried on TIR values/parameters and emitted as `nirqoba.ownership` MLIR attributes. Borrow lifetimes are limited to function parameters; local borrow expressions, aggregate field propagation, closures, loops with path-sensitive state merging, automatic drop insertion, shared ownership, and unsafe scopes are incomplete.

