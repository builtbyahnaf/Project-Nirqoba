# Phase 6 Upgrade — memory model decision record

Status: design baseline established; enforcement and runtime migration are in progress.

## Memory domains

1. **Owned** is the intended default for newly specified resource-bearing values. A value has one owner; assignment transfers ownership; leaving its owning scope destroys it exactly once.
2. **Borrowed** values do not own storage. Immutable borrows may alias; a mutable borrow is exclusive. A borrow cannot outlive the storage it references or escape a function unless its lifetime is tied to an input by a future explicit rule.
3. **Shared** values opt into reference-counted shared ownership. The count's thread-safety must be explicit; sharing does not imply mutable thread safety.
4. **GC-managed** values opt into the tracing heap, support cycles, and are reclaimed by tracing collection. They cannot also be owned or explicitly freed.
5. **Raw/unsafe** pointers carry no automatic lifetime or aliasing guarantee. Operations requiring unchecked lifetime, layout, or aliasing assumptions belong in an explicitly marked unsafe boundary.

## Transfer and destruction

Owned arguments transfer into parameters by default; borrowing must be explicit in the eventual syntax. Returning an owned value transfers it to the caller. Aggregate fields and array elements recursively own their owned members. A move leaves the source unusable until reinitialized. Destruction is exactly once and reverse construction order (LIFO), on normal block exit and every supported control-flow exit. `defer` remains available for explicit cleanup and runs in LIFO order. Error unwinding semantics must be specified before exceptions can be introduced.

GC and owned values form disjoint allocation domains. A GC reference may be held by an owned wrapper only if the wrapper is a non-owning handle with documented tracing/root behavior; freeing or moving the GC allocation through the owned allocator is invalid. Raw pointers require an explicit unsafe boundary to cross domains. Native resources use deterministic owned wrappers, never GC finalizers as the correctness mechanism.

## Current implementation boundary

The current language has no ownership/borrow syntax or compiler lifetime model. Existing pointers and `defer` retain their existing semantics. The runtime currently initializes automatic GC as enabled for compatibility; changing that flag alone would not implement owned-by-default semantics and is therefore not treated as completion. The next implementation work must add ownership metadata to semantic values and TIR, diagnose moves/borrows, then lower drops across all exits before changing defaults.

Arrays/tensors target contiguous owned storage, with borrowed views carrying validated lifetimes. GPU buffers are explicit device-owned resources with transfer/synchronization boundaries. Numerical storage must not allocate one GC object per scalar.

## Acceptance mapping

- A: this document defines domains and transfer/drop boundaries; concrete syntax is intentionally gated on compiler representation support.
- B/C: not yet enforced; existing use-after-free checks and `defer` are narrower mechanisms.
- D/E: allocator and collector performance work is not claimed complete without controlled measurements and synchronization design.
- F: target constraints are defined above; current tensor subsystem does not exist.
- G: benchmark harnesses and actual baseline numbers are required before any performance claim.
