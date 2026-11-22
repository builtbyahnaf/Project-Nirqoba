# CDMM explicit strategies

Nirqoba has three memory strategies, and a program chooses between them
explicitly:

| Strategy | Lifetime | Header per object | Reclaimed by |
|---|---|---|---|
| Ordinary owned | one allocation | allocator record (not in the object) | `memory.deallocate` / `secure_release` |
| Arena (bulk zone) | the whole allocation block | none | `arena_reset` / `arena_destroy` |
| Pool | one slot | none | `pool_release` / `pool_reset` |
| Collector-managed | reachability | 0, 8, or 16 bytes (AOR H0/H8/H16) | collection |

Terminology: an *arena* or *bulk zone* is the language-level allocator a program
chooses with `memory.arena_allocator`; the word **region** is reserved for the
collector's 2 MiB `GcRegion` storage and never names a deterministic allocator.

Nothing selects a strategy automatically yet. Phase 6 added the other half of that
connection: `nrq_cdmm_pool_create_layout` and `nrq_cdmm_arena_allocate_layout`
take a compiler layout descriptor, and `nrq_cdmm_pool_create_planned` and
`nrq_cdmm_arena_allocate_planned` take a compiler allocation plan, so the size and
alignment a compiler computed and the slot a pool reserves cannot disagree, and a
layout or plan a deterministic region cannot trace is refused outright. A compiler pass that chooses between arena, pool and collector
from the same metadata is still future work; see
[`alloc-hints.md`](alloc-hints.md) for the hint and plan it would consume.

## Why the explicit strategies are headerless

Phase 4 moved the collector from a 32-byte object header to a page that names the
representation: the page knows the header size, payload offset, slot size and
layout, so the object does not repeat any of it.

An arena and a pool have the same property for the same reason. An arena block's
layout is whatever the caller asked for, and its lifetime is the region's; a
pool's slot size, alignment and destructor belong to the pool. Neither needs a
tag in front of the payload. The metadata that remains is:

| Strategy | Per-object metadata | Per-chunk metadata |
|---|---|---|
| Arena | none, or one 32-byte destructor record if the object registered one | chunk header and bump pointer |
| Pool | 1 liveness bit, plus a 4-byte generation if requested | chunk header, bitmap, free list head |

A pool with generations therefore costs 4.125 bytes per slot; without them, 0.125.
That is the whole point of a pool: uniform objects let the allocator put the
bookkeeping in the page.

## Arena

```c
void* arena = nrq_cdmm_arena_create(initial_bytes, max_bytes);
void* p = nrq_cdmm_arena_allocate(arena, bytes, alignment);
nrq_cdmm_arena_reset(arena);      // destroys, then rewinds
nrq_cdmm_arena_destroy(arena);    // destroys, then releases the handle
```

- **Chunks.** The first chunk is committed at creation; growth chunks are the
  same size. `max_bytes` caps the total, and zero means no cap. A request that
  would cross the cap fails and leaves the arena unchanged; it is never served by
  committing more than the caller allowed.
- **Alignment.** Any power of two. Chunk bases are 64-byte aligned, so a request
  up to 64 bytes needs no padding.
- **Reset.** Runs the registered destructors, then releases every growth chunk
  and rewinds the first. Retaining one chunk is what makes a frame loop (allocate
  a frame, reset, repeat) allocation-cheap after the first frame.
- **Generation.** Every successful reset advances it. A pointer stored together
  with the generation under which it was allocated can be validated later:
  `nrq_cdmm_arena_generation` is the runtime half of the stale-reference check.
  The compile-time half lives in the language's arena rules.

### Destructors

```c
void* p = nrq_cdmm_arena_allocate_destructible(arena, bytes, alignment, fn, ctx);
```

The record is allocated in the arena ahead of the payload and linked newest-first.
On reset or destroy the list runs in reverse allocation order, so an object that
owns another arena object is destroyed after the object it owns. Allocation is
closed while destructors run: a destructor that tried to allocate would push a
record behind the one being walked. A destructor may release ordinary owned
storage — that is the nested-resource case — but must not allocate from the arena
or pool it is being destroyed with.

If the payload cannot be served after its record was, the arena is rewound to its
pre-call state, so a failed allocation leaves no stray record behind.

### Scopes

```c
NrqCdmmArenaScope scope;
nrq_cdmm_arena_begin(arena, &scope);
p = nrq_cdmm_arena_scope_allocate(&scope, bytes, alignment);
nrq_cdmm_arena_end(&scope);
```

