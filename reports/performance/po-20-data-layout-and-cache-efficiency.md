# PO-20 — Data Layout and Cache Efficiency

**Task:** P2 3.11 — Data Layout and Cache Efficiency
**Date:** 2026-11-05
**Status:** Completed & Verified (layout and IR evidence, alignment counts, unit and
integration suites; timing and hardware-counter measurements deferred per the
project policy that this task must not be benchmarked)
**Compiler:** Nirqoba 0.127-beta, LLVM/MLIR 23.1.4, MSVC x64 RelWithDebInfo

---

## 1. Problem Identified

The task asks for cache efficiency through data layout: structure layout,
alignment, locality, false sharing, and object/array layout. Before this task the
compiler had a single, hand-written layout routine and emitted no layout facts at
all:

1. **Struct fields were stored in declaration order.** `{ bool, int, char, int }`
   cost 32 bytes with 11 bytes of padding, and every array of such a struct
   wasted a third of each element and crossed cache lines more often than it
   needed to.
2. **The layout was computed in four different places.** `type_size`,
   `type_alignment`, `collect_managed_reference_offsets` and the LLVM type
   construction each re-derived offsets with their own padding loop. Any change
   to the layout had to be made four times consistently, and a mismatch between
   the GC's reference offsets and the emitted type would have been a silent
   memory-safety bug.
3. **Nothing told the user (or the compiler team) what the layout was.** There
   was no way to inspect struct sizes, field offsets or padding without compiling
   and reading LLVM IR by hand.
4. **Every memory access claimed 4-byte alignment.** The LLVM dialect defaults an
   access without an `alignment` attribute to `align 4`, so an `i64`, an `f64` or
   a pointer slot was described as half as aligned as the layout guarantees.
   LLVM could not treat aligned 8-byte accesses as aligned, and reported 4-byte
   alignment to the vectorizer and the alias analysis.
5. **The runtime's hottest shared counters shared cache lines.** Every GC
   allocation updates three process-global atomics; they sat on the same line as
   the collector's phase, epoch and statistics, so a mutator's accounting
   invalidated a line the collector was reading even though the variables are
   unrelated.

Items 6 (array-of-structures versus structure-of-arrays), 7 (cache-aware
iteration) and 8 (prefetching) are addressed in section 8: they are not
implemented, and the reasons are stated there rather than a speculative
transformation being added.

## 2. Root Cause

The layout was treated as a property of the *declaration* rather than a property
of the *language*. Nirqoba has no FFI, no `sizeof`, no unions and no type
punning, and its only cast forms are numeric and boolean, so no code outside the
compiler ever observes a struct's byte layout. That makes the field order a free
choice, but only if all four consumers of the layout derive their offsets from
the same rule.

The alignment problem had the same root: the alignment of a `T`-typed location is
a property the compiler knows (it laid the storage out) but never stated, so
every access fell back to a generic default.

The false sharing came from declaring unrelated shared state next to each other
in one translation unit — the classic accidental layout of runtime globals.

## 3. Files Modified

| File | Change |
| --- | --- |
| `compiler/include/nirqoba/Lowering/DataLayout.h` | **New.** The single layout model: size, alignment, physical field order, field index mapping, `describe_aggregate`, `print_data_layout`. |
| `compiler/lib/Lowering/DataLayout.cpp` | **New.** Implementation of the model, the physical-order rule and the layout report. |
| `compiler/lib/Lowering/TirToMlir.cpp` | Uses the shared layout model; emits struct types, GEP indices, aggregate pack/extract order and GC reference offsets in physical order; tracks pointer provenance and states access alignment. |
| `compiler/include/nirqoba/Driver/CompilationPipeline.h` | `EmitKind::Layout`. |
| `compiler/lib/CLI/Driver.cpp` | `--emit=layout` parsing and help text. |
| `compiler/lib/Driver/CompilationPipeline.cpp` | Emits the layout report after TIR verification and stops. |
| `compiler/CMakeLists.txt` | Builds `DataLayout.cpp`. |
| `runtime/include/nirqoba/runtime/cache_line.h` | **New.** `nrq_cache_line_bytes` and `NrqCacheLineAligned<T>`. |
| `runtime/src/gc.cpp` | Allocation accounting, collection phase and barrier counter each isolated on their own cache line. |
| `docs/compiler/cli.md` | Documents `--emit=layout`. |
| `tests/unit/cli_test.cpp`, `tests/unit/gc_test.cpp` | New CLI and cache-line tests. |
| `tests/integration/{mlir,positive}/…` | Five new fixtures (section 6). |

## 4. Transformation Implemented

