# Managed and explicit memory

Nirqoba currently has explicit raw allocations and a distinct tracing-GC domain.
`array.allocate<T>(count)` creates a managed array and `managed<T>` marks
references for tracing. Cycles are supported, and `gc.collect()` can reclaim
unreachable objects even when automatic collection is disabled. The runtime
starts with automatic collection disabled. Programs that explicitly use the
GC domain can enable automatic collection with `gc.enable()`; explicit
`gc.collect()` always remains available. The Phase 6 Upgrade decision is to make
ownership-based memory the language default. The compiler now tracks pointer ownership, moves, borrows, shared references, and deterministic cleanup for recognized resource producers. GC objects remain explicitly managed and cannot be released through the owned allocator.

The implemented memory domains include owned pointers and arrays (move and deterministic cleanup), borrowed pointer parameters and array views (non-owning and lifetime constrained), explicitly shared references (reference counted), and GC-managed references (traced and opt-in). Raw pointers remain available in the current low-level subset; full unsafe scopes and ownership of resource-bearing aggregate fields are not implemented.

`gc.get_statistics()` returns `allocated_bytes`, `collected_bytes`,
`collections`, and `live_objects`. The collector is non-moving. It discovers
references from compiler registered roots and typed object layouts instead of
scanning arbitrary integer data.

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
See [allocator strategies](allocators.md) for selection and lifetime rules.

`defer call(...)` captures call arguments when registered and runs calls in
LIFO order when a lexical block exits, including return, break, and continue.
It can release explicit allocations and restore a prior allocator selection.
Recognized owned pointers and resources are cleaned automatically at lexical exit, including return, break, and continue. Explicit `defer` may select a custom cleanup call and overrides the inferred automatic cleanup for that resource. Cleanup on exception unwinding remains unavailable because the language has no exception model. GC does not provide timely finalization.


memory.cpu_features() returns a bit mask of CPU features the Windows operating system reports usable by this process: bit 0 is SSE2, bit 1 is AVX, and bit 2 is AVX2. A set bit means the runtime reports support; callers should still use an appropriate target-specific code path and retain a fallback.

`file.open(path, writable)` creates an owned Windows file resource or returns null. The wrapper supports text and byte reads and writes. Use `defer file.close(resource)` when explicit close timing is needed; otherwise the compiler closes an owned file at lexical exit. Ownership moves invalidate the source binding. Use after move or close is rejected.

`memory.thread_count()` returns a Windows process-thread snapshot count. Because threads can start or exit during enumeration, treat it as an estimate rather than a synchronization guarantee.

## Ownership, borrows, and cleanup

Owned values from supported allocation and resource producers transfer when bound to another name, passed to an owning parameter, or returned through a matching owner return type. Tracked owners are cleaned in LIFO order at lexical exit, including return, break, and continue. Owner moves clear the source slot so generated cleanup remains safe across conditional control flow. Reassigning a live owner is rejected; release it before reusing the binding.

Use `borrow T* name` for an immutable pointer borrow and `borrow mut T* name` for an exclusive mutable pointer borrow. Borrows cannot be freed, closed, returned, or passed to owning parameters. The checker rejects conflicting aliases at a call site. Borrowed array views are described below. Borrow analysis is conservative and lexical; general returned references, closure capture, arbitrary interprocedural lifetimes, and path-sensitive aggregate aliases are not implemented.

`defer call(...)` captures call arguments when registered and runs calls in LIFO order on lexical block exit, including return, break, and continue. It can release explicit allocations, unpin GC objects, unlock mutexes, close files, and restore allocator selection. An explicit deferred cleanup overrides the inferred cleanup for that owner. Cleanup during exception unwinding is unavailable because the language has no exception model.

## Shared references

`memory.shared_allocate<T>(bytes)` creates a reference-counted allocation. `memory.shared_retain(pointer)` creates another release token, `memory.shared_release(pointer)` consumes one token, and `memory.shared_count(pointer)` reports the current count. Use `defer memory.shared_release(reference)` for explicit lexical cleanup; shared parameters are released automatically when the callee exits.

A function parameter may declare `shared T* name`, and a function returning a shared pointer declares `shared T* function create()`. Calls transfer one token into or out of the function. Shared pointers cannot be passed to unique-owner parameters or released with unique deallocation APIs. Reference-count updates are synchronized; access to the pointed-to data is not made thread-safe. Shared references in aggregates are not supported.

## Native mutex resources

`mutex.create()` returns an owned native mutex. `mutex.lock(handle)` blocks until acquired, and only the owning thread may call `mutex.unlock(handle)`. Mutexes are non-recursive. The compiler destroys the owned mutex at lexical exit; use `defer mutex.unlock(handle)` so unlock runs before automatic destruction. Destroying a locked mutex, recursive acquisition, and unlocking from another thread are runtime errors. Pass a mutex handle through a borrow parameter when a helper only uses it temporarily.

## Owned arrays and borrowed views

`memory.owned_array<T>(count)` creates one contiguous, uniquely owned buffer. `memory.owned_array_aligned<T>(count, alignment)` requests stronger power-of-two alignment, clamped to the element type's natural alignment. Allocation uses the selected explicit allocator. The owner exposes `.length` and checked indexing and is released automatically at scope exit. Returning the array or passing it to an `owned_array<T>` parameter transfers the allocation.

When an owned array element type contains GC-managed references, the runtime registers the backing storage as a precise repeated-layout root region. Dropping the array unregisters that region before freeing the explicit buffer. The buffer remains explicitly owned; GC traces referenced managed objects without managing the buffer allocation itself.

`memory.array_view<T>(source, start, length)` creates a read-only, non-owning view into an owned array or another view. The runtime checks the range without overflowing. Views expose `.length`, checked indexing, and can be passed to `array_view<T>` parameters. Views cannot be returned or written through. While a view binding is live, the compiler rejects moving, releasing, or mutating its owner. End the view's lexical scope before performing those operations. View lifetime tracking is conservative and lexical; views cannot be stored in aggregates.

Aggregate fields that own resources, partial moves, closure capture, exception-unwind cleanup, GPU buffers, and non-Windows runtime support remain future work.