A scope takes one registry lookup and one operation pin for the whole batch.
Single-object entry points take a shared handle lock plus the arena's operation
lock per call; a scope bypasses both. A scope belongs to one thread and must be
the only user of its arena while it is open. `reset` and `destroy` are refused
until it closes, which is exactly what the pin count reports.

## Pool

```c
void* pool = nrq_cdmm_pool_create(slot_bytes, alignment, slots_per_chunk,
                                  flags, destructor, context);
void* slot = nrq_cdmm_pool_allocate(pool);
nrq_cdmm_pool_release(pool, slot);
```

- **Free list in the slots.** A free slot holds the index of the next free slot in
  its first word, so a release and a reuse are O(1) and allocate nothing.
- **Liveness bitmap.** One bit per slot. A release of a slot whose bit is already
  clear is refused, which is the double-release check; an address that is not the
  exact start of a slot in this pool is refused too, so an interior pointer cannot
  corrupt a neighbour.
- **Generations.** With `NRQ_CDMM_POOL_GENERATIONS`, each allocation receives the
  next 32-bit generation for its slot. `nrq_cdmm_pool_release_checked` compares it,
  so a reference that outlived its allocation is rejected even when the address
  has since been handed to another object. Without generations that case is
  indistinguishable from a valid release at runtime; the language's borrow rules
  are what prevent it statically.
- **Destructor.** One per pool, because a pool is one type. It runs on release and
  on every live slot during reset and destroy, so deterministic cleanup costs no
  per-slot metadata at all.
- **Poisoning.** A released slot is filled with `0xDD` before it is linked, so a
  stale read is visible in a test or a debugger rather than reading a neighbour's
  free-list link.

## Coexistence with the collector

The two metadata systems never claim each other's storage:

- `nrq_gc_pointer_is_managed` answers only from the collector's page directory and
  large-object index; arena and pool chunks are not in either.
- `nrq_cdmm_strategy_of` answers only from the explicit-strategy range table;
  collector objects are not in it.
- A collection does not move, free or inspect explicit storage, and a reset or a
  release does not touch collector pages. The unit tests exercise both directions,
  including a collection while arenas and pools hold live objects.

## Introspection

```c
NrqCdmmStrategy strategy = nrq_cdmm_strategy_of(pointer);
NrqCdmmFacts facts;
nrq_cdmm_object_facts(pointer, &facts);   // header_bytes == 0, object_bytes, generation
```

`facts.header_bytes` is zero for both explicit strategies, which is the number a
future strategy-selection pass needs: an arena or pool object already pays no
header, so moving a value between these strategies changes lifetime, not layout.
`facts.metadata_bytes` is the owning chunk's metadata divided by its slot count —
the amortized per-object cost, not a guess.

## Failure and argument rules

Every entry point returns null or zero for an unknown handle, a zero size, a
non-power-of-two alignment, a destroyed handle, or a request the strategy cannot
serve. No failure path aborts the process and no failure path leaves a partially
recorded object behind. The handle value is freed on destroy, so a stale handle
looks unknown rather than resolving to a new allocator.

## Reading the automated decisions

Phase Psi connected the missing half of this document: automated selection now
routes an explicit allocation to the strategy its lifetime asks for. Storage that
stays in its activation goes to a stack slot, a pool group or the scope's scratch
arena; storage that escapes keeps the general allocator. Phase 9 makes those
decisions reviewable and gives the optimizer the facts they rest on.

### `--inspect-cdmm`

```
$ nirqoba build main.nrqba --inspect-cdmm
cdmm: 1 allocation site(s) classified
cdmm: main.nrqba:14:17 'value' int* -> arena (bounded to function)
```

One line per allocation site: where it is, the binding that holds it, the layout
it allocates, the selected strategy (`stack`, `pool`, `arena`, `gc`) and the fact
that selected it. The rationales use the analysis vocabulary: `bounded to
function` with `, loop-scoped` when the storage dies with its loop, `promoted to
the stack`, `repeated layout (N sites)` for a pool group, and the escape reasons
(`escaping via return`, `escaping via callee`, `escaping into foreign storage`,
`escaping inside an escaping object`, `escaping through a reference cycle`).
A site whose storage a promoted callee returns from reports `arena (returned to bounded
and escaping callers, arena handle checked at runtime)`, and a call site that passes
null reports `gc (escaping call site, null arena handle passed to <callee>)`.

