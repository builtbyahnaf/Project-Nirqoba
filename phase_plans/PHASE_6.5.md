# Phase 6.5 — Automatic Garbage Collection and Advanced Memory Management

**Status:** Planned (retrospective plan; the phase is implemented and reported in
[`reports/phase-06.5.md`](../reports/phase-06.5.md))
**Author:** Nirqoba architecture
**Plan version:** 1.0
**Scope sources:** [`project_context/ROADMAP.md`](../project_context/ROADMAP.md) §"Phase 6.5",
[`project_context/SYNTAX.md`](../project_context/SYNTAX.md) §6.5,
[`docs/memory/management.md`](../docs/memory/management.md)
**Target report:** [`reports/phase-06.5.md`](../reports/phase-06.5.md)

---

## 1. Purpose

Phase 6.5 makes collection the *default* memory behaviour for ordinary dynamic objects, and
makes the collector precise rather than conservative. Everything allocated as a managed
object must be reclaimable without the programmer calling a free function, including cyclic
graphs, and the collector must never guess: it must know exactly which slots hold references.

The phase also fixes the boundary between "managed" and "not managed", because later phases
(6.7 allocators, 6.9 utilities) depend on that boundary being explicit.

## 2. Decisions

| # | Decision | Rationale |
|------|----------|-----------|
| 6.5-D1 | Non-moving mark-and-sweep, precise roots. | Moving requires relocation support in the compiler and pinning everywhere in the runtime; not justified before the type system is stable. Precise roots are required by the roadmap. |
| 6.5-D2 | Roots come from **compiler-emitted root frames** describing live managed reference offsets, not from scanning raw stack memory. | Conservative stack scanning would pin garbage and could resurrect stale values. It is explicitly rejected as the default strategy. |
| 6.5-D3 | Managed objects live on a **separate heap** from explicit allocations. | Keeps GC/deallocator confusion impossible: a GC pointer can never reach the explicit registry, and vice versa. |
| 6.5-D4 | `gc.disable()` stops *automatic* collection only. Explicit `gc.collect()` and re-enabling remain available; allocation is refused once the heap cannot grow. | The roadmap requires that disabling never disables safety. A silent out-of-memory writing through a null pointer would be the alternative. |
| 6.5-D5 | Collection is triggered by a **byte threshold**, not an object count. | Bytes track actual pressure and are what a user can reason about when calling `gc.set_threshold`. |
| 6.5-D6 | Pinning is explicit (`gc.pin` / `gc.unpin`), scoped release via `defer`, and capped at 16 pins per object. | Non-moving collection makes pinning mostly a "do not reclaim" hint; the cap turns a forgotten unpin into a visible diagnostic instead of a permanent leak. |
| 6.5-D7 | `owned` / `borrowed` / `shared` modes are **blocked pending specification**. | 6.5.3 states the exact syntax must be established in the language specification first; `SYNTAX.md` defines none. `AGENTS.md` forbids inventing major design decisions in implementation. |

## 3. Prerequisites

- Phase 6 regression gate (`tests/run_tests.py`) so every collector change is measurable.
- Typed explicit allocation and the allocator registry (they share the runtime object header).
- Compiler support for emitting root frames at function entry/exit and at call sites.

## 4. Architecture

```
compiler                              runtime
--------                              -------
managed<T> reference type  ────────►  nrq_gc_alloc(type, trace, size)
root frame (offsets + count) ──────►  nrq_gc_enter_frame / nrq_gc_leave_frame
gc.* builtins              ────────►  nrq_gc_enable/disable/collect/set_threshold/...
                                      heap: header {mark, trace, size, pin_count, next}
                                      collection: mark(trace, roots) then sweep
```

Object header carries the type descriptor (for tracing) and the pin count. Tracing walks
typed reference offsets supplied by the compiler, so a struct with mixed scalar and reference
fields is traced exactly.

## 5. Implementation steps