### 4.1 One physical field order

`aggregate_physical_order` sorts a struct's (or tuple's) fields by descending
alignment with declaration order breaking ties; `aggregate_physical_index` and
`aggregate_declaration_index` map between the declaration order used by the
front end and the physical order used by code generation. Every layout consumer
now reads that one rule:

- `tir_type_to_mlir` builds the LLVM literal struct in physical order,
- `GetElementPtr` and `llvm.extractvalue`/`insertvalue` translate a declaration
  index to its physical slot,
- `AggregatePack` inserts elements at their physical slots,
- `type_size`, `collect_managed_reference_offsets` and `describe_aggregate`
  walk the fields in physical order.

Before / after, for `struct Sample { bool flag; int count; char tag; int total; }`:

```text
- define internal { i1, i64, i32, i64 } @bump({ i1, i64, i32, i64 } %0, i64 %1)   ; 32 bytes
+ define internal { i64, i64, i32, i1 } @bump({ i64, i64, i32, i1 } %0, i64 %1)   ; 24 bytes
```

### 4.2 `--emit=layout`

The report prints, per struct, the size, alignment, padding, how many elements
fit in a 64-byte cache line, and the size the same fields would have cost in
declaration order — plus every field's type, offset and alignment. It is the
compiler's own ABI statement for the type, produced from the same code the
lowering uses.

```text
struct Wide {
  size 24 align 8 padding 3 elements_per_cache_line 3 declared 32
  first : int @ 0 (8 bytes, align 8)
  second : int @ 8 (8 bytes, align 8)
  tag : char @ 16 (4 bytes, align 4)
  flag : bool @ 20 (1 bytes, align 1)
}
```

Over the whole integration corpus (13 distinct struct types):

| Metric | Declaration order | Physical order |
| --- | --- | --- |
| Total bytes for the 13 types | 560 | 536 (−4.3%) |
| Padding reported by `--emit=layout` | — | 27 bytes |

### 4.3 Access alignment

Every `llvm.load` and `llvm.store` now states the alignment of the type it
touches, and the lowering tracks pointer provenance (frame slots, stack-placed
array storage, and element/field pointers derived from them) so an access into a
16-byte-aligned slot can say so. The claim is sound because a `T`-typed location
is always naturally aligned: the compiler and the runtime are the only creators
of typed storage, and the physical order keeps every field at a multiple of its
own alignment.

```text
- store { i64, i64, i32, i1 } %0, ptr %3, align 4
- store i64 %6, ptr %7, align 4
+ store { i64, i64, i32, i1 } %0, ptr %3, align 8
+ store i64 %6, ptr %7, align 8
```

Over the corpus at `-O0`, of 2009 counted loads and stores:

| Alignment stated | Before | After |
| --- | --- | --- |
| `align 4` (the dialect default) | 2009 | 90 |
| `align 8` | 0 | 1873 |
| `align 1` (`bool`/byte fields) | 0 | 46 |

The remaining `align 4` accesses are 4-byte `char` fields and internal
bookkeeping slots the lowering creates itself, not user data.

### 4.4 Cache-line isolation in the runtime

The three allocation-path atomics (`bytes_since_collection`,
`pending_allocation_bytes`, `pending_allocation_count`) plus the collection
threshold now live in a 64-byte aligned `AllocationAccounting` struct that is
both updated and read together, and the collection phase and barrier counter each
get their own line. `NrqCacheLineAligned<T>` and `nrq_cache_line_bytes` express
that requirement in one place.

## 5. Correctness Argument

**Physical field order.** The language has no FFI, no unions, no `sizeof`, no
pointer casts and no type punning, so a struct's byte layout is unobservable
outside the compiler. Symbolic access is by field name at the front end and by
declaration index in TIR; every translation of that index goes through
`aggregate_physical_index`, so a value produced by one part of the lowering is
consumed at the same slot by every other part. Struct types are structural in
LLVM, so two structs with the same fields in the same physical order are the same
type — which is also true before the change.

**GC reference offsets.** The collector is told where a struct's managed
references are by `collect_managed_reference_offsets`, which now walks the same
physical order. A struct's roots therefore stay consistent with its memory
layout; `struct_layout_gc_reference` builds a 32-node chain across two forced
collections and verifies every node survived.

**Access alignment.** Claiming `align N` is safe only if the address really is
`N`-aligned. Storage is created by `alloca` (aligned to at least the type), by
the runtime allocator (which honours the requested alignment) or by GEPs into
those, at offsets that are multiples of the field's own alignment. A pointer
whose origin the lowering cannot see (a parameter, a value loaded from memory)
gets the natural alignment of the accessed type and no more, which is the
language's invariant rather than an assumption about the address.