The report is diagnostic only: it goes to stderr, the emitted program is identical
with and without the flag, and the attribute that carries the decisions to the
reporting pass is removed as it is printed.

### Dead allocations are not allocated

An allocation that nothing reads, whose pointer never leaves the activation and
whose payload needs no destructor is storage nobody can observe. It is removed
from the TIR before any strategy can claim it, so no allocator is called for it,
no arena is created for it and no release follows it.

This is the evidence selection already needs - bounded lifetime, no local use,
plain payload - so it costs no new analysis. Allocating and then releasing it
would have made the compiler do work whose only effect is to be undone.

### Promotion across a call

A function that allocates and returns the storage is, on its own, escaping: the
pointer leaves its frame, so the intra-procedural classifier has no smaller answer
than the general allocator. The caller is the one that can answer, so the
classifier now asks it.

For every call whose result the caller keeps in its own frame - no return, no
callee receives it, no store into memory the frame does not own - the call site is
marked *caller-bounded*. A single bounded call site is enough to give the callee a
trailing arena handle, because the handle is nullable: that caller passes the arena
it created, and every other caller passes null. Promotion is therefore per call
site, not per function, and one escaping caller no longer denies the arena to the
rest.

The callee branches on the handle once, at the allocation it returns:

```llvm
define internal ptr @alloc_and_return(ptr %0) {
  %2 = icmp eq ptr %0, null
  br i1 %2, label %5, label %3

3:                                                ; preds = %1
  %4 = call ptr @nrq_cdmm_arena_allocate(ptr %0, i64 8, i64 8)
  br label %7

5:                                                ; preds = %1
  %6 = call ptr @nrq_memory_allocate(i64 8, i64 8)
  br label %7

7:                                                ; preds = %5, %3
  %8 = phi ptr [ %6, %5 ], [ %4, %3 ]
  store i64 42, ptr %8, align 8
  ret ptr %8
}
```

The call sites differ only in that argument:

```llvm
; bounded: the arena this frame created and releases on the way out
%4 = call ptr @nrq_cdmm_arena_create(i64 4096, i64 0)
%5 = call ptr @alloc_and_return(ptr %4)

; escaping: the storage must outlive this frame, so the callee allocates it
%8 = call ptr @alloc_and_return(ptr null)
```

The caller's arena is created in its entry block and destroyed on its return
edges, and a bounded binding no longer releases the returned pointer: the arena
owns that storage, so the release would be a second, wrong free. An escaping site
keeps its own release, because the general allocator handed that storage out.

The report names all three decisions:

```
cdmm: main.nrqba:2:16 'node' int* -> arena (returned to bounded and escaping callers, arena handle checked at runtime)
cdmm: main.nrqba:13:13 'n' int* -> arena (bounded to the caller's frame, filled by alloc_and_return)
cdmm: main.nrqba:15:13 'm' int* -> gc (escaping call site, null arena handle passed to alloc_and_return)
```

### A loop reclaims its arena

A call site inside a loop usually keeps its storage for one iteration: the binding
is declared in the body, read there, and dropped there. The classifier picks the
narrowest loop that encloses the call and every use of the result, and the caller's
arena is then created on that loop's entry edge and released on its exit edges, so
a long-running loop's peak is one iteration's work rather than everything the frame
ever allocated.

```llvm
  ; preheader: one arena for the whole loop
  %5 = call ptr @nrq_cdmm_arena_create(i64 4096, i64 0)
  store ptr %5, ptr %2, align 8
  br label %6

6:                                                ; loop header
  %8 = icmp slt i64 %7, 3
  br i1 %8, label %9, label %19

9:                                                ; body: the callee fills it
  %11 = call ptr @alloc_and_return(ptr %10)
  br label %6

19:                                               ; exit edge: release
  %21 = call i32 @nrq_cdmm_arena_destroy(ptr %20)
  br label %17
```

The create is tagged `nirqoba.region_scope = "loop"`; a frame-scoped one is tagged
`"frame"` and sits in the entry block. The report names the scope, because that is
the difference between one arena for the frame and one per loop:

```
cdmm: main.nrqba:10:17 'n' int* -> arena (bounded to the loop, filled by alloc_and_return)
```

