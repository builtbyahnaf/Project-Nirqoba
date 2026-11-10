# ObjectHeader field audit — every site, and what each field's life is

Verified against `runtime/src/gc.cpp` at 0.341-beta. Counts are source sites
including the declaration; the roles are grouped by the phase they belong to.

## `next` / `tlab_next` — one union, two mutually exclusive lives (7 + 5 sites)

| phase | field | what happens |
| --- | --- | --- |
| allocation | `tlab_next` | written once: the new object is pushed onto the thread's unpublished batch |
| rollback | `tlab_next` | read to walk and discard a batch that could not be published |
| publish (safepoint) | `tlab_next` → `next` | the whole batch is spliced onto `unindexed_objects` with one pointer write |
| sweep setup | `next` | `sweep_list = unindexed_objects` — the budgeted work queue |
| sweep | `next` | read when an object is popped, rewritten when a survivor is relinked |
| diagnostics | `next` | walked to enumerate objects whose page has no start bitmap |

The union is sound because the two lives never overlap: an object is in exactly
one of "unpublished batch" and "published list". Both are live during marking,
which is why neither slot can host the mark stack.

Who uses it: only objects whose page has **no walkable start bitmap** (large-object
pages, or pages whose bitmap allocation failed) need `next` at all. For a page
with a bitmap the object is found by walking the page, and the field is unused —
but it is still reserved in every span, because an object does not know in advance
which kind of page it will land in.

## `mark_next` (7 sites)

| phase | what happens |
| --- | --- |
| push, ring not full | written `nullptr`, then the header goes into the 256-entry ring |
| push, ring full | written to the current overflow head, head moves to the object |
| pop | read to advance the overflow head, then written `nullptr` |

Life: within one mark cycle only; no other phase reads or writes it. It exists so
the mark stack is unbounded without allocating during marking.

## `page` (18 sites) — written once, never mutated

| phase | sites |
| --- | --- |
| allocation | 4: page identity check, storage offset, `live_objects++`, `young_objects++` |
| sweep, reclaim | 6: offset computation, `page_uses_index`, `clear_object_start`, `release_span_locked`, `live_objects--`, `young_objects--` |
| sweep, survivor | 2: `young_objects--` |
| sweep, empty page | 2: `retired = true` |
| diagnostics | 4: exact-object check, page walks |

Every write is at construction; nothing ever changes it. It is the only field with
that property, which is what makes it the only derivable candidate.

## `birth_epoch` (5 sites) — written once, read three times

Written at allocation from `next_cycle_epoch` (relaxed load). Read by the mark
filter (`>= context.epoch` ⇒ skip, it was born after the snapshot) and twice by the
sweep's retention test. Its whole purpose is one comparison per object per cycle.

## `element_count` (5 sites) — written once, read on every size query

Written as `1` for a record or the array length. Read in `object_payload_bytes`:
`element_count * layout->payload_size`, which is on the allocation, sweep and
diagnostic paths. A record stores the constant 1.

## `state` (9 direct sites + 20 accessor uses) — the only mutable field

| phase | sites |
| --- | --- |
| mark enqueue | read once for the marked/young test, written on entry |
| remembered-set insert | read for the flag, written on insert |
| sweep | read once, written twice at most (promotion clears; aging writes once) |
| diagnostics | read |

`set_young` and `set_age` have one site each left: the sweep's word-level rewrite
replaced the individual transitions, so the remaining setters exist for the paths
that touch one field on its own.

## `layout` (8 sites) — written once, read on trace and size

Written at allocation (or resolved by the intern memo). Read for `payload_size`
(size), `reference_offsets`/`trace`/`flags` (tracing), and by
`nrq_gc_object_layout` for diagnostics. Shared, immutable, never freed.

## What the audit says about candidates

- `page` is the only field that is a pure copy of something derivable, and its 18
  read sites cluster in allocation and sweep.
- `next`/`tlab_next` are load-bearing for a minority of objects but cannot be moved
  out without paying for it in every object's span: the alternative is resident
  side storage.
- `mark_next` is a per-cycle pointer, so the only cheaper replacement is
  cycle-transient storage, which allocates during marking.
- `birth_epoch`, `element_count`, `state` and `layout` are genuinely per-instance
  or per-layout facts; nothing in the runtime can reconstruct them.
