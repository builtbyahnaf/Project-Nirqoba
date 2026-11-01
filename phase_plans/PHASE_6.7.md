# Phase 6.7 — Dynamic Memory Allocation and Selectable Allocator Strategies

**Status:** Planned (retrospective plan; the phase is implemented and reported in
[`reports/phase-06.7.md`](../reports/phase-06.7.md))
**Author:** Nirqoba architecture
**Plan version:** 1.0
**Scope sources:** [`project_context/ROADMAP.md`](../project_context/ROADMAP.md) §"Phase 6.7",
[`project_context/SYNTAX.md`](../project_context/SYNTAX.md) §6.7,
[`docs/memory/allocators.md`](../docs/memory/allocators.md)
**Target report:** [`reports/phase-06.7.md`](../reports/phase-06.7.md)

---

## 1. Purpose

Phase 6.5 gave ordinary objects a collector. Phase 6.7 gives the developer a *manual* option
for the cases where a collector is the wrong tool: buffers with known lifetimes, fixed-size
records churned by the million, regions that must be freed in one shot. The goal is that
choosing an allocator is explicit, local, and never changes what unrelated allocations do.

Two invariants drive the whole design:

1. Manual allocation never silently becomes managed memory.
2. Selecting an allocator for one allocation never changes any other allocation.

## 2. Decisions

| # | Decision | Rationale |
|------|----------|-----------|
| 6.7-D1 | One typed entry point, `memory.allocate<T>(bytes)`, with `memory.deallocate(ptr)`. | Matches the roadmap syntax; the element type gives alignment and the debug-time identity of the allocation from the type alone. |
| 6.7-D2 | Alignment is validated against the element type and rejected (not rounded) when impossible. | A silent round-up hides a bug; a diagnostic build should say so. |
| 6.7-D3 | Allocators are **opaque handles** (`allocator`), not objects with methods. | Keeps the handle small, prevents user code from poking at allocator internals, and defers a real `Allocator` interface until interfaces (Phase 8) and function pointers (Phase 13) exist. |
| 6.7-D4 | Every explicit allocation is registered with its owning allocator; frees are checked against that registry. | This is what makes double-free and allocator-mismatch detection possible at all, and what forbids cross-allocator frees. |
| 6.7-D5 | Thread-local *current* allocator plus a per-allocation override. | Gives function/module-level policy without a language feature: set it, `defer` the restore. |
| 6.7-D6 | `memory.reallocate<T>(ptr, bytes)` keeps the owning allocator and recorded alignment, copies surviving bytes, grows the newest arena/stack block in place, and leaves the original valid on failure. | Reallocation must not change ownership or silently drop data; arena/stack growth-in-place is the common case and is cheap. |
| 6.7-D7 | A policy default allocator (`memory.set_default_allocator` / `get_default_allocator`) is the application-wide fallback; handle `0` means "the configured default". | Satisfies "application-wide explicit-allocation policy" without adding a global mutable variable to the language. |
| 6.7-D8 | A default allocator cannot be destroyed while it still serves that role. | Destroying it would leave every later allocation pointing at freed state. |

## 3. Prerequisites

- Phase 6 gate and Phase 6.5 collector (they share the runtime object header and the registry).
- `defer` (Phase 3) for scope-scoped allocator selection and region teardown.
- Compiler support for a *generic call* form `memory.allocate<T>` in the parser (added here).

## 4. Architecture

```
nirqoba source                compiler                     runtime
--------------                --------                     -------
memory.allocate<T>(n)  ──►  TypeChecker: arg/align check ──► nrq_allocator_allocate(handle, size, align)
memory.deallocate(p)   ──►  TypeChecker: ptr type       ──► nrq_allocator_deallocate(p)
memory.reallocate<T>   ──►  Parser generic-call form    ──► nrq_allocator_reallocate(p, size)
memory.use_allocator(h)──►  lowers to TLS set           ──► nrq_allocator_set_current / get_current
```

Registry entry per allocation: `{ptr, size, alignment, allocator_kind, allocator_handle}`.
Free path: look up → validate kind/handle → report unknown/double free or mismatch → hand to
the owning allocator.

| Strategy | Intended workload | Main limitation |
|----------|-------------------|-----------------|
| General | Anything, default | Slower than specialized strategies under churn |
| Pool | Repeated same-size objects | Wastes memory for other sizes |
| Arena | Many objects freed together | No individual free; freed as a unit |
| Stack | LIFO temporaries | Frees must be in reverse order |
| Slab | Fixed-size records | One object size per slab family |
| Buddy | Power-of-two blocks, splitting | Internal fragmentation for odd sizes |
| Free-list | Reusing differently sized free blocks | Search cost grows with fragmentation |
| TLSF | Bounded-latency alloc/free | More metadata per block |