A call outside any loop, or one whose result outlives its loop, keeps the frame's
arena: releasing at a loop exit would free storage a later read still needs. The
scope analysis is the same one Psi-3 uses for local allocations, so a call site and
an allocation in the same loop describe exactly the same span.

### Recursive chains forward the handle

A promoted function that calls itself forwards the handle it was given:

```llvm
define internal ptr @build_list(i64 %0, ptr %1) {
  ...
  %13 = call ptr @build_list(i64 %12, ptr %1)   ; the activation's own handle
  ret ptr %13
}
```

`main` creates one arena and passes it once; every level down the chain fills the
same chunk, and the destroy at the bottom of `main` releases all of it. Passing
null here would put each level on the general allocator, which is what a recursive
builder did before the handle existed.

A site forwards only when its storage leaves the frame solely through the return:
the result is returned, no callee receives it, and no store puts it where the frame
does not own it. That is what makes the forwarding sound — the callee fills the
storage this activation is already handing back, so both belong to the same arena.
A self-call that hands its result to another function, or that is not on every
return path, stays escaping and passes null. Recursion through a second function
(`A -> B -> A`) does not forward yet: only a direct self-call has a handle of its
own to give away.

The callee still branches on the handle, because its body cannot know whether the
caller had an arena. A promoted callee's release of the storage it returns is
likewise conditional:

```llvm
  br i1 %3, label %15, label %16      ; %3 = the handle is null
15:
  call void @nrq_memory_deallocate_checked(ptr %9)
  br label %16
16:
  ret ptr %13
```

The release runs on the null path alone. With a real handle the arena owns that
storage and destroys the whole chunk, so an individual free would be an invalid
one.

The report names the site, so a chain that shares the caller's arena is
distinguishable from one that falls back at every level:

```
cdmm: main.nrqba:5:16 '<temporary>' int* -> arena (forwarded this activation's handle to build_list)
```

### What the optimizer is told

`noalias` follows from freshness: a pointer the allocator just returned cannot
overlap anything the program could already name. The alignment follows from the
payload's layout. Both are stated on the allocator's declaration:

```llvm
declare noalias align 8 ptr @nrq_memory_allocate(i64, i64)
declare noalias align 8 ptr @nrq_cdmm_arena_allocate(ptr, i64, i64)
```

A declaration is where LLVM keeps a callee's return attributes, so the facts reach
every call site and stay in one place as the lowering changes.

### What may live in an arena

An arena frees a chunk, so its payloads must be reclaimable by dropping the chunk
and nothing else. That rules out anything with a release of its own — owned text,
owned or dynamic containers, shared tokens, managed references, array views and
resource handles (`file`, `socket`, `mutex`, `mapping`, `secure`) — and it does
*not* rule out pointers:

```
struct Node { int value; owned ptr Node next; }
```

A node owns its child pointer, and the child is arena storage too, so the chunk
that holds the list reclaims the list. The classifier therefore accepts a payload
whose leaves are scalars and whose pointers name other eligible payloads, and it
assumes the node it is expanding is eligible while it expands it: a struct that
names itself is a linked list or a tree, not an error.

The rule lives in `nirq/common/Type.h` (`arena_eligible_payload`) so Sema's
allocation check, the allocation classifier and the call-site classifier all answer
it the same way. Sema uses it to accept an explicit allocation of such a payload:
releasing the block alone is the whole answer for it. A `shared` or `borrow` field
is a lifetime the type does not show, so a struct holding one never qualifies.

### A promoted activation lives in its caller's arena

A callee that is promoted and whose every call site passes a handle opens no chunk
of its own: its returned storage and every arena-eligible allocation it makes are
filled from the handle it was given. That is what keeps a recursive builder to one
arena per chain rather than one per activation:

```llvm
define internal ptr @build_list(i64 %0, ptr %1) {
  %8 = call ptr @nrq_cdmm_arena_allocate(ptr %1, i64 16, i64 8)
  ...
  %17 = call ptr @build_list(i64 %15, ptr %1)
  ret ptr %12
}
```

Because no call site passes null, the body needs no general-allocator arm and no
release of its own: the caller's destroy releases everything the activation and
its callees put in the chunk. A callee that is only partially promoted keeps the
null arm, keeps its own chunk for local work, and keeps the conditional release
that runs only when no handle arrived.

### Managed fields in arena storage

