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
| 2c | allocation entry point `memory.owned_soa<T>(n)` (scalar fields only) | done (4bf57aa) |
| 2d | column addressing in TIR + lowering for `ps[i].f` | done (86fadcc) |
| 2e | runtime bounds check on the row index | done (61509a0) |
| 3 | reference-column tracing | done (3c63c47) |
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

## Stage 2c (4bf57aa)

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

## Stage 2d — implemented (86fadcc)

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

### What shipped (stage 2d)

- **TIR**: a dedicated `OpKind::SoaColumnPtr`. `lhs` is the container value,
  `operand` the row index, `field_index` the field's declaration index, and
  `string_value` carries the field name for diagnostics. It is deliberately not a
  `GetElementPtr`: a GEP multiplies one pointee type by its indices, while a column
  address multiplies two different scales, so reusing it would have encoded the
  array-of-structs arithmetic that this stage exists to avoid.
- **Sema** (`check_index_expr`): a container base yields its element struct only while
  `checking_soa_column_base_` is set; otherwise `NRQ3015` reports "an element of a
  container has no value; address a column as ps[i].field". `check_member_access` sets
  the flag around the base check and restores the enclosing value, so nested member
  accesses cannot leak the claim.
- **Builder**: both access paths recognise `Index`-over-container and emit one
  `SoaColumnPtr`: `lower_expression` for reads (then loads the address) and
  `resolve_lvalue_ptr` for writes. The field is looked up in the resolved element
  struct's declaration order; the lowering maps that index to the physical column.
- **Lowering**: `payload + column.index_in_bytes * count + row * column.element_bytes`
  in i64, then an `i8` GEP. `count` is loaded from `payload - 16`: that is
  `ObjectHeader::element_count` at header offset 32 with the payload at 48, both
  asserted in `runtime/src/gc.cpp`, and it is the same word the collector sizes the
  payload with, so it cannot disagree with the block that was allocated. No offset is
  hardcoded on faith.
- **Alignment**: the address records `min(container alignment, column alignment)`, so
  vectorised reads of a column are not blocked by the struct's larger alignment.
- **Printer/verifier**: cases derived from the GEP case; the printer shows
  `container[row] field=N`.

### Evidence that the addresses are the described ones

`tests/integration/positive/soa_column_read_write` prints 10, 11, 12, 13, 7, 43 across
two columns and four rows. Its IR shows the header load at `payload - 16`, row strides
of 8 (`element_bytes` for two ints), and the `y` column base at 16, which is
`index_in_bytes(4) * count(4)`: the numbers `describe_soa` reports, not assumed ones.

### Tests

- `positive/soa_column_read_write`: writes and reads both columns at several indices,
  including the last row.
- `negative/soa_standalone_element`: `var e = points[0]` is `NRQ3015`.
- `positive/soa_column_dynamic_row`: a variable row index keeps the column base fixed
  while the row term moves, so changing `row` cannot silently change which column is
  written.
- `negative/soa_element_assign`: `points[0] = p` is `NRQ3015`.
- `negative/soa_reference_column_rejected` still holds: element structs with a managed
  field remain refused until stage 3 traces strided columns.

Suite: 210/210 unit, 272/272 integration.

### Still open after this stage

- No runtime bounds check on the row index yet. Owned arrays behave the same, so this
  is consistent rather than a regression, but a container write past `count` corrupts
  another column; a check against the header count is the next safety item.
- Reference columns (stage 3), views over containers, and benchmarks (stage 4).
### Findings from the first implementation pass (0.363, opened in b556bd4)

The obvious route - let indexing produce an element pointer, then let ordinary field
access offset into it - is wrong, and the reason is structural rather than a missing
detail. A container element has no address of its own: its columns live at unrelated
offsets, each scaled by the element count. So the two ops the frontend would produce
today, `GEP(container, i)` followed by `GEP(ptr<Point>, f)`, can express only
`base + i * sizeof(Point) + offsetof(Point, f)`, which is the array-of-structs
address. Accepting it would read and write the wrong bytes, so indexing stays rejected
until the column form exists.

What the correct form needs:

- `count`, which is not carried by the container value. It is `header->element_count`,
  at offset 32 of the object header, and the payload starts 48 bytes in, so
  `count = load i32 at payload - 16`. One extra load per column access, from the same
  cache line as the rest of the header.
- A single addressing step, because the column base and the row stride have different
  scales: `payload + column.index_in_bytes * count + i * column.element_bytes`.
- A decision about `ps[i]` on its own. It has no representable value, because there is
  no struct to load, so sema must keep rejecting it as a value and accept it only as
  the base of a member access. That check is context-sensitive: the outer
  `MemberAccess` is checked first and can type the column, but the inner `IndexExpr` is
  still visited on its own, so sema has to remember which index expressions were
  already claimed as column bases.

Order of work, so each step is safe alone:

1. Sema: claim `Index`-over-container when it is the base of a member access, type that
   member from the element struct, and keep rejecting a bare `ps[i]`.