**Cache-line padding.** `alignas` and a padding member cannot change program
meaning; the counters remain plain atomics with their original memory orders.

## 6. Tests Added or Modified

| Test | What it locks down | Result |
| --- | --- | --- |
| `mlir/struct_field_layout` | Emitted struct type is `{ i64, i64, i32, i1 }`, the field GEP and `extractvalue` use the physical slot, and `{ i1, i64, i32, i64 }` never appears | pass |
| `mlir/access_alignment` | Slot, aggregate and field accesses carry `align 8` at `-O0` | pass |
| `positive/struct_layout_correctness` | Scalars, a nested struct, a fixed array field and a tuple inside a struct all read back their own values (17/5/4/10/true/true/q/47) | pass |
| `positive/struct_layout_gc_reference` | A struct with `int`, `bool` and one `managed<Node>` field keeps a 32-node chain alive across two collections; prints 528 | pass |
| `positive/struct_layout_report` | `--emit=layout` prints the expected size, padding, cache-line count, declared size and field offsets | pass |
| `CliTest.ParsesLayoutEmitKind` | `--emit=layout` parses; `--emit=layouts` is rejected | pass |
| `GcTest.SharedCountersDoNotShareACacheLine` | `nrq_cache_line_bytes == 64`, `NrqCacheLineAligned<T>` has size and alignment 64, and two instances never share a line | pass |

`struct_layout_correctness` is the important guard: a wrong field offset swaps
two values rather than crashing, so the fixture prints every field separately.

## 7. Verification

- `./build/msvc-dev/tests/nirqoba_unit_tests.exe` — **140/140**.
- `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe` —
  **241/241**.
- All corpus measurements above are static (emitted IR, `--emit=layout`, layout
  arithmetic) or structural (a unit test on the padding type), not wall-clock
  timings, per the project policy for this task.
- ABI and interoperability: Nirqoba has no foreign function interface, so no
  external ABI is affected. The runtime boundary passes only scalars and
  pointers, both unchanged. `--emit=layout` is the documented view of the layout
  for anyone reasoning about memory use.

Compilation time: the same corpus timing script used in PO-16 (149 programs at
`-O2`, best of three) reports **38.1 ms/program** with these changes against
**39.7 ms/program** for the pre-change compiler, i.e. no regression beyond noise.
The layout work itself is a single linear pass over each function's aggregate
fields plus one small sort per aggregate type, and `--emit=layout` only runs when
asked for.
**Memory:** no new per-function allocation; the provenance map is one entry per
pointer value already tracked in the translator.

## 8. Not Implemented, and Why

1. **Array-of-structures to structure-of-arrays.** The transformation is only
   correct if every access to a given array is field-wise, and it changes what a
   borrowed view of the array aliases. Without alias and escape proofs stronger
   than the ones in `3.1`/`3.2`, applying it would be the speculative
   transformation the task forbids. Padding elimination already removes the worst
   of the AoS waste (a third of each element for the mixed-width struct above).
2. **Cache-aware iteration (interchange, tiling).** Any of these needs trip-count
   and aliasing facts the compiler does not yet derive; the existing blocker for
   vectorizing branchy loops (the shared cold bounds-failure block, documented in
   `po-15`) applies first. Publishing layout facts (`align`, physical order) is a
   prerequisite for both and is done.
3. **Prefetching.** The task allows it "only when justified by measurements".
   This task's policy forbids runtime benchmarking, so `llvm.prefetch` is
   deliberately not emitted; adding it without evidence would be exactly the
   speculative transformation the task excludes.
4. **False sharing beyond layout.** The allocation counters are still updated by
   every mutator thread, which is true sharing, not false sharing. Removing it
   means per-mutator accounting (the collector already keeps per-thread pending
   batches), which belongs to `3.12`/`3.14`.
5. **16-byte alignment for heap array buffers.** The compiler already passes the
   element alignment to `nrq_memory_allocate_array`; raising it to a vector width
   changes the runtime's allocation padding contract and needs the allocator's
   size classes to be revisited, so it is left with a named follow-up.

## 9. Remaining Opportunities

1. Per-mutator allocation accounting to remove the residual contention on the
   allocation path.
2. Loop versioning to lift the bounds guard out of hot loops, which unblocks
   vectorization and cache-aware iteration (carried over from `po-15`).
3. `align 16` heap array buffers together with an `llvm.assume` on the returned
   pointer, so heap loops can use aligned vector moves.
4. Field-order hints in the language (a `packed`-like annotation) once users need
   to influence locality beyond size minimization.