An arena releases a chunk, so a payload that owns storage of its own cannot be
reclaimed by dropping the chunk alone: the chunk frees the node, and the node's
buffer would leak. The arena therefore keeps a teardown registry, and a payload
with managed fields registers one record per field when it is allocated:

```llvm
%4 = call ptr @nrq_cdmm_arena_allocate(ptr %1, i64 32, i64 8)
%5 = getelementptr { ptr, ptr }, ptr %4, i32 0, i32 0
%6 = ptrtoint ptr %5 to i64
%7 = or i64 %6, 1
%8 = inttoptr i64 %7 to ptr
%9 = call i32 @nrq_cdmm_arena_register_teardown(ptr %1, ptr %4, ptr %8)
```

The compiler knows where the field's pointer word sits and the object's address is
the allocation's result, so registration follows the allocation rather than riding
on it. `context` is the field's pointer-word address with the release *kind* in the
three low bits - the address is at least eight-byte aligned, so the kind fits
without displacing a bit of it: 1 is `nrq_ffi_free`, 2 is
`nrq_file_text_release`, 3 is `nrq_array_free`, 4 is a fixed owner's buffer
(`nrq_memory_deallocate_checked`, since that buffer comes from the checked
allocator rather than the dynamic container's), and 5 is a file resource
(`nrq_file_close_handle`). One runtime destructor reads that encoding, which is
why no function pointer crosses the boundary and no thunk has to be generated per
payload.

`nrq_cdmm_arena_destroy` runs the records in reverse allocation order while every
chunk is still mapped, and only then releases the chunks. A node allocated three
times therefore registers three teardowns from one site - the site is in the
recursive activation, not in the caller - and a three-node chain releases three
buffers before the chunk that held them goes away.

A registered field that was never written holds null, and the destructor treats
null as nothing to do, so a partially built chain is as safe as a complete one.

#### What the arena still refuses

A `shared` field is a reference count, not storage: no chunk release can decrement
it, and there is no field teardown to name. A bare `string` is a view of someone
else's buffer. A resource handle whose domain has no teardown kind - `socket`,
`mutex`, `mapping`, `secure` - has the same problem, so only a file resource
travels through a chunk today. Payloads holding any of those keep the collector,
and an explicit allocation of one is still refused with `NRQ3015`.

A manual allocation has no registry at all: `memory.manual_allocate` frees the
block and walks nothing, so it refuses every payload with a managed field, not
only the ones no kind can describe.

### Pools reuse slots, so their teardowns run on release

An arena keeps its records until the chunk goes, because an arena never hands the
same address out twice. A pool does: the slot goes back on the free list and the
next allocation receives it, so a record that outlived its object would destroy the
next occupant's fields. The registry is therefore per object, not per pool:

```
%4 = call ptr @nrq_cdmm_pool_allocate(ptr %1)
%5 = getelementptr { ptr, i64, i64 }, ptr %4, i32 0, i32 0
%6 = ptrtoint ptr %5 to i64
%7 = or i64 %6, 3                     ; kind 3 = nrq_array_free
%8 = inttoptr i64 %7 to ptr
%9 = call i32 @nrq_cdmm_pool_register_teardown(ptr %1, ptr %4, ptr %8)
```

The pool consumes the records registered for an object when that object is
released, before the slot is poisoned and put back on the free list, and it consumes
every remaining record when the pool is reset or destroyed - a pool that died with
live slots would otherwise leak exactly the buffers those objects owned. The
`context` encoding is the arena's: the field's pointer-word address with the release
kind in the three low bits, read by `nrq_cdmm_release_managed_field`.

### A reset ends the generation

`nrq_cdmm_pool_reset` runs the same per-slot pass a destroy does, in the order that
makes a stale pointer harmless:


1. The live bitmap is scanned and every live slot's records run, newest first, so a
   slot's fields are destroyed while the payload is still readable.
2. The optional pool destructor runs, then the slot's generation advances and the
   payload is poisoned and linked back onto its chunk's free list.
3. After every chunk has been walked, the record table is cleared once. A chunk that
   held no live slot could not have registered anything, so the next generation
   begins with nothing to inherit.


The generation bump is what makes the reset a boundary rather than a fast free: a
pointer handed out in the generation that just ended fails its checked release
against the slot's new generation even before that slot is reused, and the liveness
bit refuses it again afterwards.


### A loop-scoped pool dies with its loop

A pool the classifier scopes to a loop is created on the edge into the loop's
header and destroyed on every edge out of it: a `break`, an early `return` and a
false loop condition each carry their own destroy block, so exactly one of them
runs and the loop's chunks are reclaimed when the loop ends. The span that decides
membership is re-derived at lowering time from the loop's header label, because the
teardown expansion changes the operation stream's length and a position measured
before it can land on the loop's own exit label.

### A payload that can close a cycle is collector storage

The classifier's shape analysis answers where a value's storage may live; it cannot
answer whether the value's *type* lets deterministic release describe it at all.
`is_structurally_cyclic` answers that second question from the program's struct
catalog: a loop of owning edges that crosses a reference count (`shared ptr`) is a
cycle no release can break, because the count includes the edge that closes it,
while a loop of unique owners is a tree - the closing edge would give the object a
second owner, and no well-formed program can write one. Borrows, raw pointers, views
and weak references keep nothing alive, and a `managed` reference leaves the cycle to
the collector.

A site whose payload can close such a cycle is accepted for the collector even when
its fields need destruction: the deterministic gate in Sema refuses a payload whose
release would not run its field teardowns, and a cyclic payload has no such release
to run. The report names the shape - `-> gc (cyclic object graph detected)` - rather
than an escape rationale, which is the difference between a fallback and a selection.

### The collector's heap, not a deterministic block

A payload the cycle analysis accepts is lowered differently from every other
ownership strategy. `memory.allocate<GraphNode>` that no stack, arena or pool
claims becomes a collector allocation: the site publishes the object into a
registered root slot, the root frame covers the call so the object is reachable the
moment it exists, and the payload's managed references travel with the allocation
as byte offsets the runtime walks while marking. The runtime never interprets a
type; it walks the table the compiler emitted for that layout.

The deterministic release disappears with the deterministic block. A Drop on a
collected object lowers into nothing, because the collector reclaims it and a manual
free would return a block the collector still traces. Nothing else moves: a payload
whose fields cannot close a cycle keeps `nrq_memory_allocate` and the free that
matches it, and the stack, the arena and the pool are still asked first.

A `shared ptr` field is a reference count, not a traced edge - the marking walk
sees managed references only - so a collected object's shared fields keep their own
protocol. The collector is their last owner: the payload's counts are registered
with it at allocation, as byte offsets the sweep walks once the object is gone.

### A collected payload returns its counts

The registration table is per object, not per layout. A layout describes traced
edges, and two payloads of the same geometry can still differ in which word carries
a count, so the offsets travel with the allocation call instead:

```
@nrq_gc_layout_8_16 = private constant [2 x i64] [i64 8, i64 16]
call void @nrq_gc_register_shared_fields(ptr %10, ptr @nrq_gc_layout_8_16, i64 2)
```

The table is keyed by the payload address. Reclaim releases each non-null word
through `nrq_memory_shared_release`, after the registry lock is dropped, so a last
release can take the shared-control lock without nesting inside the collector's.

The sweep has two ways to reclaim an object, and only one of them visits it: a page
whose objects are all unreachable is retired whole, from its counters, without
reading a header. That path is correct for a payload the collector has no
obligation to - nothing observable lives inside one - and wrong for a registered
payload, whose counts would outlive the storage that described them. A page that
holds a registration therefore falls through to the per-object sweep, which is what
runs the finalizer:

```
struct Node { int value; shared ptr Node next; shared ptr int counter; }

var keep = memory.shared_retain(counter);   // 2: keep and the field
gc.collect();                                // node unreachable
memory.shared_count(keep);                   // 1: the field's share came back
```

### A returned local is promoted, not coerced

`AstNode* function build() { var root = AstNode { ... }; return root; }` is the
shape a caller wants and a stack binding cannot give: the value would die with the
frame. The binding is promoted instead - the return site asks the CDMM selector for
an allocation, moves the value into it, and hands the caller the pointer.

The decision is made before the returned expression is checked, and that ordering is
the whole implementation. The ordinary check reports that an owned field cannot
escape an aggregate, and types the value as the aggregate; both are true of a stack
binding and false of a promoted one. So Sema answers the question first
(`promote_returned_local`), types the expression as the declared pointer, and the
later rules see a pointer that matches.

The synthesized site is an ordinary one, which is why nothing downstream needed a
new strategy:

```
cdmm: ...:7:5 '<temporary>' AstNode* -> arena (returned to bounded callers, arena handle checked at runtime)
cdmm: ...:11:16 'node' AstNode* -> arena (bounded to the caller's frame, filled by build)
  %2 = call ptr @nrq_cdmm_arena_allocate(ptr %0, i64 8, i64 8)
```

Promotion is only offered where the payload can actually be reclaimed: a bulk
release reclaims it whole, or the collector owns its shape. The frame's records for
the binding - its field walk and its release obligation - are deactivated at the
return, because the storage now belongs to the caller; leaving them armed is a
double free, and it is the failure this feature would otherwise introduce.

### Shared storage has a destructor too

Reference counting says *when* shared storage dies; it never said *what dies with
it*. A shared payload that holds an owned field used to be refused outright
(`NRQ3015`), because the count's last release freed the block and nothing walked the
words inside it. So a shared pointer could name a graph but not own anything.

The release that takes the count to zero is the payload's destruction, so the
allocation site registers one teardown per destructible field with the storage
itself - the arena and pool registries already had the shape, and the encoding is
theirs: the field's pointer word with the release kind in the three low bits.

```
%33 = call i32 @nrq_memory_shared_register_teardown(ptr %28, ptr %28, ptr %32)
...
%44 = call i32 @nrq_memory_shared_release(ptr %43)   ; count reaches zero
  -> nrq_cdmm_release_managed_field per registered field, then the block is freed
```

A release that only decrements touches nothing: another owner still reads those
fields. That is the whole difference between this registry and a stack local's
cleanup, which runs exactly once because the frame runs exactly once.

A destination in shared storage is therefore excluded from the frame's field walk.
`*node = Node { items: items }` in an ordinary frame destroys the literal's fields
when the frame ends; in shared storage it does not, because the fields now belong to
the object and the object's destruction belongs to whichever release reaches zero.
Sema records the destination's storage class on the assignment
(`destination_is_shared_storage`) and the TIR builder keeps the walk out.

With the gate gone, a cycle of shared pointers is expressible for the first time:
two nodes whose fields name each other hold two counts each. It is still not
*collectable* - that is what the collector is for, and a cycle of owning edges is
what `is_structurally_cyclic` routes to it.

### Only a live field is overwritten

A store into a shared field is two ownership operations: the value joins the field's
ownership, and the value the field already held leaves it. The second half is only
valid when the field held something. A field of a partially initialized aggregate
holds undefined bytes, and releasing those is a use of uninitialized storage - the
one thing a destructor-shaped protocol must never do.

Sema already tracks which fields have been written. The answer travels with the
statement: `AssignmentStmt::destination_holds_live_value`, then
`tir.store ... destination_holds_live_value`, and the lowering that emits the load
and release reads it there. A first store into a field emits the retain and the
store and nothing else. The same statement also records whether the value is an
existing reference or a fresh one: a name read out of storage keeps the count its
source owns, so the destination takes one of its own; a fresh allocation hands its
single count over, and retaining it would orphan the count.

Where a field is addressed, the catalog decides whether it holds a reference at all
- a GEP's own type has that erased - so the ids that name a shared field are
recorded when the address is formed.

### A dynamic array is a field

A `dynamic_array<T>` field owns its buffer, and both halves of the machinery above
can name its release, so the field is admitted. Its release kind is 3, and
`nrq_array_free` reclaims the buffer while the array's own three-word header lives
in the aggregate that holds it. An `owned_array<T>` field is admitted the same way
with kind 4, and a `owned file ptr` field with kind 5.

### Assignment through a pointer transfers the fields

An aggregate literal built on the right of an assignment owns its managed fields
from the moment it exists. When the assignment's destination is the storage a
pointer names - `*node = AstNode { children: items }` - the fields must end up
owned by the *storage*, not by the temporary that filled it:

```
tir.aggregate_field_destroy %17 field=0:nrq_array_free
```

One record stands for the whole walk instead of an emitted address-load-release
triple per field, because whether the walk should run is decided later, where the
storage's strategy is known. Lowering expands the record into a release per field
at the destination's address, or drops it when the destination belongs to an arena
or a pool: that strategy's registry already destroys the fields before it reclaims
the chunk, and running both would free the buffer twice. The decision traces the
destination pointer back through the loads and stores of its slot, so a pointer
loaded from a cleaned-out slot still finds the allocation it named.

The record is its own TIR operation rather than a call to an internal function, so
the escape and lifetime analyses do not read it as passing the storage to a callee
and demote the site to the collector.

