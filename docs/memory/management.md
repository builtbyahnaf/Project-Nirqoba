# Managed and explicit memory

Nirqoba currently has explicit raw allocations and a distinct tracing-GC domain.
`array.allocate<T>(count)` creates a managed array and `managed<T>` marks
references for tracing. Cycles are supported, and `gc.collect()` can reclaim
unreachable objects even when automatic collection is disabled. The runtime
starts with automatic collection disabled. Programs that explicitly use the
GC domain can enable automatic collection with `gc.enable()`; explicit
`gc.collect()` always remains available. The Phase 6 Upgrade decision is to make
ownership-based memory the language default. The compiler now tracks pointer ownership, moves, borrows, shared references, and deterministic cleanup for recognized resource producers. GC objects remain explicitly managed and cannot be released through the owned allocator.

The implemented memory domains include owned pointers and arrays (move and deterministic cleanup), borrowed pointer parameters and array views (non-owning and lifetime constrained), explicitly shared references (reference counted), and GC-managed references (traced and opt-in). Raw pointers remain available in the current low-level subset; full unsafe scopes and general ownership for arbitrary pointer fields are not implemented.

`gc.get_statistics()` returns `allocated_bytes`, `collected_bytes`,
`collections`, and `live_objects`. The collector is non-moving. It discovers
references from compiler registered roots and typed object layouts instead of
scanning arbitrary integer data.

The collector is precise, non-moving, generational, and incremental. It keeps a
young generation and an old generation. Young objects that survive a small
number of minor collections are promoted; a minor collection traces young
objects plus the remembered set of old-to-young references, while unreachable
old objects wait for a full collection. Automatic collection uses minor cycles
and inserts a full cycle at a fixed interval, so long-running programs
eventually reclaim old garbage. `gc.collect()` requests a full collection; the
runtime also exposes `nrq_gc_collect_minor()` for embedders. When generational
collection is disabled, a minor request runs as a full collection.

Marking and sweeping are sliced. After a short stop at the cycle boundary, where
registered mutators park to publish thread-local allocations and prepare roots,
marking runs in bounded slices with mutators resumed between slices. The sweep
then detaches its snapshot list and frees objects in bounded chunks while
mutators continue executing ordinary code. Objects allocated during the cycle
are retained until the next cycle. This is incremental and overlapping with
mutator execution, not a parallel collector: heap metadata, allocation-page
refills, root publication, and each sweep slice still use the collector mutex.

The runtime root-store and root-copy operations are write barriers. Generated
code uses them for root slots and managed-field stores; they retain the old
referent during marking (snapshot-at-the-beginning), mark the new referent, and
record old-to-young edges in the remembered set. Native code that writes a
managed reference field directly must use `nrq_gc_root_store` (or
`nrq_gc_root_copy` for aggregates); uninstrumented direct stores are outside the
collector's guarantee. The remembered set is conservative: a removed
old-to-young edge may keep a floating young object alive until it is promoted or
a later minor cycle reclaims it.

Use `memory.allocate<T>(bytes)` for explicitly managed raw memory and pair it
with `memory.deallocate(pointer)`. A failed allocation returns null. The
runtime tracks allocation ownership and rejects invalid and repeated frees.
The compiler detects use after supported deallocation through tracked aliases
in a function, but general raw-pointer use is not dynamically checked.

Explicit allocator profiling can be enabled with
`memory.profiler.start()`, sampled with `memory.profiler.capture()`, and
disabled with `memory.profiler.stop()`. Explicit allocation fields report
current, cumulative total, peak bytes, allocations, and deallocations. Each
capture snapshots GC live bytes, collected bytes, collection count, and live
objects. Explicit current/peak counters exclude the GC heap and mappings.
A capture also reports the allocation-size histogram (`allocations_le_64`
through `allocations_gt_65536` and `largest_allocation`), bytes currently held
per allocator kind (`general_bytes` through `custom_bytes`), and region
occupancy (`free_bytes`, `largest_free_block`, `fragmentation_bytes`).
Occupancy is sampled from the allocators whose free space is enumerable, so the
general-purpose and custom allocators report zero, meaning "not measurable".
`fragmentation_bytes` is `free_bytes - largest_free_block`: free space that
cannot serve the largest single request. Secure buffers can be created with
`memory.secure_allocate<T>(bytes)`, cleared with `memory.secure_zero`, and
released with `memory.secure_release`; release zeroes the tracked allocation
before returning it to its allocator.

Managed GC objects are non-moving. `memory.pin(object)` temporarily adds a GC
root and `memory.unpin(object)` removes it. Pair a pin with
`defer memory.unpin(pinned)` to release it on every supported lexical exit,
including return, break, and continue. A repeated pin of the same object is
accepted up to 16 times; further attempts return a null pointer and increment
`memory.pin_violations()`, while `memory.pin_count()` reports how many distinct
objects are pinned, so a forgotten unpin is visible rather than silent. File
mappings are explicitly released with `memory.unmap_file`;
`memory.mapping_size` reports their length, but ordinary pointer indexing does
not enforce mapping bounds.

