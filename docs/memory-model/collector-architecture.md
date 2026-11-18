# Collector architecture: one collector, three headers

This is the reference for how the tracing collector handles the adaptive object
representations. It complements [garbage-collection.md](garbage-collection.md),
which records the interoperability contract, and
[allocation-coverage.md](allocation-coverage.md), which records which allocation
path every source construct takes.

## Reserved runs belong to a page generation

A refill does not take one run at a time. It reserves a batch of short runs and
hands them out from a thread list without the allocator lock, which is what keeps
a fragmented heap's few-cell runs from putting the lock back on the allocation
path. Two consequences follow, and both are load-bearing:

- **The list has its own generation.** The ordinary and the headerless refill
  share one list, but each keeps its own cursor stamp. A list stamped by the
  cursor of the other shape looks stale to the next refill of this one, and the
  discard path may then abandon runs that are perfectly valid. Abandoned runs are
  neither published nor free, so the page holding them can never be recycled:
  a silent leak that only shows up as a heap that will not shrink.
- **A cycle boundary must return every reserved run before it recycles a page.**
  The flush happens under the collector lock, before the page generation moves.
  A reserved run is the one piece of page space that is neither an object nor a
  free run, so a page holding one must not be judged empty.

The page generation itself is atomic: the allocation fast path reads it without
the collector lock, and the boundary publishes a new value under that lock. An
acquire load that observes the new generation tells a resumed mutator that its
cursor may point into a page the boundary has already dealt with.

## The strategy boundary

Nirqoba's hybrid memory model is not the collector's to change, and this document
describes only the collector side of it. The rules the collector obeys:

- **Deterministic ownership is the default.** Nothing is tracing-managed unless a
  program allocates it through a collector entry point.
- **Arena and pool objects keep their own lifetime.** A region is created,
  reset and destroyed by its owner; the collector never traces, reclaims or
  destroys a region or anything in it, and a region never releases a collector
  object.
- **A plan may choose a strategy, never change one.** A deterministic plan
  carries `NRQ_ALLOC_PLAN_DETERMINISTIC` and no representation; a region
  constructor refuses a plan that carries a representation, a header, an array
  or managed references, because a region does not trace. The collector's planned
  entry points refuse a deterministic plan outright and return null rather than
  quietly allocating collector storage for a site that asked not to be
  collector-managed.
- **AOR is a representation, not an ownership class.** H0/H8/H16 describes where
  an object's layout and state live *inside collector storage*. A 16-byte arena
  object has no representation and no header; a 16-byte collector record is
  headerless. Equal size, different ownership.
- **Cross-strategy references are visible, not merged.** An explicit region can
  hold a collector pointer and register that storage as an array root, which
  keeps the object alive while the registration exists. Releasing the root hands
  the object back to the collector alone; destroying the region does not touch
  it.

Two directions of the boundary are asserted by
`tests/unit/cdmm_coexistence_test.cpp` (collector pages are not in the region
range table, and regions survive collection) and by
`tests/unit/strategy_boundary_test.cpp` (a deterministic plan is refused by every
collector entry point and honoured by a region, representation does not decide
ownership, and an explicit region can root a collector object without owning it).

## Page classes are the representation

A page is dedicated to one representation, so an object never stores its own
header size. Discovery is one subtraction from the span start an address resolves
to, and the page supplies everything else.

| Page class | Header | Object state lives in | Layout comes from |
|---|---:|---|---|
| `Headerless` (H0) | 0 | page state byte per object ordinal | `page->uniform_layout` |
| `Compact` (H8) | 8 | `state` word in the header | `layout_id` in the header |
| `Extended` (H16) | 16 | `state` word in the header | `layout_id` in the header |
| `Large` | 16 | `state` word in the header | `layout_id`, exact object index |
| `Fallback` | 16 | `state` word in the header | `layout_id`, exact object index |

`Large` is a page larger than 64 KiB; `Fallback` is a page whose directory
insertion failed. Both use the exact address index instead of a start bitmap, so
the sweep treats them as a separate list rather than walking a bitmap they do
not have.

A headerless page is uniform: one slot size and one layout for every object. That
is what lets the object carry no header at all, and it is why a headerless page
cannot serve a different layout while any of its objects are live.

## Discovery

`find_object_locked` resolves a reference in this order:

1. Page directory: a direct-mapped probe, then an open-addressed table.
2. Start bitmap: the span start must be set and the derived payload must equal
   the address that was asked about. An interior pointer is therefore a decisive
   miss, not a hit on a neighbouring object.
3. Exact object index, only for pages without a start bitmap.

No step allocates and no step takes a lock: the caller holds the heap lock.

## Tracing

The mark worklist carries `ObjectRef{header, page}`, so the marker never re-probes
the directory for the object it is currently tracing. Tracing then reads the
descriptor once:

- a fixed record with one or two references and no callback takes the narrow
  path, reading the offsets inline from the descriptor;
- an array walks elements by the descriptor's element stride;
- a structure-of-arrays payload walks each managed column by `(base, stride)`,
  because reading it as rows would visit interleaved scalar slots;
- a descriptor with a `trace` callback visits the fields the table cannot
  describe.

Payload size is `element_count × payload_size`, so no per-instance size field is
needed and it cannot disagree with the descriptor.

