# Phase 6 Upgrade — memory model decision record

Status: architectural decisions are adopted. Sections A–F are implemented for the supported language subset; benchmark comparison and tensor/GPU runtime work remain explicitly gated. Evidence and open items continue in `reports/phase-06-upgrade-followup.md`.

## Memory domains

1. **Owned** is the default for supported resource-bearing values. One binding owns the allocation; a move transfers that responsibility, and deterministic cleanup releases a still-live owner once.
2. **Borrowed** values do not own storage. Immutable borrows may alias; mutable pointer borrows are exclusive at a call site. A borrow must remain within the owner's lifetime.
3. **Shared** values opt into reference-counted ownership. Reference-count updates are synchronized; sharing does not make the pointed-to data thread-safe.
4. **GC-managed** values opt into the precise tracing heap, support cycles, and are reclaimed by collection. The collector starts with automatic triggers disabled. GC allocations cannot be explicitly released through the owned allocator.
5. **Raw** pointers carry no automatic lifetime or aliasing guarantee. An explicit unsafe boundary is the intended long-term design; the current language has only a restricted low-level subset and does not yet provide unsafe scopes.

## Transfer and destruction

Owned pointer, array, and resource-bearing aggregate arguments transfer to ordinary owning parameters; a borrow must be explicit. A resource owner parameter can preserve its destructor with `owned file byte*`, `owned socket byte*`, `owned mutex byte*`, `owned mapping byte*`, or `owned secure byte*`; call sites must pass the matching resource. Matching owned returns transfer to the caller. Resource factories preserve their cleanup contract with the same `owned <resource> byte*` return prefixes; returning a resource through an unqualified pointer return is rejected. Resource-bearing structs, tuples, and fixed arrays move as whole values and recursively clean nested owned arrays. Owned-array fields can be moved individually; lowering clears the moved source field so aggregate cleanup drops only fields that remain live. Reinitializing moved fields and moving raw/native-resource pointer fields are not yet supported. Destructuring remains separate work. Dynamic owned arrays reject element types that own nested allocations until recursive element destruction is implemented. Moving an owner makes its source unusable until that binding is reinitialized. Compiler-generated cleanup runs in reverse registration order on scope exit, return, break, and continue. `defer` captures arguments at registration and runs in LIFO order; an explicit deferred cleanup replaces inferred cleanup for its owner. Exception/error-unwind cleanup is undefined because the language has no unwind model.

The compiler/runtime distinguish allocations from the GC heap and the explicit allocator. An owned array may contain GC references only because its backing region is registered as a precise repeated-layout GC root; managed-reference slots are cleared before registration, and dropping the array unregisters the root before freeing the explicit buffer. GC does not own or free that buffer. Native resources use deterministic cleanup rather than GC finalization.

## Current implementation boundary
Borrowed pointer returns name their source parameter explicitly:
`borrow source byte* function alias(borrow byte* source)`. The checker requires
all return paths to derive from that borrowed parameter. Callers inherit the
source allocation identity, so they cannot free, move, or close it while the
returned borrow remains live. `borrow mut source` requires a mutable borrowed
parameter and a mutable local binding; an immutable returned borrow cannot be
upgraded to mutable. Borrowed aliases never receive inferred destruction.

Owned, shared, and borrowed raw pointers cannot be embedded in aggregates because fields do not declare an ownership contract; GC-managed and raw pointers remain non-owning.

- Owned pointers from supported allocation/resource producers move through bindings and ordinary pointer function parameters/returns. Borrowed pointer parameters, resource-qualified file/socket/mutex/mapping borrows, file/socket/mutex/mapping/secure owning parameters and returns, and shared pointer parameter/return contracts are checked.
- Files, mutexes, secure buffers, mappings, and owned arrays receive deterministic cleanup. Automatic cleanup applies on supported lexical control-flow exits.
- `owned_array<T>` provides contiguous, checked storage, `.length`, allocator selection (including a per-construction handle), optional stronger alignment, and ownership transfer. `array_view<T>` supports checked immutable views; mutable views from `memory.array_view_mut<T>` allow indexed writes only through an exclusive `var` binding. Both view forms support nested ranges, view parameters, and lexical owner-lifetime protection.
- Shared allocation uses explicit retain/release tokens. GC uses precise roots, pins, per-object trace metadata with inline offsets for common one- and two-reference layouts, aligned bump regions, and an open-addressed object index. Managed-array allocation publishes into a registered root under the collector lock, and returned managed references use balanced transit pins until the caller roots them. The collector is generational and incremental: young survivors are promoted, minor cycles use a remembered set, and marking/sweeping run in bounded slices with mutators resumed. Mutators park only at cycle boundaries to publish thread-local allocations and prepare roots; root-slot writes and managed-field stores synchronize with collection through the runtime write barriers.
- Resource-bearing structs, tuples, and fixed arrays transfer as whole values and recursively drop nested owned arrays. Owned-array fields support individual moves with branch-sensitive use-after-move checking and source-slot clearing. Reinitializing moved fields, native-resource pointer field moves, shared references in aggregates, closure capture, returned borrows, and general escape analysis remain open. Borrowed pointers require an explicit source-parameter return contract and views are rejected on return; ownership state is merged across `if` branches and loop exits, including individual `break` and `continue` paths. Deferred cleanup for scopes exited by loop control is reflected in those snapshots, and `for` steps are analyzed on continuing paths. Loop back-edges are checked conservatively. Views cannot be returned or stored in aggregates.
- GC TLABs, cooperative mutator safepoints, two generations with promotion, a remembered set, incremental marking, and sliced sweeping are implemented. The collector is not parallel or fully concurrent: heap metadata and each sweep slice remain serialized by the collector mutex, and the implementation does not claim concurrent-GC safety for uninstrumented direct writes. Native managed-reference stores must use the runtime barriers.
- Allocator operations use per-instance synchronization. Allocation, checked deallocation, reallocation, secure release, final shared release, and allocator free-space queries perform allocator work outside the global registry lock while pinning the allocator and marking pointer owners in flight where applicable. Registry updates, shared counts, selection, and profiler snapshots remain globally synchronized.
- CPU/GPU tensor abstractions and device-owned buffers do not exist yet; their ownership model is defined below so future tensor storage remains contiguous and outside the per-scalar GC object model. The runtime is currently Windows x64 only.

