# AOR representation diagnostics

**Status:** Phase 4, complete  
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

Everything here is a read-only observation of a running collector. None of it
changes representation selection, and none of it is on an allocation path.

## Per object

| Call | Answers |
|---|---|
| `nrq_gc_object_representation(object)` | which representation the object uses, or `NRQ_GC_REPR_AUTO` if the address is not a live managed object |
| `nrq_gc_object_header_size(object)` | 0, 8, or 16 bytes; 0 for a non-object |
| `nrq_gc_object_layout(object)` | the descriptor the object was allocated with, or null |
| `nrq_gc_pointer_is_managed(object)` | whether an address is a published object start |

An unpublished allocation reports no representation. That is the page-local
discovery rule, not a bug: a thread's TLAB batch becomes visible to the collector
(and to these calls) when the batch is published at mutator leave, a range-table
flush, or a cycle boundary.

## Fixed facts

`nrq_gc_get_representation_facts()` returns the build's constants:

```
headerless_header_bytes            0
compact_header_bytes               8
extended_header_bytes             16
max_header_bytes                  16
headerless_payload_alignment      alignof(max_align_t)
compact_payload_alignment         8
extended_payload_alignment        alignof(max_align_t)
headerless_state_bytes_per_object  1
```

These are what the tests pin. A build whose facts disagree with
`docs/memory-model/aor-headers.md` is a documentation bug or a header change.

## Live statistics

`nrq_gc_get_representation_stats()` walks the page set under the collector lock
and reports:

| Field | Meaning |
|---|---|
| `headerless_objects`, `compact_objects`, `extended_objects` | live objects by representation; large and fallback count as extended |
| `headerless_pages`, `compact_pages`, `extended_pages` | pages by class, large and fallback separate |
| `large_pages`, `fallback_pages` | exact-index pages |
| `header_bytes_saved_vs_extended` | header bytes the smaller representations save against H16 for this live set |
| `headerless_metadata_bytes`, `compact_metadata_bytes`, `extended_metadata_bytes` | page metadata actually allocated by each class |

The metadata fields are measured, not derived: a headerless page reports its four
bitmaps plus its state table, so a change to the state table shows up here rather
than only in a document.

## Where to look when a representation is wrong

1. `nrq_gc_object_representation` after publication. Before publication the answer
   is `AUTO` by design.
2. `nrq_gc_object_header_size` against the facts. A headerless object reporting 8
   means the page is compact, which means selection chose differently.
3. `nrq_gc_get_representation_stats().*_pages` for page-class churn. One page per
   object is the signature of a reuse failure; the mixed-shape slab bug looked
   exactly like that.
4. The cycle-end audit in development builds. It fails with
   `GC page describes a header larger than 16 bytes` or
   `GC headerless page cannot decode its objects` before any statistics can hide
   the problem.

## Cost

`nrq_gc_object_*` takes the collector lock and resolves the page: it is a
diagnostic, not a hot-path helper. `nrq_gc_get_representation_stats()` is linear in
the page count.