`memory.mapped_read<T>(mapping, byte_offset)` and
`memory.mapped_write<T>(mapping, byte_offset, value)` validate the requested
range and stop with a runtime diagnostic when it falls outside the mapping.
`memory.start_ref_count()`, `memory.get_ref_count(object)`, and
`memory.stop_ref_count()` enable on-demand reference diagnostics; these counts
describe observed references and are not ownership counts.

`memory.available_bytes()` reports an estimate from Windows physical memory
status. It does not reserve memory. `memory.total_bytes()` reports installed
physical memory and `memory.used_bytes()` reports the process working set; both
are operating-system estimates, not accounting for Nirqoba's own heap.
`memory.runtime_version()` returns the runtime library's generated version and
phase string. `memory.cpu_count()` reports Windows logical processors.
`memory.allocator_kind(handle)` identifies the built-in allocator associated
with a handle and returns `-1` for an invalid nonzero handle; handle zero
reports the configured default allocator. `gc.is_enabled()` reports whether
automatic collection is currently active, without affecting explicit
`gc.collect()`.

Explicit allocations use the default general allocator unless the current
thread selects another allocator. These allocations are not visible to the GC.
See [allocator strategies](allocators.md) for selection and lifetime rules,
including the sharded inline ownership registry used by the default path
(0.465-beta).

`defer call(...)` captures call arguments when registered and runs calls in
LIFO order when a lexical block exits, including return, break, and continue.
It can release explicit allocations and restore a prior allocator selection.
Recognized owned pointers and resources are cleaned automatically at lexical exit, including return, break, and continue. Explicit `defer` may select a custom cleanup call and overrides the inferred automatic cleanup for that resource. Cleanup on exception unwinding remains unavailable because the language has no exception model. GC does not provide timely finalization.


memory.cpu_features() returns a bit mask of CPU features the Windows operating system reports usable by this process: bit 0 is SSE2, bit 1 is AVX, and bit 2 is AVX2. A set bit means the runtime reports support; callers should still use an appropriate target-specific code path and retain a fallback.

`file.open(path, writable)` creates an owned Windows file resource or returns null. The wrapper supports text and byte reads and writes. Use `defer file.close(resource)` when explicit close timing is needed; otherwise the compiler closes an owned file at lexical exit. Ownership moves invalidate the source binding. Use after move or close is rejected.

`memory.thread_count()` returns a Windows process-thread snapshot count. Because threads can start or exit during enumeration, treat it as an estimate rather than a synchronization guarantee.
`memory.device_memory_total_bytes()` and `memory.device_memory_used_bytes()`
report DXGI estimates for non-software adapters. They return zero when DXGI or
the adapter query is unavailable. Dedicated local memory and current local
segment usage are estimates; integrated adapters, shared memory, and drivers
may report different or incomplete values. These queries do not allocate or
change device memory.

## Ownership, borrows, and cleanup

Owned values from supported allocation and resource producers transfer when bound to another name, passed to an owning parameter, or returned through a matching owner return type. Resource-bearing structs, tuples, and fixed arrays move as whole values; their nested owned arrays are released recursively. Individual owned fields can be moved and reinitialized when the semantic checker can track the field path and all control-flow paths agree. This covers supported aggregate ownership, including nested owned arrays and resource-domain pointer fields. Dynamic owned arrays currently reject elements that themselves own allocations, because recursive element destruction is not implemented. Tracked owners are cleaned in LIFO order at lexical exit, including return, break, and continue. Owner moves clear the source slot so generated cleanup remains safe across conditional control flow. Reassigning a live owner is rejected; release it before reusing the binding.

Use `borrow T* name` for an immutable pointer borrow and `borrow mut T* name` for an exclusive mutable pointer borrow. Opaque resource parameters declare their domain, for example `borrow file byte* handle`, `borrow socket byte* handle`, `borrow mutex byte* handle`, or `borrow mapping byte* handle`. Calls must pass a matching resource, and resource APIs inside the helper check that contract. Borrows cannot be freed, closed, or passed to owning parameters; returning one requires the explicit source-bound borrowed-return form. The checker rejects conflicting aliases at a call site. Borrowed array views are described below. Tracked pointer borrows carry their originating owner through nested calls, and `&owner` is tied to `owner`; using or forwarding the borrow after the owner's scope ends is rejected (0.415-beta). Borrowed returns use an explicit source parameter and are checked against that source (0.419-beta), described below. A borrow of an aggregate field records the owner and dotted field path, so replacing that field, an ancestor, or a descendant while the borrow is live is rejected while independent sibling fields remain writable (0.423-beta). Mutable borrows are exclusive by owner and path: a competing mutable borrow or an overlapping read is rejected while the borrow is live, while disjoint sibling fields remain independently borrowable and call argument order participates in the same rule (0.427-beta). General interprocedural lifetime inference, closure capture, arbitrary returned references, and path-sensitive aggregate aliases remain unimplemented.

`defer call(...)` captures call arguments when registered and runs calls in LIFO order on lexical block exit, including return, break, and continue. It can release explicit allocations, unpin GC objects, unlock mutexes, close files, and restore allocator selection. An explicit deferred cleanup overrides the inferred cleanup for that owner. Cleanup during exception unwinding is unavailable because the language has no exception model.

