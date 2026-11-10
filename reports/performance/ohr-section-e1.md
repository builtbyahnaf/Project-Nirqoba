# OHR Section E1 — physical field layout, inspected

Section E asks whether the payload can be arranged better. Before changing anything,
this records how it is arranged now, where each decision is made, and what the
arrangement costs. Everything below is SOURCE or MEASURED; nothing is projected.

## One calculation, three consumers

`compiler/lib/Lowering/DataLayout.cpp` is the only place a field offset is computed:

| consumer | entry point | line |
| --- | --- | --- |
| LLVM struct type construction | `for_each_field_in_layout_order` | `TirToMlir.cpp:74` |
| aggregate construction / extraction, field access | `aggregate_physical_index` | `TirToMlir.cpp:3911`, `3977`, `3996` |
| GC reference-offset table | `collect_managed_reference_offsets` -> `for_each_field_in_layout_order` with a running offset | `TirToMlir.cpp:778-804` |
| `--emit=layout` report | `describe_aggregate` | `DataLayout.cpp:136`, printed by `print_data_layout:194` |

There is no second layout algorithm. The tracing offsets are produced by walking the
*same* physical order the struct type was built from, so a reference field cannot be
reported at a different offset than the one the type places it at. This is the
property E1 requires ("Preserve a single authoritative layout calculation"), and it
already held before this section; the table is here so a future change that adds a
second walk is visible as a change to this table.

## The current rule

Fields are sorted by alignment descending, declaration order breaking ties
(`aggregate_physical_order:62`, a `std::stable_sort`). Aliasing and identity are
untouched because the rule only permutes a private layout: Nirqoba has no FFI,
`sizeof`, unions or type punning, so nothing outside the compiler can observe byte
order, and the record/tuple aggregate is still one allocation with one identity.

Measured with `--emit=layout` (MEASURED, this build):

| struct | declaration order | physical order | saved |
| --- | ---: | ---: | ---: |
| `Wide { bool, int, char, int }` | 32 | 24 | 8 |
| `Mix { bool, char, int, bool }` | 24 | 16 | 8 |
| `Nested { char, Wide }` | 32 | 32 | 0 |

## Padding: interior is zero, trailing is not

Padding in a `Wide` is 3 bytes and all of it trails the last field; the same holds for
every aggregate measured. That is not a coincidence of these two structs:

- Offsets are built by rounding up to each field's alignment.
- The order is descending, and every alignment `type_alignment` can return is a power
  of two (1, 2, 4, 8), because it is derived from a width in bytes.
- If the current offset is a multiple of `2^a` and the next field needs `2^b` with
  `b <= a`, the offset is already a multiple of `2^b`, so the round-up is a no-op.

Interior padding is therefore zero by construction, and the only remainders are the
tail that rounds the stride up to the aggregate's alignment. Removing that tail is not
available: it is what makes `array[i]` aligned for every `i`.

The report now prints the split (`padding 3 (interior 0, trailing 3)`) and
`struct_layout_no_interior_padding` pins it on a struct where declaration order costs
8 bytes more.

## What is left in E1, and why it was not done here

- **Compact field representations** (e.g. packing `bool` fields into a byte) change
  what a store to one field does to its neighbours under the concurrent mutator
  protocol, so they are a semantic change, not a layout change. Not attempted.
- **Reordering within one alignment class** can only affect which fields share a cache
  line. That is exactly E2, and the roadmap forbids guessing access frequency, so it
  needs a workload before it becomes a change.

## E2 prerequisites, from the report

`fields-beyond-line` is now reported per aggregate, so a hot/cold change has a
quantity to move. The decision needs evidence from a representative workload, which
the next E2 step is: measure a struct with a hot field first against the same struct
with it last, and only reorder if the difference survives. Until then the ordering
rule stays alignment-driven.