## 5. Implementation steps

1. **Registry.** Allocation record table with validation on free; unknown handle, double free
   and allocator mismatch are distinct diagnostics.
2. **General allocator** (default) with alignment support and allocation-failure reporting.
3. **Specialized strategies.** Pool, arena, stack (LIFO enforced), slab, buddy, free-list,
   TLSF, each with `reset` and `destroy` where meaningful, and per-strategy limitations
   documented in `docs/memory/allocators.md`.
4. **Typed front end.** `memory.allocate<T>`, `memory.secure_allocate<T>`,
   `memory.deallocate`, `memory.reallocate<T>`; parser support for the generic call form;
   type checking for pointer/numeric arguments and alignment; lowering to runtime calls.
5. **Selection.** Per-allocation allocator argument, thread-local current allocator,
   application-wide policy default, and a `defer`-based pattern for function/block scope.
6. **Reallocation.** Keep owner + alignment, copy surviving bytes, grow the newest
   arena/stack block in place, otherwise allocate-copy-free; refuse non-top stack blocks
   (LIFO); free on zero bytes; leave the original valid on failure.
7. **Separation guards.** `memory.deallocate` on a managed pointer and GC on an explicit
   allocation are both rejected; `reallocate` on a managed pointer is a compile-time error.
8. **Diagnostics.** Static use-after-free for direct variable aliases in the type checker;
   dynamic double-free and mismatch detection in the runtime; both documented as
   diagnostics, not as a substitute for ownership enforcement.
9. **Benchmarks and tests.** See §6.

## 6. Testing strategy

| Requirement | Test |
|-------------|------|
| Alloc/dealloc correctness | Unit: round-trip for every strategy; payload intact after write |
| Alignment guarantees | Over-aligned requests return aligned pointers; impossible alignment rejected |
| Allocation failure | Huge request returns failure, does not crash or corrupt the registry |
| Double free / unknown free | Fails with the specific diagnostic |
| Allocator mismatch | Free through a different allocator handle is refused |
| Each strategy | Pool, arena, stack, slab, buddy, free-list, TLSF unit suites incl. `reset` |
| Fragmentation | Occupancy reported through the profiler (`free_bytes`, `largest_free_block`, `fragmentation_bytes`) |
| Repeated alloc/free | Churn loop returns occupancy to baseline |
| Reallocation | Growth preserves data, keeps the owner, grows arena/stack top in place, refuses non-top stack block, zero size frees |
| GC/allocator isolation | Managed pointer cannot be deallocated; explicit allocation is never swept |
| Policy default | `set_default_allocator`, handle `0` means default, destroy-while-default refused |
| Source-level custom allocator | **Blocked** — needs interfaces (Phase 8) and function pointers (Phase 13). Native embedders use the C callback interface. |
| Benchmarks | Small / large / mixed-size / long-running workloads printed, not asserted |

## 7. Acceptance criteria

- Explicit allocation never enters the managed heap; the GC never reclaims an explicit
  allocation.
- Any allocation can override the allocator without changing unrelated allocations.
- All eight strategies are selectable and documented with their intended workloads and
  limitations.
- Invalid free, double free and allocator mismatch are diagnosed.
- Reallocation preserves data and ownership, and leaves the original valid on failure.
- Single documented command reproduces the gate; both presets green.

## 8. Known limitations at close

- No Nirqoba-source `Allocator` declarations (blocked on Phases 8/13); a native C callback
  interface exists for embedders.
- Container/collection-level allocator selection needs the collections library.
- Use-after-free detection covers direct variable aliases only; no interprocedural escape or
  aggregate-hidden alias analysis.
- Partial static pointer lifetime checking; ownership/borrow enforcement is not in place.

## 9. Risks

| Risk | Mitigation |
|------|------------|
| Users assume one strategy is "fastest" | Docs state no universal winner and name each strategy's workload and limits |
| Registry overhead on hot paths | Registry is a hash lookup; benchmarks measure alloc/free cost |
| Capability asymmetry while custom allocators are blocked | Report entry and `docs/memory/allocators.md` say so plainly instead of implying support |
| Reallocation misuse on stack blocks | LIFO rule enforced with an explicit diagnostic, plus a negative integration test |