## Acceptance mapping

- **A — Design:** this record defines memory domains, transfer, borrows, cleanup order, GC/owned boundaries, and implementation limits.
- **B — Ownership analysis:** implemented for the supported subset; pointer and array moves, whole-value ownership transfer for resource-bearing aggregates, resource-qualified file/socket/mutex/mapping borrows, shared contracts, use-after-move checks, view lifetime checks, branch merging, and loop break/continue exit merging are enforced. Native-resource field moves and fully general escape/alias inference remain future work.
- **C — Deterministic cleanup:** implemented for normal lexical exits, `return`, `break`, and `continue`; tracked owners/resources are dropped in reverse registration order and `defer` runs LIFO. The error-unwinding contract is defined below but awaits a language-level error model.
- **D — Explicit allocators:** implemented for the supported runtime surface; all built-in strategies, allocator identity checks, per-instance operation locking, per-allocation handle selection, and narrowed global lock scope are present. Thread-local allocator caches and comparative allocator measurements remain performance work under 6U:G.
- **E — GC:** implemented for the supported incremental model; contiguous regions, TLABs, cooperative safepoints, precise root validation, array root regions, the compact object index, generational promotion, remembered sets, write barriers, incremental marking, and sliced sweeping are present. Parallel marking/sweeping, compaction, fully concurrent heap metadata, and safety for uninstrumented native direct writes remain future work.
- **F — Arrays:** implemented for contiguous owned arrays, explicit alignment, immutable and mutable views, allocator-aware construction, and recursive cleanup for arrays embedded in whole-value aggregates. Tensor and GPU buffer runtime types are future phase work; their ownership design is recorded below.
- **G — Benchmarks:** the C++/Rust runner includes scalar, array, small/large/mixed allocation, graph, and multithread workloads with checksums, warmups, repeated percentiles, and environment metadata. Comparative execution has not been possible in the current environment because the active shell lacks `cl.exe` and `rustc.exe`. GC benchmarking is intentionally deferred.

## IR preservation

TIR values and function parameters retain ownership categories. MLIR carries `nirqoba.ownership` and return ownership attributes so lowering can preserve owned, borrowed, shared, GC-managed, and raw boundaries. Root stores for GC references go through the synchronized runtime API.

## Error-path cleanup contract

The language still has no expression-level error value or unwind mechanism, so no
currently supported operation transfers control to an error handler. If one is
added, cleanup must be emitted by the compiler rather than by a C++ runtime
unwinder: each active scope contributes its registered drops in reverse order,
`defer` actions run between ordinary owner drops according to their registration
order, and a caught error resumes in the handler only after every scope exited by
the unwind has run its cleanup. A GC root frame must be closed on the same edge.
Native resources (files, sockets, mutexes, mappings) follow the same rule as
owned arrays; no resource may rely on GC finalization. This contract must be
implemented together with the error-handling feature, not by a partial handler
that skips cleanup.

## Tensor and device-buffer ownership design

A tensor is a single owned descriptor over one contiguous element buffer plus
shape and stride metadata. It is not an array of individually GC-managed scalar
objects. A tensor view is a non-owning descriptor with the same lifetime rules as
an array view: it cannot outlive its source and a mutable view is exclusive.
Allocator selection follows the owned-array rule, and alignment is part of the
allocation request so SIMD loads do not require copies. A GPU buffer is a
distinct owned resource domain with explicit host-to-device and device-to-host
transfer operations; it is never collected by the CPU tracing heap. Moving a
CPU or GPU tensor transfers the single buffer owner, and dropping it releases the
backing allocation exactly once. No tensor or device runtime is implemented in
this upgrade; these are the invariants the future implementation must preserve.