1. **Heap.** Segregated managed heap with a header per object, allocation statistics, and a
   freelist on sweep. Refuse allocation with a diagnostic when growth is impossible and
   automatic collection is disabled.
2. **Root frames.** Compiler emits a frame description for every function activation listing
   the managed-reference slots; enter on function entry, leave on every return path.
3. **Mark phase.** Worklist-based marking over type descriptors; handle cycles by marking
   before descending.
4. **Sweep phase.** Unmarked, unpinned objects are returned to the freelist; pinned objects
   survive and reset their mark bit.
5. **Triggers.** Default-on automatic collection at a byte threshold; `gc.set_threshold`
   lowers/raises it; `gc.collect` forces a full cycle; `gc.disable`/`gc.enable` toggle
   automatic triggering only.
6. **Statistics.** `gc.get_statistics()` exposes allocated bytes, live objects, collections,
   last-collection duration and the current threshold, in a struct the front end can name.
7. **Pinning.** `gc.pin` / `gc.unpin` with a documented per-object cap, `gc.is_pinned`, and
   diagnostics for over-pinning.
8. **Deterministic cleanup.** `defer`-based LIFO cleanup for external resources, so file,
   socket and handle release never depends on a collection happening. Finalizers are not
   offered as a substitute.
9. **Collector-compatibility seams.** Write-barrier hook, allocation hook, and a
   thread-local allocation buffer entry point, so a moving/generational collector can be
   added later without changing the language surface.
10. **Tests and benchmarks.** See §6.

## 6. Testing strategy

| Requirement | Test |
|-------------|------|
| Reclaim unreachable, preserve reachable | `gc_collect_*` unit tests: allocate → drop root → collect → live count drops |
| Cycles | Two-object cycle with no external root is collected |
| Root tracking | Reference held in a local across a call survives a collection |
| Automatic and manual | Threshold-driven collection observed through statistics; `gc.collect()` forces one |
| Disabled automatic collection | Allocation succeeds after disable; explicit collect still reclaims |
| Thresholds | `gc.set_threshold` changes when automatic collection fires |
| Repeated allocation/collection | Loop of N allocate/collect cycles; live count returns to baseline |
| Pinned objects | Pinned object survives; unpin then collect reclaims it |
| External resources | `defer` closes a handle with collection disabled |
| Overhead | Benchmark: allocation rate and collection pause printed, not asserted |

Multithreaded allocation and async/coroutine roots are *not* tested because threads and
coroutines do not exist in the language yet; the seams are in place and recorded as blockers.

## 7. Acceptance criteria

- Ordinary dynamic allocation is managed by default; no source-level free is needed.
- Collection is precise: no conservative stack scan anywhere in the collector.
- Cyclic garbage is reclaimed.
- Disabling automatic collection never causes a use-after-free or a crash; it only defers
  reclamation.
- `gc.*` API matches `SYNTAX.md` §6.5 exactly.
- Unit + integration suites green in both presets.

## 8. Known limitations at close

- No generational, incremental or concurrent collection (seams only).
- No moving/compacting collection; no relocation support.
- No asynchronous or coroutine roots because the language has no async surface yet.
- Owned/borrowed/shared modes blocked on the specification (6.5-D7).
- No native resource wrapper types; `defer` is the mechanism.

## 9. Risks

| Risk | Mitigation |
|------|------------|
| A missed root causes a live object to be swept (memory corruption) | Root frames are compiler-generated and unit-tested; keep a stress test that allocates under pressure with references held in parameters, locals and aggregates |
| Collector pause grows with heap | Byte-threshold triggering plus statistics; benchmark pauses in Phase 6.9 |
| Users treat `gc.disable()` as "no memory management" | Docs state explicitly that reclamation is deferred, not disabled |
| Pinned objects leak silently | Pin cap + `gc.pin_count()` / `gc.pin_violations()` diagnostics (added in 6.9) |
