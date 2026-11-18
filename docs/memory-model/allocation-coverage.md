# Allocation coverage

Which allocation paths use a compiler plan, which use a typed entry point, and
which fall back on purpose. Every row was read off the lowering and the runtime,
not assumed.

## Compiler paths

| Source construct | Lowered runtime call | Plan | Site cache |
|---|---|---|---|
| `array.allocate<T>(n)` | `nrq_gc_allocate_array_planned` | array + extended | yes |
| `memory.owned_soa<T>(n)`, no managed column | `nrq_gc_allocate_array_planned` | array + extended | yes |
| `memory.owned_soa<T>(n)`, managed column | `nrq_gc_allocate_soa_planned` | array + extended | yes |
| `memory.allocate<T>(…)`, `memory.owned<T>` records | `nrq_memory_allocate` | none: owned storage, not collector storage | n/a |
| `memory.allocate_array<T>` (owned) | `nrq_memory_allocate_array` | none: owned storage | n/a |
| explicit arena request | `nrq_arena_allocate` / `nrq_memory_allocate_from` | none at the call site; the arena was created with a plan or a layout | n/a |
| explicit pool request | `nrq_cdmm_pool_allocate`, scope forms | none at the call site; the pool was created with a plan or a layout | n/a |

Records in Nirqoba are owned storage allocated by the general allocator, not
collector objects, so no representation is chosen for them and no plan applies.
The compiler therefore emits no plan for a record site; a record that needs the
collector is expressed as an array of one element or through the explicit API.

## Runtime entry points

| Entry point | Uses a plan | Representation |
|---|---|---|
| `nrq_gc_allocate` | no | Auto |
| `nrq_gc_allocate_repr` | no, explicit representation | caller's choice, validated |
| `nrq_gc_allocate_layout` | no, descriptor given | Auto |
| `nrq_gc_allocate_array` / `…_typed` | no | Auto |
| `nrq_gc_allocate_array_typed_rooted` | no | Auto |
| `nrq_gc_allocate_soa_rooted` | no | Auto |
| `nrq_gc_allocate_planned` | yes | H0, H8 or H16 from the plan |
| `nrq_gc_allocate_array_planned` | yes | extended |
| `nrq_gc_allocate_soa_planned` | yes | extended |
| `nrq_cdmm_pool_create_planned` | yes | no header (deterministic) |
| `nrq_cdmm_arena_allocate_planned` | yes | no header (deterministic) |
| `nrq_cdmm_pool_create_layout` | descriptor | no header (deterministic) |
| `nrq_cdmm_arena_allocate_layout` | descriptor | no header (deterministic) |

## Representation selection

| Payload | Record | Array |
|---|---|---|
| ≤ 256 B and fits a page | H0, header 0 | H16, header 16 |
| 257 B … page | H8, header 8 | H16 |
| > page | H16 | H16 |

The page-class check is the runtime's: a hint that asks for H0 on an object larger
than a slot is not honoured, it is upgraded. The plan predicts the same answer, and
`AllocPlanPropertyTest.ThePlanPredictsTheRepresentationForRecords` checks the two
against each other across 14 sizes.

## Intentional fallbacks

- A plan that is rejected, describes an array at a record site, or names
  deterministic storage at a collector site is ignored and the allocator decides.
- A site cache whose descriptor disagrees with the call's shape is discarded and
  rewritten; it never produces a wrong representation.
- A plan or layout with managed references at an arena or pool is refused with a
  null result, because a deterministic region cannot trace.
- The typed entry points stay in place: `nrq_gc_allocate_array_typed_rooted` and
  `nrq_gc_allocate_soa_rooted` are what a caller with no plan uses, and what the
  planned entries fall back to.

## Descriptor table footprint

Measured on a generated program with 40 `array.allocate<int>` sites:

| Item | Size |
|---|---|
| per-site cache global | 8 bytes (`[8 x i8]`, align 8) |
| 40 sites | 40 globals, 320 bytes of module data |
| object file for the 40-site program | 6 288 bytes |
| thread-local layout memo (16 sets × 2 ways + replacement bits) | 260 bytes per thread |

The per-call cost is one extra argument (the cache address) and one immediate (the
plan word). The per-site cost is the 8-byte word, which is only read and written on
the site's own allocations.
