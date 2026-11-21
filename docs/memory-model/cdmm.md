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
