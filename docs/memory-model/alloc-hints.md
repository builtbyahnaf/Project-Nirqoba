# Compiler allocation hints

Phase 6 lets the compiler tell the allocator what it already knows about an
allocation site, and lets the allocator keep the final say.

Two things travel from compiler to runtime:

- a **hint**, which restates the facts the compiler derived from the type (payload
  size, element count, reference count, lifetime) — `NrqAllocHint`;
- a **plan**, which is the representation decision those facts imply —
  `NrqAllocPlan`.

A plan is a value. `nrq_alloc_plan` computes it from a hint, and
`nrq_alloc_plan_for_layout` computes it from a descriptor the runtime interned, so
both sides can be compared without allocating anything.

## The plan

| Field | Meaning |
|---|---|
| `representation` | `NONE` (deterministic storage), `HEADERLESS`, `COMPACT`, `EXTENDED` |
| `header_bytes` | 0, 8, or 16 |
| `payload_bytes` | bytes of one record, or of one array element |
| `total_bytes` | `payload_bytes * element_count` |
| `slot_bytes` | payload rounded up to the plan's alignment |
| `flags` | see below |

| Flag | Meaning |
|---|---|
| `NRQ_ALLOC_PLAN_ARRAY` | the site allocates an array |
| `NRQ_ALLOC_PLAN_REPEATED` | more than one instance |
| `NRQ_ALLOC_PLAN_LARGE` | a single object is larger than a 64 KiB page |
| `NRQ_ALLOC_PLAN_HAS_REFERENCES` | the payload owns managed references |
| `NRQ_ALLOC_PLAN_DETERMINISTIC` | an arena or pool object: no header, no tracing |
| `NRQ_ALLOC_PLAN_REJECTED` | the hint cannot describe any allocation |

The representation occupies two bits at a fixed shift
(`NRQ_ALLOC_PLAN_REPR_SHIFT`), so `nrq_alloc_plan_flags` packs a whole plan into
one 32-bit word. That is what a compiler emits at a call site.

The policy the planner encodes is the Phase 4 policy, unchanged: an array or an
object larger than a page is extended (H16), a fixed record up to 256 bytes is
headerless (H0), anything in between is compact (H8). A rejected hint — zero
payload, a non-power-of-two alignment — produces `NONE` plus the rejected flag
rather than an exception or a partial plan.

## The planned allocation entry point

```c
void* nrq_gc_allocate_array_planned(void** root_slot, size_t count,
                                    size_t element_size,
                                    const size_t* reference_offsets,
                                    size_t reference_count,
                                    void** site_cache, uint32_t plan_flags);
```

The compiler emits one zero-initialized 8-byte global per allocation site and
passes its address as `site_cache`. The runtime stores the resolved descriptor
there, so a site searches the layout memo once and afterwards reads one pointer.

Nothing about that cache is trusted. On every call the runtime checks the plan
(rejected bit clear, array bit set, representation extended) and the cached
descriptor (element size, reference count, array kind, reference offsets). A
mismatch is not an error: the runtime behaves as though the compiler had said
nothing, resolves the layout itself, and rewrites the cache. A stale or corrupted
cache therefore costs a lookup, never a wrong representation.

## Who decides

| Fact | Compiler | Runtime |
|---|---|---|
| payload size, element count, reference shape | derives from the type | re-checks against the call |
| which representation an array uses | emits `EXTENDED` | validates, and arrays are extended regardless |
| resolved layout descriptor | caches the result of one lookup | owns the descriptor and validates the cache |
| arena or pool geometry | describes the payload | refuses a layout it cannot trace |

The last row is the interesting one. A deterministic region does not trace, so
`nrq_cdmm_pool_create_layout` and `nrq_cdmm_arena_allocate_layout` accept only a
plain record layout: no reference offsets, no structure-of-arrays columns, no
custom trace callback, not an array. A layout that owns managed references is
refused with a null result instead of being accepted and traced by nobody.

## Example

```nrqba
struct Point {
    int x;
    int y;
}

int function main() {
    var points = array.allocate<Point>(4);
    points[0].x = 1;
    print(points[0].x);
    return 0;
}
```

For this site the compiler emits plan `1 | (3 << 8)` (array, extended), a
16-byte-element shape, and one `nrq_alloc_site_N` word. The lowered call is
`nrq_gc_allocate_array_planned(slot, 4, 8, offsets, 0, &nrq_alloc_site_N, 769)`.

## Migrating

Nothing has to change. `nrq_gc_allocate_array_typed_rooted` and
`nrq_gc_allocate_soa_rooted` keep their signatures and their behaviour, and
`nrq_gc_allocate_array_planned` reaches the same allocation path with one extra
validated input. A site that does not want a plan passes `0` as `plan_flags` and
`NULL` as `site_cache`.

## Reproducing the measurements

```
nirqoba_phase5_benchmark 200000 7
```

The rows `gc-array-typed`, `gc-array-planned`, and `gc-array-planned-cold` compare
the typed entry point, a warm site cache, and a cache cleared before every
allocation. Numbers and variance are in `reports/aor/phase-6-performance.md`.