Struct fields declare their ownership contract: `owned`, `owned <resource>`, `shared`, `borrow` and `borrow mut`. GC-managed and unqualified raw pointer fields remain non-owning.

## Shared references

`memory.shared_allocate<T>(bytes)` creates a reference-counted allocation. `memory.shared_retain(pointer)` creates another release token, `memory.shared_release(pointer)` consumes one token, and `memory.shared_count(pointer)` reports the current count. Use `defer memory.shared_release(reference)` for explicit lexical cleanup; shared parameters are released automatically when the callee exits.

A function parameter may declare `shared T* name`, and a function returning a shared pointer declares `shared T* function create()`. Calls transfer one token into or out of the function. Shared pointers cannot be passed to unique-owner parameters or released with unique deallocation APIs. Reference-count updates are synchronized; access to the pointed-to data is not made thread-safe. Shared references in aggregates are not supported.

## Native mutex resources

`mutex.create()` returns an owned native mutex. `mutex.lock(handle)` blocks until acquired, and only the owning thread may call `mutex.unlock(handle)`. Mutexes are non-recursive. The compiler destroys the owned mutex at lexical exit; use `defer mutex.unlock(handle)` so unlock runs before automatic destruction. Destroying a locked mutex, recursive acquisition, and unlocking from another thread are runtime errors. Pass a mutex handle through a borrow parameter when a helper only uses it temporarily.

## TCP socket resources

`socket.connect(host, port)` creates an owned Windows TCP socket or returns
null. Ports must be between 1 and 65535. `socket.send(handle, text)` sends the
entire UTF-8 byte sequence and returns its byte count, or a negative Winsock
error. `socket.receive(handle, destination, capacity)` reads up to `capacity`
bytes and returns the count, zero on peer shutdown, or a negative Winsock
error. Send and receive can proceed concurrently on one connection. Closing
shuts down both directions and waits for active operations to finish.
`socket.close(handle)` closes the owned resource and returns zero on success.
Use `defer socket.close(handle)` when choosing the close point; the
compiler otherwise closes the owned socket at lexical exit. Socket handles are
opaque, move with ownership, and cannot be indexed. Borrow parameters can be
used by helper functions that send or receive without taking ownership.

## Owned arrays and borrowed views

`memory.owned_array<T>(count)` creates one contiguous, uniquely owned buffer. `memory.owned_array_aligned<T>(count, alignment)` requests stronger power-of-two alignment, clamped to the element type's natural alignment. Allocation uses the selected explicit allocator. The owner exposes `.length` and checked indexing and is released automatically at scope exit. Returning the array or passing it to an `owned_array<T>` parameter transfers the allocation.

When an owned array element type contains GC-managed references, the runtime registers the backing storage as a precise repeated-layout root region. Dropping the array unregisters that region before freeing the explicit buffer. The buffer remains explicitly owned; GC traces referenced managed objects without managing the buffer allocation itself.

`memory.array_view<T>(source, start, length)` creates a read-only, non-owning view into an owned array or another view. `memory.array_view_mut<T>(owner, start, length)` creates an exclusive mutable view from a mutable owned array. Mutable views must be bound with `var`, can be passed to `borrow mut array_view<T>` parameters, and allow checked indexed writes. Immutable views cannot be upgraded to mutable views. While a mutable view is live, the compiler rejects indexed reads or writes through the owner; while any view is live, it rejects moving, releasing, or mutating the owner. A mutable borrowed parameter cannot be returned under an immutable lifetime because that would leave a mutable alias active beside the returned reference. End the view's lexical scope before performing owner operations. View lifetime tracking is conservative and lexical; views cannot be stored in aggregates or returned as mutable views.

General native-resource field ownership, shared references inside aggregates, closure capture, exception-unwind cleanup, GPU buffers, and non-Windows runtime support remain future work. Supported owned field paths already permit partial moves and reinitialization.

## Raw pointers (`ptr T`, `&`, `*`)

The pointer spelling from the language specification is implemented for scalar
variables. `var p: ptr int = &s;` declares one pointer level, `ptr ptr int` adds a
second, `&name` takes the address of an existing variable, and `*p` reads, assigns
or updates the object at that address (`*p = 100;`, `*p += 5;`). Taking an address
neither copies nor transfers ownership and it does not keep the owner alive. When
the result is bound to a pointer, the checker records the originating owner and
rejects a use or forwarded call after that owner's scope ends; a plain raw pointer
produced outside that path is still unchecked.

Two limits are deliberate in this increment. Only numeric, boolean and pointer
operands are addressable; taking the address of a struct, an owned array or a
managed reference is rejected, because a raw alias into those could bypass
deterministic destruction or the collector's write barrier, and dereferencing a
`managed<T>` reference is rejected for the same reason, so managed field access must
use the ordinary member path. A field projection is addressable when its leaf has
numeric, boolean or pointer storage; the checker records the dotted path and ties
the alias to the aggregate binding, not to the field alone.

