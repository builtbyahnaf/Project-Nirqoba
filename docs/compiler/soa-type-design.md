# `soa<T>` — native structure-of-arrays, design and implementation plan

Status: **design only. No compiler or runtime change is in this commit.** This
document exists because the feature spans every stage and the edit sites have to be
named before any of them moves.

## What the type is

`soa<Point>` is a monomorphic type constructor over an **existing named aggregate**.
The field set is the struct's field set; nothing is declared twice, and no user
generics are introduced. That is the minimum capability the request allows: a new
type constructor over a named struct, not a general parameterised type system.

```
struct Point { int x; int y; managed<Node> owner; }
var ps = memory.owned_soa<Point>(n);   // one allocation, three columns
ps[i].x = 3;                           // column store
var xs = ps.column_x();                // borrowed view over one column
```

AoS stays the default. `soa<T>` is only ever written by the programmer.

## Representation

One GC object, header 48 bytes, payload = `n` contiguous columns in the struct's
existing physical field order, each column `n * elem_size` bytes:

```
payload = [ x: n * 8 ][ y: n * 8 ][ owner: n * 8 ]
column_offset(f) = physical_index(f) * n * elem_size
address(s, i, f) = payload + column_offset(f) + i * elem_size
```

`payload_size` in the descriptor is `n * elem_size` summed over fields, so `header + 1`
and the existing span arithmetic (Section C2) keep working unchanged.

## Identity, references, aliasing, mutation, iteration, views, ownership

| concern | rule |
| --- | --- |
| identity | The container has identity (one GC object, one header, one layout). Elements do **not**: `ps[i]` is a field proxy, not a value with an address, and `&ps[i]` is a compile error. This is the semantic price and it is a diagnostic, not a silent difference. |
| references | A reference field becomes a reference column. Tracing needs stride, which `reference_offsets` (element-relative) cannot express, so the descriptor carries a **compiler-emitted `trace` callback** per `soa<T>` instantiation: `count = object_bytes / elem_size`, then for each reference field visit `payload + column_offset + i * elem_size`. No descriptor ABI change: `nrq_gc_trace_fn(object, object_bytes, visit, ctx)` already carries what the callback needs. |
| aliasing | Columns are interior to one allocation, so two columns cannot alias each other. A borrowed column view may not outlive the container — the existing borrowed-view escape rules apply unchanged. |
| mutation | `ps[i].f = v` lowers to one store into one column. No element's field shares storage with another element's field, so mutation has the same isolation AoS gives. |
| iteration | Index loop over `i`; the container's column views make the hot-column loop the natural form (`xs[i]`). |
| views | `column_f()` returns a borrowed view (existing `ArrayView` machinery), so subview and return rules need no new code. |
| ownership / lifetime | Owned, deterministic, moved like any other owned value; destroyed at scope exit. No refcount, no GC-managed column semantics beyond the existing reference tracing. |

## Stages and edit sites

1. **Type system** — `compiler/include/nirqoba/Common/Type.h:37` `TypeKind` gains `Soa`;
   `Type` gains `soa_element()` plus `is_soa()`, mirroring `OwnedArray`. The type is
   created only by the type constructor, and its element must be `TypeKind::Struct`.
2. **Parser** — `compiler/lib/Frontend/Parser.cpp:268` `parse_struct` shows the aggregate
   path; type parsing gains `soa<` as a constructor next to `memory.owned_array<...>`
   handling. No new statement forms.
3. **Sema** — `compiler/lib/Sema/TypeChecker.cpp`: ownership rules at `:21-97` and the
   borrow checks at `:788-1140` extend to the new kind; new diagnostics for
   `&soa[i]`, for assigning a whole element (`ps[i] = point`, which cannot be one store),
   and for `soa<T>` where `T` is not a struct.
4. **Lowering** — `compiler/lib/Lowering/DataLayout.cpp` gains the column layout as a
   function of the struct's existing `aggregate_physical_order` (one authoritative
   calculation, per E1); `TirToMlir.cpp` field-access lowering around `:3911-3996`
   gains the column address form, and allocation at `:3053` gains the SoA descriptor.
5. **Runtime** — no new allocator path. The generated `trace` callback is the only
   runtime-facing change; `nrq_gc_layout_intern` already accepts a trace function.
6. **Verification** — layout report shows columns and strides; a GC test forces
   collection with a reference column and proves survival; identity/aliasing negatives
   are `expect_error` tests; benchmarks repeat the E4 AoS/SoA/AoSoA comparison with the
   native container as the SoA case.

## Acceptance criteria, in the order they must pass

1. `--emit=layout` reports `soa<Point>` with per-column offsets and stride.
2. `ps[i].x = v; ps[i].x` round-trips for every field, including a nested struct field.
3. A reference column keeps its targets alive across a forced major collection.
4. `&ps[i]`, `ps[i] = p`, and `soa<int>` are rejected with specific diagnostics.
5. Borrowed column views obey the existing escape rules (positive and negative tests).
6. Benchmark: native SoA within a few percent of the hand-written column arrays from
   `benchmarks/bottleneck_discovery/soa/soa.nrqba`, and clearly ahead of AoS.

## Why this is not in the tree yet

Stages 1–4 touch the type system, the parser, semantic analysis and the lowering, and
stage 6 requires the whole suite to run over a new aggregate kind. Landing any subset
leaves the compiler in a state where `soa<T>` parses but does not lower, or lowers but
traces wrongly — and a wrongly traced reference column is a use-after-free, not a
missing feature. The honest state is this document.
