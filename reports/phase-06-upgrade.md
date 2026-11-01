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


## 23. Explicit shared allocation domain â€” 2026-11-01

Added `memory.shared_allocate<T>(bytes)`, `memory.shared_retain(pointer)`, `memory.shared_release(pointer)`, and `memory.shared_count(pointer)`. Shared allocations are tracked separately from unique owners, reference updates are protected by the allocator synchronization domain, and the final release frees the allocation using its original allocator. Unique deallocation, secure release, and resizing reject shared pointers. Semantic analysis tracks shared handles independently and prevents using unique-owner release APIs for them; TIR/MLIR marks shared values explicitly.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 51/51. Positive coverage checks count transitions across retain and release; negative coverage rejects releasing a unique allocation through the shared API.

Limitations: shared handles are currently scoped to local pointer bindings. Shared function parameter/return contracts, aggregate propagation, automatic drops, and synchronization of the pointed-to data are not implemented. Reference counting itself is synchronized; pointee mutation is not.

## 24. Shared ownership at function parameters â€” 2026-11-01

Added the `shared T* name` parameter qualifier. A shared reference can be transferred only into a parameter declared shared; passing it to an ordinary unique-owner parameter is rejected. The callee receives one owned reference-count token and may retain, transfer, or release it. TIR function arguments carry shared ownership metadata into MLIR.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 52/52. Coverage includes a reference transferred to a shared parameter and a negative shared-to-unique call.

Limitations: shared function return types are not expressible yet; shared references also cannot be stored in aggregates. These remain part of ownership modeling.

## 25. Shared ownership at function returns â€” 2026-11-01

Added shared pointer return contracts using `shared T* function name(...)`. The checker verifies that return expressions match the declared domain and transfers the shared token to the caller. Function signatures carry the shared return contract into TIR and MLIR as `nirqoba.return_ownership = "shared"`; callers recover shared state when binding the result.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 53/53. Coverage includes shared factory return and consumption, invalid unique return of a shared value, and MLIR return ownership metadata.

Limitations: shared aggregates and compiler-inserted automatic release remain unimplemented.

## 26. Reject raw pointer ownership escalation â€” 2026-11-01

The ownership checker now rejects raw or untracked pointers passed to unique-owner parameters and returned as owned pointers. Borrowed arguments require an explicit borrow parameter; raw/borrowed pointers cannot escape through an owning return contract. This closes a path where the callee could free storage while the caller retained an untracked alias.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 55/55, including new negative cases for raw pointer transfer and return.

## 27. Automatic cleanup for tracked owners â€” 2026-11-01

Added compiler-generated LIFO cleanup for recognized owning pointer/resource bindings at lexical scope exit. Cleanup also runs on return, break, and continue through the existing cleanup lowering. Owned pointer parameters default to `memory.deallocate`; shared parameters default to `memory.shared_release`. The checker carries cleanup identity across local moves and assignments, cancels the source cleanup on transfer, tracks reallocation as consuming the old owner, and rejects transferring resources whose destructor does not match the generic owner contract. An explicit `defer` for a named owner replaces its inferred cleanup.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 57/57. New runtime cases verify local owned/shared cleanup and parameter cleanup; existing defer, file, pin, move, and reallocation cases remain green.

GC correctness hardening: allocation, live-object, collection, and collected-byte counters now saturate instead of wrapping, including the automatic-allocation byte accumulator. This is accounting correctness work; no GC benchmark or speed claim is included.

Limitations: aggregate/partial moves, closure capture, raw unsafe scopes, exception unwinding, and function return contracts for specialized resources still need type-system representation. GC remains stop-the-world and globally synchronized.

## 28. Path-safe owner transfer lowering â€” 2026-11-01

Owner transfers now load the current value and clear the source slot in TIR, while generated cleanup remains attached to the slot and safely receives null after a move. This makes implicit drop correct across conditional move paths: if a branch is skipped, the original owner is still released; if it runs, cleanup sees null. Deferred explicit cleanup clears the same slot after running, so the inferred cleanup remains safe. Assignments attach cleanup to the binding's lexical owner scope, and ownership-domain changes on an existing owner binding are rejected.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 57/57. The owner cleanup integration case exercises both taken and skipped conditional release branches, manual release, early return, scope exit, and shared cleanup.

## 29. GC allocation and root failure safety â€” 2026-11-01

GC root registration now fails fast when the root descriptor is invalid, conflicts with an existing slot layout, or cannot be recorded due to metadata allocation failure. Previously those cases silently left a live managed pointer unrooted, allowing later collection to reclaim it. The object index insertion also checks duplicate-address failures before linking the new object into the heap.

Validation: Added a unit case for oversized allocation rejection with unchanged GC statistics. RelWithDebInfo build succeeded; CTest passed 1/1 and integration tests passed 57/57. No GC benchmark was run.

## 30. Owned native mutex resource â€” 2026-11-01

Added the Windows native mutex wrapper as a first non-file resource integration. `mutex.create()` produces an owned handle, generated cleanup calls `mutex.destroy`, transfers clear the old binding, and borrow parameters remain the path for non-owning helper access. A deferred unlock executes before automatic destroy due to LIFO cleanup ordering.

Validation: RelWithDebInfo build succeeded; CTest passed 1/1 with concurrent counter serialization, and integration tests passed 59/59. No GC benchmark was run.