A pointer can be printed: `print(p)` writes the address without dereferencing it, and
a null pointer prints `NULL` so absence is distinguishable from address zero. The
null pointer literal is `nullptr`; it has no intrinsic type, so a declaration must
supply one (`var q: ptr int = nullptr;`). `NULL` is the nullable-value literal from
§7.3, not the pointer literal. Nullable pointer types (`@Nullable ptr T`, `is NULL`
checks), pointer interpolation, and general cross-function lifetime analysis remain
unimplemented. `nullptr` may be assigned to an existing pointer binding, and `is` /
`is not` compare a pointer with `nullptr`, adopting the pointer's type; comparing
`nullptr` with a non-pointer is rejected.

## Nullable pointer bindings (0.379-beta)

`@Nullable` on a `var`/`let` declaration lets a pointer binding hold `nullptr`; plain
pointer bindings reject `nullptr` at initialization. The nullable flag lives in symbol
state, and `*p` is legal only while the checker has a non-null proof for that name.
Proofs come from `p is not nullptr` in a then-branch and from `p is nullptr` in the
else-branch or after a then-branch that cannot fall through; they are branch-scoped
and dropped by any assignment. Nullability is a property of the binding, so it does
not follow a copy into another binding or a function signature yet.

## Confirmed syntax conventions

Decisions confirmed against SYNTAX.md sections 16-17. They are the target for the
pre-migration grammar work, not a description of what the current parser accepts.

- **Declarations.** C-style is the implemented convention: `Type name = value;` and
  brace initializers such as `Point p = {1, 2};` (positional or named). `var`/`let`
  remain alternatives and also accept the type-first spelling
  `var ptr int g = nullptr;`. A brace initializer is only valid for a struct type:
  `ptr int g = {1};` is rejected with a diagnostic. A type-first declaration is
  recognised only when the declared name is followed by `=`, so expressions are not
  misread. Call-style `Point(1, 2)` is still unsupported.
- **Null literals.** `nullptr` and `NULL` are the same null pointer literal in
  initializers, assignments and `is` / `is not` checks (implemented in 0.379-beta).
- **Nullable placement.** The annotation qualifies the type:
  `@Nullable ptr int p = NULL;` or `@Nullable var ptr int p = NULL;`. The older
  `@Nullable var p: ptr int = NULL;` still parses for compatibility.
- **Unknown qualified generics.** A qualified generic type with no known meaning is
  rejected, never reinterpreted as a bare type name.
- **Collection names.** `owned_array<T>` and `array_view<T>` remain the builtins;
  `Vector<T>` and `Span<T>` are reserved for later library aliases.
- **Packing.** Explicit packed layout is planned and out of scope for now.

## Borrowed struct fields (0.384-beta)

A struct field may be `borrow ptr T` or `borrow mut ptr T`. The qualifier lives on the
field description (the same `Parameter` record used for function parameters), so no
separate field-ownership representation exists. Construction requires a borrow for the
field; an owned value is rejected. Reading through both forms is allowed, writing
through `borrow` is rejected, and `borrow mut` permits writes. Borrowed fields own
nothing, so destruction generates no cleanup for them and no reference counting is
involved. Field lifetimes are not yet checked against the containing value, and `owned`
or `shared` pointer fields remain rejected until field destruction exists.

## Owned struct fields (0.389-beta)

`owned ptr T` is stored as `owned_ptr<T>`, so ownership lives in the type and the same
rules that already governed owned arrays apply to the field: the aggregate is move-only,
a copy transfers the value and invalidates the source, and field reinitialization is
only legal on a moved path. `aggregate.owned_drop` walks fields in reverse declaration
order and, for an owned pointer field, releases the GC root registration and calls
`nrq_memory_deallocate_checked` on the stored pointer. No reference counts, no metadata
and no per-operation checks are added; destruction is statically generated from the
field types. `shared` fields and resource-domain owned fields remain rejected until
their destruction paths exist, and `uninit` is still absent.

## Owned field reinitialization (0.390-beta)

After an owned field is moved out, assigning a fresh owner reinitializes it: the
assignment accepts a raw pointer of the field's element type and performs the same
raw-to-owned coercion as construction, so `h.data = memory.allocate<int>(n)` is valid
on a moved field. A field that was not moved on every path is still rejected, and the
owner must be live and mutable. Aggregates that hold owned pointers may be nested in
other structs; `aggregate.owned_drop` recurses into them, while owned arrays whose
elements own resources remain rejected.

## Shared pointer fields (0.391-beta)

A `shared ptr T` field has the type `shared_ptr<T>` (`TypeKind::SharedPointer`). It is
a full pointer for dereference, printing, size and alignment (one word), and lowering,
but it carries ownership in the type itself, so `aggregate.owned_drop` can see it.

- **Construction** takes the initial reference from `memory.shared_allocate` or
  `memory.shared_retain`; no extra retain is emitted.
- **Copy** of an aggregate with shared fields (`var b = a;` or `b = a;`) emits
  `aggregate.shared_retain`, which calls `nrq_memory_shared_retain` once per shared
  field, including fields nested in structs, tuples and arrays.
- **Move** of an aggregate that also owns resources transfers the references: the
  source is zeroed, no retain is emitted, and only the destination releases.
- **Destruction** calls `nrq_memory_shared_release` once per shared field, in reverse
  declaration order, recursing into nested aggregates.
