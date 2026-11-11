# `soa<T>` implementation progress

Design: `docs/compiler/soa-type-design.md`. This file is the persistent source of
truth: every decision, site, test result and remaining task goes here, so a session
that loses context can resume from it plus `git log`.

## Stage table

| stage | scope | state |
| --- | --- | --- |
| 1 | type kind, syntax, gate | done (`8f48c93`) |
| 2a | authoritative column layout, sema owned rules, type allowed in signatures | done (bec12d3) |
| 2b | struct-field resolution for `soa<T>` in signatures, layout report test | done (8425948) |
| 2c | allocation entry point `memory.owned_soa<T>(n)` (scalar fields only) | done (this commit) |
| 2d | column addressing in TIR + lowering for `ps[i].f` | next |
| 3 | strided GC trace callback for reference columns | pending |
| 4 | column views, benchmarks, docs, version milestone | pending |

## Stage 1 (`8f48c93`)

`TypeKind::Soa` (`compiler/include/nirqoba/Common/Type.h`), `Type::soa_type`
(`compiler/lib/Common/Type.cpp`), `soa<T>` parsing in
`parse_type_annotation` (`compiler/lib/Frontend/Parser.cpp`), `NRQ5010` rejection.
Tests: `tests/integration/negative/soa_requires_struct`.
Result: 210/210 unit, 266/266 integration.

## Stage 2a (bec12d3)

### Decisions

- **Column order is the struct's existing physical order** (widest-alignment-first),
  so a container and an array of the same struct never disagree on field order. This
  keeps E1's "single authoritative layout calculation" intact: `describe_soa` calls
  `aggregate_physical_order`, it does not re-derive an order.
- **Column offsets are stored count-independent.** A column's byte base is
  `index_in_bytes * count`, so the type does not have to carry an element count and
  the report can exist before allocation does.
- **The blanket parse-time gate is gone; the type is allowed in a program.** Nothing
  can *create* a container because no allocation entry point exists, so no unsafe
  state is reachable. The non-struct element check stays (`NRQ5010`).

### Modified files

- `compiler/include/nirqoba/Lowering/DataLayout.h` — `SoaColumn`, `SoaLayout`,
  `describe_soa(const Type&)`; the header comment carries the address formula.
- `compiler/lib/Lowering/DataLayout.cpp` — `describe_soa` implementation; SoA report
  block in `print_data_layout`; empty-aggregate exit now also checks for SoA types.
- `compiler/lib/Frontend/Parser.cpp` — `soa<T>` accepted in type position, element
  must be a struct.
- `compiler/lib/Sema/TypeChecker.cpp` — `is_soa()` added to `contains_owned_array`,
  `contains_array_view`, and `cleanup_for_type` (owned release), so the container is an
  owned value with deterministic destruction the moment it can be created.
- `tests/integration/negative/soa_allocation_unavailable` — asserts that no allocation
  path exists (`memory` is undeclared), replacing the obsolete gate test.

### Test result

210/210 unit, 266/266 integration after the stage.

### Known gap, and the exact next step (2b)

`soa<Point>` in a function signature carries the element struct **without its
fields**: the parser creates `Type::struct_type(name)` and semantic analysis resolves
fields only on paths that already know a struct type. `describe_soa` therefore returns
zero columns and the layout report prints nothing for a container.

Next step: resolve the element struct where signature/declaration types are resolved,
mirroring `resolve_generic_type` (`compiler/lib/Sema/TypeChecker.cpp:2780-2806`), adding
`if (type.is_soa()) return Type::soa_type(resolve_generic_type(type.element_type()));`
at the appropriate declaration-resolution site (the one that already gives a plain
`Point` parameter its fields). Then re-add
`tests/integration/positive/soa_layout_report` with these expectations, which were
verified by hand against `describe_soa`:

```
soa Point element 9 align 8 columns 2
  x : int index 0 (element 8 bytes, align 8)
  flag : bool index 8 (element 1 bytes, align 1)
```

## Stage 2b (8425948)

### Root cause of the stage 2a gap

The parser builds a named type as `Type::struct_type(name)` with no fields; semantic
analysis fills them in `resolve_aggregate_type` (`compiler/lib/Sema/TypeChecker.cpp:2035`),
which is what `resolve_declaration_types` calls for return types and parameters. It had
cases for arrays, owned arrays, owned pointers and views but not for a container, so a
`soa<Point>` parameter stayed fieldless and `describe_soa` correctly reported no columns.

### Change

`resolve_aggregate_type` now resolves through `soa<T>` as well:

```cpp
if (type.is_soa())
  return Type::soa_type(resolve_aggregate_type(type.element_type(), active));
```

It is placed with the other element-resolving cases, so recursion, cycle protection via
`active`, and nested aggregates behave exactly as they do for `owned_array<T>`.

### Verified