2. Builder: emit one `GetElementPtr` for the claimed pair, carrying the container as
   `lhs`, the row index as `operand` and the field as `field_index`, instead of the
   two-op chain. This reuses the existing op and needs no new op kind.
3. Lowering: when `lhs.type.is_soa()`, compute the mixed offset in i64 and take an `i8`
   GEP, then record the column alignment rather than the struct's.
4. Tests: write and read every field of every index including the last, then force a
   major collection with the container still reachable.

## Stage 2e — runtime bounds checking (61509a0)

### Investigation

Bounds checking already has one mechanism, and it was reused rather than paralleled:

- `TirToMlir.cpp:1626 emit_bounds_guard(index, length, loc)` zero-extends the index to
  i64, emits `icmp ult index, length`, and branches to a single cold failure block per
  function.
- That block (`bounds_fail_block`, `TirToMlir.cpp:1650`) calls
  `nrq_memory_array_bounds_fail` marked noreturn, cold and nounwind, then `unreachable`.
  One block for every check in the function keeps hot blocks contiguous, which the
  comment records as the reason it is shared.
- `runtime/src/allocator_windows.cpp:1281 nrq_memory_array_bounds_check` is the
  non-branching sibling; the fatal path ends in `nrq_runtime_fatal`, which exits 70 and
  writes the message to stderr.

### Implementation

One call site, in the `SoaColumnPtr` case of the lowering, immediately after the header
count load and after the row is extended to i64, and before `column_base`, `row_offset`
and the address GEP. Nothing is computed from the row until the check has passed.

The ordering matters twice over. A row past `count` would otherwise land inside the
next column, so a store there corrupts a different field rather than faulting; and
because the row is sign-extended before the unsigned compare, a negative row reads as a
huge index instead of wrapping into range. Emitted IR for a computed out-of-range row:

    %3 = getelementptr i8, ptr %2, i64 -16      ; element_count at payload - 16
    %4 = load i32, ptr %3
    %5 = icmp ugt i32 %4, 5                     ; count > row, row folded to 5
    br i1 %5, label %6, label %10
  6: ... getelementptr i8, ptr %2, i64 40 ...   ; address only on the in-bounds path
 10: call void @nrq_memory_array_bounds_fail()

The address GEP appears only in the in-bounds block, which is the property that makes
the check meaningful rather than decorative.

### Known wording gap

The shared failure block prints "owned array index is outside the array bounds", which
is imprecise for a container. A per-kind message would need a failure block per check,
which is exactly what the shared block exists to avoid, so the wording is recorded here
rather than changed in passing.

### Tests

- `positive/soa_column_bounds_edges`: rows 0 and 3 of 4 both pass.
- `negative/soa_column_bounds_negative`: row `0 - 1` exits 70.
- `negative/soa_column_bounds_count`: a write at row `count` exits 70 before the store.
- `negative/soa_column_bounds_dynamic_read`: row `2 + 3` read exits 70.
- `negative/soa_column_bounds_dynamic_write`: row `3 + 2` write exits 70.

Harness detail worth remembering: `build_and_run` compares merged output, so a runtime
failure message is asserted with `expect_stdout_contains`. `expect_stderr_contains` is
for compile diagnostics; using it for the runtime message fails the test even though the
program behaves correctly.

Suite: 210/210 unit, 277/277 integration.

### Remaining

Stage 3 (reference columns and strided tracing), views over containers, benchmarks, then
version 0.369 once two further upgrades have landed. No version bump here.
## Stage 3 — reference-column tracing (3c63c47)

### Investigation

The layout descriptor already had both halves of a tracing contract: a flat
`reference_offsets` table that the runtime walks once per element stride, and a
`trace` callback invoked as `trace(payload, payload_bytes, visit, context)`. The table
cannot describe a column, because its offsets are relative to one element while a
column base is `index_in_bytes * count`. The callback could, but it receives no
geometry, so a shared runtime callback has nothing to walk with.

### Design

- `nrq_object_layout` gained two trailing fields: `soa_columns` (offset, stride byte
  pairs) and `soa_column_pairs`. The compiler emits no `nrq_object_layout` literals, so
  no existing descriptor changes shape, and descriptors built by the existing pool are
  value-initialized, leaving the new fields null and zero.
- `nrq_gc_allocate_soa_rooted(root, count, element_bytes, pairs, pair_count)` interns
  descriptors in their own pool keyed by pair contents, then allocates through the
  existing `allocate_array_rooted_impl`. A separate pool keeps one key from meaning two
  different layouts.
- `trace_children` walks the pairs before the flat table: `rows = payload_bytes /
  payload_size`, then for each pair visit every non-null pointer at `base + row *
  stride`, with a bound test per row. Ordinary layouts are untouched.
- Lowering builds the pairs from `describe_soa`: for each column,
  `collect_managed_reference_offsets(column.type)` yields the intra-element offsets, so
  a column of structs that themselves contain references also works. Each pair is
  `(column.index_in_bytes + offset, column.element_bytes)`.