- Only shared values may initialize a shared field; owned, borrowed and unqualified
  raw pointers are rejected. `shared` locals and parameters keep their existing
  move-on-copy behavior.

## Partial initialization with `uninit` (0.396-beta)

`var aggregate: T = uninit;` creates a zero-initialized aggregate and marks every
field as not live, reusing the moved-field state rather than introducing a runtime
mask. Field assignment initializes the field and clears that path (and its
descendants); reading a field before it is initialized is NRQ3033. Destruction runs
the normal recursive `aggregate.owned_drop`, which releases only fields holding a
non-null owner: uninitialized fields are null because the slot was zeroed, so they are
skipped by the runtime without extra checks. Ordinary fully initialized aggregates are
unaffected and carry no additional state.

## Partial-initialization guards (0.397-beta)

`uninit` keeps its zeroed-slot, field-by-field model, but two unsafe uses are now
rejected at compile time.

- **Whole-value use:** a Name expression naming an aggregate with any uninitialized
  field is rejected unless it is an assignment target or the base of a member access.
  This covers copies, moves, returns, argument passing and other consuming contexts.
- **Partial overwrite:** when a nested field is initialized (`o.inner.data = ...`), the
  enclosing path (`o.inner`) is recorded as partially initialized. Assigning the whole
  field afterwards is rejected, because replacing it would overwrite the initialized
  contents without releasing them. Assigning the remaining fields individually stays
  valid.

Neither guard adds runtime state: both are semantic checks over the existing
per-symbol field sets, and destruction remains the zeroed-slot recursive drop.

## Initialization completion (0.401-beta)

Field-by-field initialization now completes a nested aggregate. After each field
assignment the checker tests whether every child of the enclosing aggregate is
initialized; when it is, the enclosing path leaves the uninitialized set and the test
repeats for its own parent. A completed nested aggregate can therefore be read or
copied as a whole, and a parent whose fields were all completed that way can be used
as a whole as well.

Completion feeds the existing ownership paths rather than adding new ones: copying a
completed aggregate with shared fields retains them, and moving a completed aggregate
with owned fields transfers them. Aggregates that are still incomplete keep every
guard: whole-value use, reads of uninitialized fields and whole-field overwrites stay
rejected.

## Resource-domain owned fields (0.403-beta)

`owned file ptr byte`, `owned socket ptr byte`, `owned mutex ptr byte`,
`owned mapping ptr byte` and `owned secure ptr byte` declare fields whose release
belongs to a runtime domain. The parser records the domain on the owned-pointer
type itself, so construction, equality, mangling and lowering all see it; no
side table can drift from the field type.

Construction accepts only a value whose cleanup is the field's domain: a fresh
domain producer, a matching owned local, or a matching resource field. A plain
`owned ptr T` field rejects resource domains because its destructor is
`memory.deallocate`, and a domain field rejects raw memory. Reinitialization
follows the existing owned-field rule: the field must be moved on every path,
and the replacement must own the same domain.

`aggregate.owned_drop` walks fields in reverse declaration order and, for a
domain field, calls the matching runtime destructor instead of unregistering a
GC root and deallocating. Every destructor accepts null, so untouched `uninit`
slots and moved-out fields are safe. Nested aggregates recurse before the
enclosing field is released; each handle is released once.

## Borrow lifetime propagation (0.415-beta)

A pointer borrow now records the binding it came from. `var p = &owner;` shares the
owner's allocation identity and is marked borrowed rather than becoming an
unrelated raw pointer. Passing `&owner` directly to a `borrow` parameter resolves
the argument through the address-of expression to the same owner.

Each symbol carries its lexical scope depth and, for a borrow, the owner's name. A
Name use, an assignment, or a call argument that forwards a borrow rechecks that
the owner still exists, has not moved, and still has the same allocation identity.
Assigning a borrow to a binding in an outer scope is rejected because the target
would outlive the owner. Borrowed parameters are their own origin inside the
callee, so forwarding a borrowed parameter through nested calls is valid; the
caller-side chain still resolves to the caller's real owner.

`p = nullptr` releases a borrow and remains legal. Rebinding a borrowed pointer to
a different live owner is still rejected conservatively. The rule is lexical and
modular: it does not add caller allocation ids to the callee, does not check
borrowed struct fields, does not implement branch-sensitive owner selection, and
does not change mutable-borrow exclusivity across nested calls. It adds no runtime
state or generated checks.

## Source-bound borrowed returns (0.419-beta)

A function returns a borrow only through an explicit source parameter:
`borrow <source> T* function ...` for an immutable borrow, or
`borrow mut <source> T* function ...` for a mutable one. The declared source must
be a borrowed parameter, and every returned value must resolve to that parameter's
allocation.

Direct returns, forwarding through nested borrowed-return calls, and selecting
one declared source among several parameters are accepted. The caller's real
owner is preserved through the chain. A local owner, a temporary allocation, an
undeclared parameter, a moved owner, and an expired source are rejected. Storing
the returned borrow in a binding that outlives its owner is also rejected.

An owner with a live returned borrow cannot be transferred into an owning pointer
parameter. The checker rejects the call rather than marking the owner moved, so
the borrow and the owner remain consistent. Assignment to an existing pointer also
ensures an address-of origin has a tracked allocation identity before the borrow
is recorded.

