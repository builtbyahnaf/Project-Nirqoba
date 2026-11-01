# Phase 6 Upgrade — memory model decision record

Status: architectural decisions are adopted; compiler and runtime enforcement are partial and continue in `reports/phase-06-upgrade-followup.md`.

## Memory domains

1. **Owned** is the default for supported resource-bearing values. One binding owns the allocation; a move transfers that responsibility, and deterministic cleanup releases a still-live owner once.
2. **Borrowed** values do not own storage. Immutable borrows may alias; mutable pointer borrows are exclusive at a call site. A borrow must remain within the owner's lifetime.
3. **Shared** values opt into reference-counted ownership. Reference-count updates are synchronized; sharing does not make the pointed-to data thread-safe.
4. **GC-managed** values opt into the precise tracing heap, support cycles, and are reclaimed by collection. The collector starts with automatic triggers disabled. GC allocations cannot be explicitly released through the owned allocator.
5. **Raw** pointers carry no automatic lifetime or aliasing guarantee. An explicit unsafe boundary is the intended long-term design; the current language has only a restricted low-level subset and does not yet provide unsafe scopes.

## Transfer and destruction

Owned pointer, array, and resource-bearing aggregate arguments transfer to ordinary owning parameters; a borrow must be explicit. Matching owned returns transfer to the caller. Resource-bearing structs, tuples, and fixed arrays move as whole values and recursively clean nested owned arrays. Partial field moves and destructuring are rejected. Dynamic owned arrays reject element types that own nested allocations until recursive element destruction is implemented. Moving an owner makes its source unusable until that binding is reinitialized. Compiler-generated cleanup runs in reverse registration order on scope exit, return, break, and continue. `defer` captures arguments at registration and runs in LIFO order; an explicit deferred cleanup replaces inferred cleanup for its owner. Exception/error-unwind cleanup is undefined because the language has no unwind model.

The compiler/runtime distinguish allocations from the GC heap and the explicit allocator. An owned array may contain GC references only because its backing region is registered as a precise repeated-layout GC root; dropping the array unregisters the root before freeing the explicit buffer. GC does not own or free that buffer. Native resources use deterministic cleanup rather than GC finalization.

## Current implementation boundary

- Owned pointers from supported allocation/resource producers move through bindings and ordinary pointer function parameters/returns. Borrowed pointer parameters and shared pointer parameter/return contracts are checked.
- Files, mutexes, secure buffers, mappings, and owned arrays receive deterministic cleanup. Automatic cleanup applies on supported lexical control-flow exits.
- `owned_array<T>` provides contiguous, checked storage, `.length`, allocator selection, optional stronger alignment, and ownership transfer. Read-only `array_view<T>` supports checked nested ranges, view parameters, and lexical owner-lifetime protection.
- Shared allocation uses explicit retain/release tokens. GC uses precise roots, pins, per-object trace metadata, aligned bump regions, and an open-addressed object index. It remains stop-the-world under a global mutex; root-slot writes synchronize with collection through the runtime API.
- Resource-bearing structs, tuples, and fixed arrays transfer as whole values and recursively drop nested owned arrays. Individual resource field extraction/replacement, shared references in aggregates, closure capture, arbitrary returned references, general escape analysis, and path-sensitive state merging are not implemented. Views cannot be returned or stored in aggregates.
- GC TLABs, safepoints, generational/incremental collection, and concurrent mutator writes are not implemented. The implementation does not claim concurrent-GC safety for uninstrumented writes.
- Allocator operations now use per-instance synchronization and allocation work is performed outside the global registry lock. Ownership registry updates, deallocation, reallocation, shared counts, selection, and profiler snapshots still use global synchronization.
- CPU/GPU tensor abstractions and device-owned buffers do not exist. The runtime is currently Windows x64 only.

## Acceptance mapping

- **A — Design:** this record defines memory domains, transfer, borrows, cleanup order, GC/owned boundaries, and implementation limits.
- **B — Ownership analysis:** partial; supported pointer and array moves, whole-value ownership transfer for resource-bearing aggregates, pointer borrowing, shared contracts, use-after-move checks, and view lifetime checks are enforced. Field-level moves and general escape/lifetime analysis remain open.
- **C — Deterministic cleanup:** partial; tracked owners/resources are dropped on normal lexical exits and `defer` runs LIFO. Error/exception unwinding has no language model.
- **D — Explicit allocators:** partial; all built-in strategies remain, allocator identity checks remain, and lock scope is narrowed. Comparative general-purpose allocator evaluation and controlled contention measurements remain.
- **E — GC:** partial; contiguous regions, precise root validation, array root regions, and the compact object index are implemented. TLABs, safepoints, generational/incremental collection, and concurrent mutation are open.
- **F — Arrays:** partial; contiguous owned arrays, explicit alignment requests, read-only borrowed views, and recursive cleanup for arrays embedded in whole-value aggregates are implemented. Tensors, GPU buffers, mutable views, and field-level aggregate lifetimes remain open.
- **G — Benchmarks:** the C++/Rust runner includes scalar, array, small/large/mixed allocation, graph, and multithread workloads with checksums, warmups, repeated percentiles, and environment metadata. Comparative execution has not been possible in the current environment because the active shell lacks `cl.exe` and `rustc.exe`. GC benchmarking is intentionally deferred.

## IR preservation

TIR values and function parameters retain ownership categories. MLIR carries `nirqoba.ownership` and return ownership attributes so lowering can preserve owned, borrowed, shared, GC-managed, and raw boundaries. Root stores for GC references go through the synchronized runtime API.