- Sema's gate narrowed from "any managed field" to owned arrays and array views, which
  need per-row destruction the column model does not have.

### Blockers found, with the evidence that located them

1. `collect_managed_reference_offsets` had no `soa` case, so the container's root slot
   was never published and allocation aborted with "rooted GC allocation requires a
   registered pointer root slot". A container value is one pointer to a traced object,
   so it now reports offset 0.
2. `type_size`/`type_alignment` returned 0 for `soa<T>`, which made the root descriptor
   invalid: "GC root registration received an undescribable layout". Both now report
   pointer size and alignment.
3. `null` is not an expression in this language (only raw pointer declarations accept
   it), so a null-reference test cannot be written in source. The runtime skips null
   column entries, but that path has no source-level test and is recorded as a
   limitation rather than claimed as verified.

### Tests

- `positive/soa_reference_retention`: two nodes in a column survive two collections,
  printing 11 and 22.
- `positive/soa_reference_column_order`: the reference column is the second column and
  each row's scalar sits exactly where a row-interleaved walk would look for a pointer.
  Retaining both nodes and reading 9/10/71/72 is the evidence the walk is column-strided
  rather than array-of-structs.
- `positive/soa_reference_zero_elements`: count 0, two collections, prints 42.
- `positive/soa_reference_reclaimed`: live-object count sampled while the container is
  alive, then after it goes out of scope and two collections run; prints 1, so retention
  did not become a leak.
- `negative/soa_column_owned_array`: an `owned_array` column is still refused.
- Removed `negative/soa_reference_column_rejected`, which asserted the gate this stage
  lifted.

Suite: 210/210 unit, 281/281 integration.

### Remaining

Views over containers, a source-level way to write a null managed reference, and the
stage 4 benchmarks. 0.369 is opened for 2d, 2e and 3 (f4a6bb6).
## Stage 3 post-implementation audit (this commit)

Six areas were checked against the code at 3c63c47. Two defects were real; the rest
held up.

### Defect 1: the SoA descriptor pool ignored the element size

`soa_layout_pool` was keyed by the column pair table alone. Two layouts can share
reference geometry and differ in payload per row, for example
`struct Narrow { managed<Node> node; }` and
`struct Wide { managed<Node> node; int pad; }`, which both yield the pair (0, 8) with
element sizes 8 and 16. The second container would have reused the first descriptor and
inherited `payload_size = 8`, so tracing would compute `rows = payload_bytes / 8`, twice
the real row count, and read padding words as pointers.

Fixed by making the intern key `[element_bytes, pairs...]`. The descriptor still stores
only the pairs, because `payload_size` already carries the size.
Regression test: `positive/soa_reference_mixed_sizes`, which populates the padding
slots with non-zero values so a misinterpreted walk is visible, then collects twice.

### Defect 2: no validation of element size or column bases

`nrq_gc_allocate_soa_rooted` cast `element_bytes` into the descriptor's `uint32`
`payload_size` without a range check, and accepted any pair table. A malformed pair
whose base was near the element size would have described pointer reads outside the
payload. Fixed with an `element_bytes` range guard and a per-pair check that
`base + sizeof(void*) <= element_bytes`, both returning null rather than building a
descriptor that cannot be honoured.

### Verified clean

- **Root versus child tracing.** Offset 0 for `soa<T>` is produced by
  `collect_managed_reference_offsets`, which feeds only root registration, where the
  convention is that offset 0 names the slot's own pointer (`nrq_gc_root_register`
  itself uses a zero offset). Element-relative child offsets live in the descriptor's
  `reference_offsets`, a different mechanism that the SoA layout leaves null.
- **Descriptor lifetime.** Pairs are copied into heap storage that is never reclaimed and
  the descriptor is never freed, so a live object's descriptor and its pair table stay
  valid for the process, across any number of collections and reuses.
- **Tracing.** Multiple reference columns (`soa_reference_two_columns`) and a
  reference inside a nested aggregate column (`soa_reference_nested`) both retain their
  targets across two collections; zero elements and repeated collections were already
  covered. Null entries are skipped by the walk, but no source-level null reference
  exists in the language, so that branch remains untested and is recorded as such.
- **Collector compatibility.** Ordinary descriptors are value-initialized, so the new
  trailing fields are null and zero and `soa_column_pairs` is 0; the flat-table path and
  custom trace callbacks are reached exactly as before. The suite is the evidence.
- **Ownership gate.** `contains_owned_array` and `contains_array_view` both recurse
  through structs, tuples and arrays and both treat `soa` as owned, so a column cannot
  hide an owned array, an array view or a nested container behind a field type.

Suite: 210/210 unit, 284/284 integration.
## Invariants that must not be broken

- Ordinary structs, AoS layout, and their tests are untouched by every stage.
- No program may allocate an SoA until stage 3's trace callback exists; a reference
  column without one is a use-after-free, not a missing feature.
- `NRQ5010` remains the code for "element is not a struct".
