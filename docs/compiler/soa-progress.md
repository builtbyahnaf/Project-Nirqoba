# `soa<T>` implementation progress

Design: `docs/compiler/soa-type-design.md`. This file is the resume point; update it
at the end of every stage.

| stage | scope | state |
| --- | --- | --- |
| 1 | type kind, its syntax, and a gate that forbids use | **done** |
| 2 | allocation + column addressing (no reference columns yet) | not started |
| 3 | strided GC tracing for reference columns, gate lifted | not started |
| 4 | borrowed column views, benchmarks, docs, version bump | not started |

## Stage 1 (landed)

- `TypeKind::Soa` in `compiler/include/nirqoba/Common/Type.h`, with `is_soa()`,
  `soa_element()` and `Type::soa_type(Type)` in `compiler/lib/Common/Type.cpp`;
  `to_string` renders `soa<Point>` and `mangle` renders `SOA_<element>`.
- `soa<T>` parses in type position (`compiler/lib/Frontend/Parser.cpp`,
  `parse_type_annotation`), alongside `owned_array<T>` and `array_view<T>`.
- **Gate**: any `soa<T>` in a program is rejected with `NRQ5010`, either because the
  element is not a struct or because allocation is not implemented. Nothing can be
  allocated, so no program can reach a mis-traced column. Tests:
  `tests/integration/negative/soa_gated_until_allocation`,
  `tests/integration/negative/soa_requires_struct`.
- Suite after the stage: 210/210 unit, 266/266 integration.

## Stage 2 — next, in this order

1. `TypeChecker`: treat `Soa` where `OwnedArray` is treated at
   `compiler/lib/Sema/TypeChecker.cpp:21-97` (release requirement, containment walk,
   borrow rules) and add the diagnostics that element identity cannot exist:
   `&ps[i]` and `ps[i] = point` are errors, not silent copies.
2. `DataLayout`: column layout derived from `aggregate_physical_order` — the single
   authoritative walk, per E1 — exposed as `describe_soa(Type)` returning per-column
   offsets and the element stride.
3. `TirToMlir`: allocation for `memory.owned_soa<T>(n)` next to the owned-array path
   (`:3053`), with `element_count` in the header and payload size `n * elem_size`
   summed over columns.
4. Column address lowering: `column_offset(f) + i * elem_size` for `ps[i].f`.
5. Keep `NRQ5010` in place for **reference columns only** until stage 3 emits their
   trace callback; scalar-only `soa<T>` may be ungated at the end of stage 2.

## Stage 3 — the one that must not be rushed

Emit a per-instantiation `nrq_gc_trace_fn` that walks each reference column with
stride `elem_size`, `count = object_bytes / elem_size`, and proves it with a forced
major collection over a reference column. Only then is `NRQ5010` removed for
reference-containing structs.

## Stage 4

`column_f()` borrowed views through the existing `ArrayView` path, benchmark against
`benchmarks/bottleneck_discovery/soa/{aos,soa,aosoa}.nrqba`, docs, version bump.