Source selection is not branch-sensitive: a return that might choose different
owners on different paths is rejected unless it matches the single declared source.
Borrowed struct fields still lack checked lifetimes, and mutable-borrow exclusivity
across nested calls remains separate work. The whole rule is compile-time only and
adds no runtime counters, locks, descriptors, heap allocation, or generated checks.

## Aggregate-field borrows (0.423-beta)

`&obj.data` and nested projections such as `&obj.inner.data` are tracked as
borrows of `obj` with a dotted field path. The borrow shares the aggregate's
allocation identity, so it cannot outlive the aggregate, survive a move that
invalidates the aggregate, or remain meaningful after the field is replaced.

A direct field borrow can be passed to a `borrow` parameter and returned through
the existing source-bound borrowed-return form. The caller's field path is
propagated through aliases and nested calls. Replacing the borrowed field, an
ancestor of it, or a descendant of it while the borrow is live is rejected. Whole
aggregate replacement is also rejected. Compound writes such as `obj.data += 1`
follow the same rule, while a different sibling field remains writable.

Field borrowing is limited to leaves with numeric, boolean or pointer storage.
Aggregate-valued fields, owned arrays and managed references cannot be addressed.
Branch-selected paths were not yet retained at 0.423; 0.431 preserves them.
Owning aggregates keep the existing broad live-borrow mutation guard, so some
unrelated field writes remain conservatively rejected. No runtime state or checks are added.

## Mutable-borrow exclusivity (0.427-beta)

Borrow conflict queries now carry the requested owner, dotted field path and
mutability. Two immutable borrows of one path may coexist. A mutable borrow
conflicts with any overlapping borrow, and an immutable borrow or read conflicts
with an overlapping mutable borrow. Whole-binding access uses the empty path, so
it overlaps every field of that owner; sibling fields do not overlap each other.

Creating `&obj.field` is rejected while an overlapping mutable borrow is live.
Reading a field, nested field, tuple element, owned array or whole aggregate is
rejected under the same condition. A member access is checked at its leaf path,
not as a whole-value read of the base, so a disjoint sibling remains readable and
writable.

Calls track the borrows claimed by earlier arguments while checking later ones.
Passing two overlapping mutable borrows to one call, or claiming the same alias
twice, is rejected; disjoint field arguments are accepted. Address-of and read
checks inside a later argument also see the earlier claims. Returned mutable
borrows retain mutability when assigned, and forwarding through nested
borrowed-return helpers preserves the caller's field path. Exclusivity ends when
the borrow's lexical scope ends.

At 0.427, conditional branches that assigned different field paths cleared the
cached path to whole-owner access; 0.431 retains the distinct paths instead, and
0.437 keeps a one-sided borrow's path while marking it conditional. The callee
still treats a borrowed parameter as its own origin and does not receive
the caller's allocation id or live-borrow set. Closure capture and general
branch-sensitive analysis remain unimplemented, and no runtime counters, locks,
descriptors, heap allocation, or generated checks are added.

## Branch-selected borrow paths (0.431-beta)

A borrowed binding keeps the set of field paths that can be live after a
conditional merge. One path stays inline; alternatives are stored only when the
branches select different paths, so ordinary borrows keep their compact state.

When both branches leave a live borrow of the same owner, every possible path is
retained. A later conflict query checks all of them: an access overlapping any
possible path is rejected, while a field disjoint from every alternative remains
independently accessible. Nested paths keep their full dotted identity, so
`.inner.a` and `.inner.b` stay distinct even though either overlaps `.inner`.

The merged symbol also retains the shared borrow owner and adopts the allocation
identity both branches agree on. This is required for later queries to match the
owner the paths actually reference. Branches that select different owners mark
the binding unusable. At 0.431 a borrow present on only one branch still fell
back to whole-owner access; 0.437 retains it as a conditional borrow.

Path sets propagate through pointer aliases, mutable borrow arguments, and
source-bound borrowed returns. Returning a borrow from an ambiguous live alias is
rejected rather than creating a second mutable alias. The representation and all
overlap checks are compile-time semantic state only; no runtime tracking,
counters, locks, descriptors, allocations, or generated checks are introduced.

## One-sided conditional borrows (0.437-beta)

When exactly one branch leaves a live borrow, the merge keeps that branch's
owner, mutability, and field path instead of clearing the projection to
whole-owner access. A separate `borrow_conditional` flag records that the borrow
is possible but not present on every path. Conflict queries still include the
symbol, so a later read, write, or borrow overlapping the retained path is
rejected exactly as if the borrow were definitely live; a disjoint sibling field
remains accessible.

Outside a proven non-null guard, a conditional borrow is not a definite borrow.
Aliasing it, taking a new borrow through it, forwarding it to a call, or
returning it produces `borrow of '...' may not be live on every branch`. That
keeps the alias and returned-borrow rules sound without pretending the binding
exists on both paths. Null checks and other ordinary pointer uses remain
available, and an explicit assignment replaces the conditional state with the
new binding's state.

