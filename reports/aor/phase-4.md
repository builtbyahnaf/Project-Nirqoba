# AOR Phase 4 — adaptive object representations

**Status:** complete  
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)  
**Scope:** replace the H32 object header with H0 (headerless), H8 (compact), and
H16 (extended); make H16 the absolute maximum.

## Result

No standard GC-managed object retains a header larger than 16 bytes. A page names
the representation, so no object carries a size tag:

| Representation | Header | Used for |
|---|---|---|
| H0 headerless | 0 B | fixed record, payload ≤ 256 B |
| H8 compact | 8 B | larger fixed record that still fits a page |
| H16 extended | 16 B | arrays, SoA containers, large and fallback objects |

Allocation chooses by shape and size; `nrq_gc_allocate_repr` lets a caller state a
requirement and rejects one that cannot be honoured. The layouts, the exact
selection rule, and the page metadata cost are documented in
`docs/memory-model/aor-headers.md` and `docs/memory-model/aor-eligibility.md`.

## What had to change

The H32 header was not just a size; it was where six different pieces of state
lived. Each moved to the smallest place that could still answer its read path:

| Field | Phase 4 replacement |
|---|---|
| `layout` (8 B) | page `uniform_layout` (H0) or 32-bit `layout_id` (H8/H16) |
| `element_count` (4 B) | H16 only; implicit one on H0/H8 |
| `state` (4 B) | H8/H16 word; one byte per object on an H0 page |
| `birth_epoch` (8 B) | page born bitmap + lazy `born_epoch` stamp |
| `next` (8 B, unindexed pages) | `std::vector<ObjectRef>` sweep list |
| header padding | removed; H16 is exactly four words |

Two consequences drove most of the work:

1. **Discovery changed.** The object-start bitmap now indexes span starts. A
   payload no longer equals its start bit, so every path that derived an object
   from a payload had to subtract the header of the page it is on.
2. **Reuse became representation-local.** A free run on a compact page cannot
   become a headerless object even when the span matches, because the page kind
   decides how the object is decoded.

## Bugs found and fixed

Each was found by the existing suite or by a new test, and each has a regression
test or an assertion that would catch it again.

1. **Recycled pages kept `use_object_index`.** A page that had once been a
   directory fallback stayed marked unwalkable after recycling, so the sweep
   skipped it and its live objects were never reclaimed. The flag is now cleared
   with the rest of the page metadata.
2. **Headerless pages had no free-run reuse.** Every headerless allocation that
   missed the current slab carved fresh page space, even when the page had just
   reclaimed the same slot. Reuse now consults runs with the page's layout and
   slot size as part of the match.
3. **Headerless refills did not count as refills.** `nrq_gc_tlab_refill_count`
   ignored them, so the fragmentation regression could not see the cost it was
   written to measure.
4. **Unindexed objects were swept through a stale intrusive list.** Large and
   fallback objects moved to a vector, and the cycle-end sweep now walks that
   vector instead of a `next` chain that no longer exists.
5. **A one-element container was described as a record.** The descriptor inferred
   "array" from `bytes != element_size`, so `owned_soa<T>(1)` became headerless
   and the compiler's `payload - 16` count read returned page metadata. The shape
   is now stated by the entry point and is part of the layout memo key.
6. **Mixed-shape threads thrashed headerless slabs.** A second small shape evicted
   a half-full slab on every alternating allocation. The second shape now falls
   back to H8 while the slab is active.
7. **Fragmented headerless pages refilled one run at a time.** The four-object
   run pattern of a churned page put the allocator lock back on the allocation
   path. Headerless refills now reserve a batch of runs, like the other
   representations.
8. **The headerless state table was indexed per 8-byte slot.** A 64-byte record
   paid 8 bytes of page metadata — exactly the H8 header the representation was
   removing. It is now one byte per object.

## Invariants preserved

| Invariant | Evidence |
|---|---|
| Precise collection | mixed-representation tracing tests; full suite |
| SATB | existing write-barrier tests unchanged and passing |
| Generational remembered sets | remembered bitmaps still page-local; minor-cycle tests |
| Deterministic destruction | ownership teardown tests |
| TLAB range publication | ranges walk span starts by page kind |
| External mark worklist | ring + cycle-reused vector unchanged |
| Large objects | exact index, H16, per-object sweep |
| Arrays and SoA | always H16; `payload - 16` count read unchanged |
| Pinning | pin counts and transfer tests |
| Multithreaded collection | concurrent collection tests |
| Production/debug equivalence | Phase 9 matrix and `*gc*` equivalence |

## Verification

| Check | Result |
|---|---|
| Unit tests | 349 / 349 passed |
| Integration cases | 624 / 624 passed |
| Phase 7 isolation | passed |
| Phase 9 optimization matrix (O0/O2 × production/debug) | passed |
| `*gc*` configuration equivalence | 67 agreed, 0 skipped, 0 failed |
| Benchmark | measured; see `phase-4-performance.md` |

## Limitations

- The benchmark runs the development runtime, not the production one. The
  production path is covered by equivalence checks, not by those timings.
- Headerless objects are capped at a 256-byte payload. Raising the cap would make
  a dedicated page per shape more expensive than the header it removes.
- A second small shape on a thread is served as H8 while a slab is active. This
  is a policy choice, not a correctness limit.
- Large and fallback objects are always H16; there is no headerless large object.
- The H16 `flags` word is reserved and written as zero. Phase 5 will use it.

## Roadmap status

Phase 4 is complete. Phase 5 can make the representation choice in the compiler:
selection is a pure function of payload size and the array flag, and the H16
`flags` word exists for the hint it will carry.