`--emit=layout` on `struct Point { bool flag; int x; }` now reports:

```
soa Point element 9 align 8 columns 2
  x : int index 0 (element 8 bytes, align 8)
  flag : bool index 8 (element 1 bytes, align 1)
```

Column order is the struct physical order (`x` before `flag`), element bytes are the sum
of the column element sizes, and the indices are the count-independent bases that
`describe_soa` documents. Test: `tests/integration/positive/soa_layout_report`.

Suite: 210/210 unit, 267/267 integration.

## Stage 2c — next, exact plan

1. Recognise `memory.owned_soa<T>(n)` as a builtin returning `soa<T>`, next to the
   owned-array cases in `compiler/lib/Sema/TypeChecker.cpp:121`, `:136` and `:2538`
   (arity: one element count, optional allocator). Reject it while the struct has a
   managed reference field, with a dedicated message, until stage 3.
2. Lower it in `compiler/lib/Lowering/TirToMlir.cpp` where `memory.owned_array` is
   handled (`:2729` for the call, `:3053` for the rooted allocation), passing
   `payload_bytes = count * describe_soa(...).element_bytes` and an element size of
   `element_bytes`, so the header records `element_count = count`.
3. No reference columns yet: the array descriptor is exact only for scalar fields,
   which is why (1) rejects reference structs.
4. Test: allocate, write and read one field per index, assert values; keep the
   `soa_allocation_unavailable` negative test replaced by a reference-struct rejection
   test.

## Stage 2c (this commit)

### What was implemented

- **Sema builtin** `memory.owned_soa<T>(n)`: struct element only, exactly one argument,
  and refused when any field is a managed reference. That refusal is the stage 3
  boundary: without a strided trace callback a reference column would be invisible to
  the collector. Diagnostic code `NRQ3015`, message names the offending column.
- **Parser**: `owned_soa` added to the generic-call detection list
  (`compiler/lib/Frontend/Parser.cpp:1108`). Without it the call was parsed as a
  variable reference and failed with "undeclared variable 'memory'".
- **Lowering** (`compiler/lib/Lowering/TirToMlir.cpp`, Call handler before the owned
  array branch): one GC object, `element_bytes = describe_soa(...).element_bytes` as
  both the runtime element size and the per-element byte cost, so the payload is
  `count * element_bytes` and the header records `element_count = count`. Root slot
  registered; descriptor pointer is an explicit null (`LLVM::ZeroOp`) with count 0.
- **Representation**: `tir_type_to_mlir` maps a container to an LLVM pointer, the same
  representation an owned array's data pointer uses.
- **Release**: `soa.owned_release` unregisters the root and nothing else. The block is
  the collector's, so freeing it would be a double free.

### Discovered dependencies (each cost a failed attempt)

1. A default-constructed `mlir::Value()` used as a call operand segfaults the
   compiler; the null must be materialised with `LLVM::ZeroOp`.
2. `nrq_memory_deallocate_checked` rejects a GC block at runtime ("invalid or already
   freed pointer"), which is what proved the release path must be unregister-only.
3. `tir_type_to_mlir` had to learn the type before any local of it could be allocated;
   the first insertion attempt landed outside the function and changed nothing, so the
   fix is anchored on the function signature itself.

### Tests

- `tests/integration/positive/soa_allocate_and_run` — allocates, prints, exits 0.
- `tests/integration/negative/soa_reference_column_rejected` — `NRQ3015`.
- Removed `soa_allocation_unavailable`, which asserted the absence of an allocation
  path that now exists.

Suite: 210/210 unit, 268/268 integration.

## Stage 2d — next, exact plan

Column addressing for `ps[i].f`. The address is `payload + column_base(f) + i *
column_element_bytes(f)`, where the two column quantities come from `describe_soa`
and nothing else. Sites:

1. Sema index handling (`compiler/lib/Sema/TypeChecker.cpp:2226-2250`) must accept a
   container base and return a *field proxy* concept rather than a struct value;
   `&ps[i]` and `ps[i] = point` stay errors.
2. TIR: the existing `GetElementPtr` op cannot express two independent scales, so add
   a `SoaColumnPtr` op (base, element index, column index) or lower the pair
   (index + field access) to one GEP with a runtime-computed offset; prefer the op so
   the lowering stays total.
3. Lowering: compute the mixed offset in i64 (`column.index_in_bytes * count +
   i * column.element_bytes`) and reuse the existing alignment recording with the
   column alignment.
4. Test: write and read every field of every index, including the last index, then
   force `gc.collect()` with the container reachable.

## Invariants that must not be broken

- Ordinary structs, AoS layout, and their tests are untouched by every stage.
- No program may allocate an SoA until stage 3's trace callback exists; a reference
  column without one is a use-after-free, not a missing feature.
- `NRQ5010` remains the code for "element is not a struct".