The merge adopts the active branch's allocation identity when the other side is
an ordinary or untracked pointer. If the inactive side owns or shares an
allocation, the binding remains unusable, so two owners are never collapsed into
one. The flag is one boolean on the existing symbol and the path is the existing
inline path, preserving the ordinary single-path fast path. No runtime counters,
locks, descriptors, allocations, or generated checks were added.

## Null-check refinement (0.441-beta)

A conditional borrow can be discharged on a path that explicitly proves the
binding non-null. The merge records whether the path where the borrow is absent
is definitely null; a non-null check can only turn the binding into a definite
borrow when that condition holds. This prevents a non-null pointer to a
different object from being mistaken for the borrow.

Definite nullness is tracked as a small compile-time flag on the symbol. It is
set by a `nullptr` declaration or assignment, cleared by any other pointer
assignment, and merged with logical AND across branches. The merge also records
`borrow_conditional_absent_null` on the one-sided conditional symbol.

Inside `if (view != nullptr)`, or the else path of `if (view == nullptr)`, the
branch state clears `borrow_conditional` when the absent path is null. The
binding keeps its owner, mutability, and field-path set, so nested calls, aliases,
and dereferences are accepted only while that non-null proof holds. The proof is
branch-local: a join restores the conditional flag unless every continuing path
preserves the same null-absence proof, a null path keeps it, and an early-return
guard carries the refined state forward.

Field-path exclusion is unchanged after refinement. A definite conditional
borrow still blocks a later borrow or read of the same path, while disjoint
fields remain available. The refinement adds two booleans to the existing symbol
and no runtime state, counters, locks, allocations, or generated checks.

## Short-circuit null paths (0.445-beta)

The condition analyzer now derives null facts from `&&`, `||`, and logical
negation. It follows the actual evaluation paths: for `A && B` true, both facts
hold; for `A && B` false, a fact is kept only if it is implied by both the
`A`-false path and the `A`-true-then-`B`-false path. `A || B` is dual. This
intersection rule prevents the common mistake of treating the true side of
`||` as though the right operand was always evaluated.

`!` swaps the requested truth value, while nested logical expressions recurse.
The resulting facts update both the nullable-dereference proof set and the
conditional-borrow refinement. A null fact also clears a name from the non-null
proof set, so contradictory nested checks do not grant an unsafe dereference.
Only name-versus-null comparisons contribute facts; arbitrary predicates remain
opaque. The analysis is syntax-local and allocates only small fact sets while
checking branches.

## Branch-dependent owner identity (0.445-beta)

A conditional merge that leaves live borrows of different owners no longer marks
the binding unusable. The primary owner, mutability, and path remain in the
existing symbol fields. Secondary owners are stored as compact `BorrowTarget`
records containing an allocation id, owner name, path set, and mutability. The
vector is empty for ordinary single-owner borrows.

Conflict, live-borrow, field-borrow, moved-allocation, and allocation-status
queries inspect both the primary target and every secondary target. Thus a
later access to either selected owner path is rejected when it overlaps, while
fields disjoint from each owner's path remain available. Moving or releasing
either possible owner conservatively invalidates the binding.

A binding with multiple possible owners cannot be used where one definite owner
must be named: aliasing, calls, address-taking, assignment, and returned
borrows are rejected with `selects between multiple owners`. Direct reads and
dereferences remain valid because every target is a live borrow of the same
value type. The representation remains compile-time only and the ordinary
single-path case adds no allocation.

## Interprocedural borrowed-return effects (0.449-beta)

A borrowed return already names its source parameter. The signature now also
stores the resolved index of that parameter, computed once when the signature
is built. Callers use this compact effect summary when recovering the origin of
a returned borrow, so nested and chained calls do not rescan the parameter-name
list.

When a call that returns a borrow is itself used as a call argument, the
checker walks back through the declared source parameter to the original owner.
It also recovers the field-path set from the source expression. The result is
treated as a real borrow argument: overlapping nested results conflict with a
live borrow or with an earlier argument, while disjoint fields remain usable.
Because only the declared source is stored, compilation remains modular and the
callee does not need the caller's allocation ids.

A stored returned borrow remains an alias of its source. Rebinding or clearing
the binding that produced it does not release other aliases of the same owner,
so stale aliases continue to participate in exclusivity checks.

## Flow-sensitive borrow expiration and rebinding (0.449-beta)

A borrowed binding may be rebound to another validated borrow or cleared with
`nullptr`. Both operations end the old borrow. The replacement's origin must be
live, must not select between multiple owners, and must outlive the target
binding. Its owner/path pair is installed in place of the old one, so a later
borrow of the released field is accepted while a borrow through any remaining
alias is still rejected. Rebinding to a plain owned or raw pointer remains
rejected because that would bypass explicit borrow semantics.

Control-flow merges preserve borrows that may still exist. When one arm clears a
borrow and the other keeps it, the merged symbol retains the active borrow's
owner identity and path even though the inactive arm allocated a fresh pointer
id. This keeps the still-possible borrow visible to conflict checks. When every
continuing arm releases the old field, the merged binding no longer blocks that
field; sibling fields and any newly selected paths remain independently tracked.

The expiration state is entirely compile-time. No runtime borrow counters,
locks, descriptors, allocations, or generated checks are introduced.

## Compact function borrow effects (0.453-beta)