A narrow record's child is resolved against the page the record itself is in
before the directory is consulted. A chain is allocated sequentially, so
consecutive nodes share a page almost always, and the page is already in hand:
the check is a range compare, a start bit and a payload compare, and anything
outside the page takes the ordinary discovery path. This is the marking change
Phase 8 measured, and it is why the directory probe is not on the per-object
path for a sequential graph.

## Generational state

Young, promotion age and remembered membership are tracked per object. Mark and
remembered membership are page-local bitmaps; birth-in-this-cycle is a page
bitmap stamped with the cycle epoch, so a stale bit from an older cycle is never
read and no page walk is needed to reset it.

The store barrier is armed only while a minor collection can run: it is raised
when a cycle starts and lowered at the end of a cycle if no old object exists or
generational collection is disabled. A registered mutator buffers referents in
its own array, so an armed store is a pointer store plus an append, and the
collector drains the buffers after parking every mutator.

**Documented conservatism:** a remembered entry is not removed when a program
clears the old object's field. The collector cannot know whether another old
object still references that young object without scanning every remembered
container, so the young object stays reachable for the minor generation. A full
collection marks from the roots only and reclaims it. `GcPhase7Test` pins this
behaviour rather than asserting a reclamation the collector does not perform.

## Allocation and publication

Thread-local reservations publish at a refill, at a safepoint that needs them, or
when a cycle prepares. Publication walks the reservation's page ranges and, per
representation, either sets a start bit (bitmap pages) or inserts into the exact
index (large and fallback pages).

An object that a cycle publishes is stamped with the cycle epoch that is about to
become active, not the one that is ending, so the sweep of that cycle sees it as
ordinary rather than as new. Objects published after the sweep captured its page
limit belong to the next cycle.

A headerless reservation is per layout and per slot size: switching layout starts
a new reservation instead of reinterpreting the cursor.

**Payload zeroing.** Every managed payload is promised zero-initialised. Storage
the OS has just committed already holds zeros, so a payload handed out from a
page's untouched tail needs no fill, and a page the allocator dedicated to one
large object - where the fill was measured at four fifths of the allocation cost -
needs none at all. The decision is made where the page is read anyway, at the
refill or at the single-object allocation, and carried to the object as a flag: an
earlier version asked the page per object and cost more than the fill it saved.
Each page carries the offset from which its storage is known to be zero; a
reclaimed span or a recycled page shell ends that claim, so stale bytes are never
handed out as zeros. A development build samples a freshly committed page against
that premise rather than trusting it.

## Sweep

Sweep visits pages in allocation order and consumes set bits only, so zero bits
are never visited and a chunk that stops mid-word resumes without re-testing.

A page a cycle has not yet touched is classified from its counts:

- all live: clear the page's marks and move on, without reading a header;
- all dead: clear the start bits, marks and remembered bits, return the whole
  bump area as one reusable run, and retire the page; managed payloads have no
  destructors to run;
- otherwise: per-object reclamation, which releases each dead object's span into
  its own page so the space is reusable rather than lost to the bump cursor.

Reclaimed spans are recorded per page (`FreeSpan`, 8 bytes), indexed so a refill
finds a page that can answer a request without walking the page set. Runs a
thread has reserved but not used never survive a cycle boundary: they are
returned to their page before the reclaimer decides which pages are empty.

The list starts inline at 384 entries and grows on demand, doubling up to 4 096,
only for a page that actually shatters. The fixed bound was a space defect, not a
conservative choice: at 16-byte slots a page holds 4 096 objects, so a heap that
loses every other object needs 2 048 runs in one page, and everything above the
bound was dropped. The matrix measures the consequence directly - the bytes a page
could not record, and the pages a second allocation wave commits beyond what the
heap already holds - and both fall to zero and to the minimum once the list can
grow. A page that reaches the grown limit keeps its largest runs and drops the
smallest, which is the last resort rather than the policy.

## What the measurements say

The Phase 7 harness (`benchmarks/aor/phase7`) measures these paths at nanosecond
resolution, which the stress corpus cannot do with a whole-millisecond clock.
Medians over five runs at one million objects, on the development runtime:

| Shape | Allocate | Full cycle, graph live | Full cycle, graph garbage |
|---|---:|---:|---:|
| H0 record, 16 B, chained | 38.6 ns/obj | 42.05 ms (42.1 ns/obj) | 67.6 µs (0.07 ns/obj) |
| H0 record, planned entry point | 40.3 ns/obj | 45.52 ms | 47.6 µs |
| H0 record, allocate + root store | 55.2 ns/obj | 38.17 ms | 52.5 µs |
| H8 record, 512 B (41,666 objs) | 145.1 ns/obj | 2.008 ms (48.2 ns/obj) | 44.6 µs |
| H16 array, 4 × 16 B | 58.3 ns/obj | 49.53 ms (49.5 ns/obj) | 1.18 ms |
| H16 large object, 200 KiB (256 objs) | 60.7 µs/obj | 94.7 µs (370 ns/obj) | 22.9 µs |
| H16 SoA, pointer column | 70.6 ns/obj | 46.83 ms (46.8 ns/obj) | 371.7 µs |

A dead graph is reclaimed by retiring whole pages, which is why its cost is a
small fraction of the cost of marking a live one. That is also why the stress
corpus reports `COLLECT_MS 0` for one million reclaimed objects: the phase is
tens of microseconds, and its clock cannot represent that.