Each function signature carries one effect byte per parameter. Four bits record
whether a borrowed parameter may be read, written, aliased, or escape through a
borrowed return. Declared mutability and return-source information seed the
bits before a body is checked. While checking a body, direct parameter uses and
nested call effects merge into the current summary; after the body is checked,
the signature is cached as complete. Recursive calls reuse the declared summary
and incomplete or unknown callees contribute all four effects conservatively.

At a call boundary, a write effect is treated as an exclusive mutable claim. This
makes forwarded effects participate in the same conflict checks as direct
borrows without receiving the caller's allocation id. Effects are aggregate
per-parameter facts, not a path-sensitive interprocedural analysis, and add one
byte per parameter plus no runtime state.

## Aggregate borrow escape safety (0.453-beta)

A struct initializer records every borrowed field as an owner, allocation id,
path set, and mutability. Nested struct initializers contribute their field
origins transitively. The containing symbol carries those origins as
`field_borrows`, so they can be checked when the aggregate is declared, assigned,
passed to a call, returned, or stored into another aggregate.

A borrowed-field aggregate may be declared where its owner's scope is at least
as deep as the binding's scope. Assigning an inner aggregate to an outer target
is rejected. Returning a borrowed-field aggregate is rejected even when the
borrow is derived from a parameter, and passing one through a call is rejected
because the callee could retain or return it. Aggregate-field storage and
owner replacement are rejected while the field borrow is live.

The metadata is transitive and compile-time only; valid same-scope nested
aggregates and disjoint field paths remain usable. No runtime borrow counters,
locks, descriptors, heap allocations, or generated checks are added.

## Audit hardening (0.457-beta)

Explicitly immutable borrowed pointers and fields are read-only at every write
boundary: direct assignment, indexed assignment, compound assignment, and
address-taking through a dereference. A `borrow mut` field requires a mutable
owner or an already mutable borrowed source. Raw `&` pointers remain distinct
from explicit borrow contracts; their owner lifetime is tracked, but they are not
retroactively treated as immutable references.

A borrowed field cannot be extracted into an independent pointer, because the
result would carry no owner or path metadata. Copying an aggregate that contains
a `borrow mut` field is rejected, as is constructing one aggregate with
conflicting mutable and immutable target paths. Immutable borrowed fields remain
copyable because they carry no write permission.

Branch joins merge initialization conservatively. Uninitialized and partially
initialized paths are retained if either continuing branch could have them, so
a branch-local allocation cannot be overwritten without destruction. Fields are
marked initialized only when both branches agree, and completing a nested
aggregate clears its stale partial marker.

These checks are compile-time-only and add no runtime ownership state.

## Deterministic cleanup (0.461-beta)

Compiler-generated destruction and `defer` share one lexical cleanup stack.
Each automatic entry is recorded when an owned binding is initialized. Explicit
`defer` calls are appended when they are encountered. On scope exit the stack is
consumed from the innermost scope outward and from the last entry to the first,
which is reverse initialization/declaration order. Function parameters are
initialized before the body, so body values are destroyed first and parameters
last, in reverse parameter order.

An explicit defer in the owner's own lexical scope replaces the automatic
cleanup for that owner: both actions run at the same scope exit, so keeping both
would call the same destructor twice. An unconditional defer may also replace an
outer declaration's cleanup. A defer in a conditional scope keeps the outer
automatic fallback; if the branch is not taken, the fallback still destroys the
value, and if it is taken the explicit cleanup clears the slot so the fallback
is a no-op. Moves follow the same rule: a transfer outside any conditional region
proves the owner is gone on every path to its scope exit and removes the dead
automatic action, while a move in a branch or loop body retains the fallback for
the path where the move did not happen.

A `for` initializer has its own cleanup scope. Its values live across the
condition, body, and step, are destroyed on `break` or a false condition at the
loop exit edge, are destroyed by `return`, and remain alive across `continue`.
Body values are destroyed before initializer values. Nested aggregates recurse
through owned fields in reverse field order; an uninitialized or moved field is
zeroed, so its typed destructor receives a null handle and performs no release.

Moving, returning, field-extracting, aggregate-packing, or transferring an owner
after a defer captured it is rejected. The defer argument is evaluated at the
defer statement; allowing a later transfer would give two owners to one
allocation. Reassigning the same binding to a new owner remains valid: the defer
releases the captured value and the automatically tracked binding releases the
new value at scope exit.

GC root frames are still function-activation state. Cleanup runs before every
`return`, and the root frame closes after cleanup on each return edge; loop
`break` and `continue` do not close the activation's frame. Native resources are
released by their typed runtime destructors (`file.close`, `socket.close`,
`mutex.destroy`, mapping/secure release), independently of GC reclamation and
of `nrq_gc_root_unregister` for owned collector storage.

The error/unwind model is not implemented. Its integration point is the same
lexical cleanup stack: a future unwind edge must run the active entries down to
the handler's scope depth before transferring control, in the same reverse order.
No landing pads, exception state, or unwind tables are generated today.

Raw `&` pointers remain a lower-level boundary and are not treated as ownership
actions. Cleanup applies to bindings whose type or producer carries an owned
resource, not to trivial values, borrows, or managed references.
