# Managed and explicit memory

Nirqoba currently has explicit raw allocations and a distinct tracing-GC domain.
`array.allocate<T>(count)` creates a managed array and `managed<T>` marks
references for tracing. Cycles are supported, and `gc.collect()` can reclaim
unreachable objects even when automatic collection is disabled. The runtime
starts with automatic collection disabled. Programs that explicitly use the
GC domain can enable automatic collection with `gc.enable()`; explicit
`gc.collect()` always remains available. The Phase 6 Upgrade decision is to make
ownership-based memory the language default. The compiler now tracks pointer ownership, moves, borrows, shared references, and deterministic cleanup for recognized resource producers. GC objects remain explicitly managed and cannot be released through the owned allocator.

The implemented memory domains include owned pointers (move and deterministic cleanup), borrowed function parameters (non-owning and lifetime constrained), explicitly shared references (reference counted), and GC-managed references (traced and opt-in). Raw pointers remain available only within the current low-level subset; full unsafe scopes and aggregate ownership are not implemented.

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


memory.cpu_features() returns a bit mask of CPU features the Windows operating system reports usable by this process: bit 0 is SSE2, bit 1 is AVX, and bit 2 is AVX2. A set bit means the runtime reports support; callers should still use an appropriate target-specific code path and retain a fallback. This API reports only the currently implemented feature set.

ile.open(path, writable) creates an opaque Windows file resource or returns null. Use defer file.close(resource) immediately after opening; ile.write(resource, text) returns a Windows error code (zero on success). The compiler rejects use through a binding after ile.close. The current wrapper supports text writes and does not implement exception unwinding.

memory.thread_count() returns a Windows process-thread snapshot count. Because threads can start or exit during enumeration, treat it as an estimate rather than a synchronization guarantee.

Owned pointer values produced by explicit allocation/resource APIs transfer when bound to another name or passed to a pointer parameter. Pointer return values transfer to the caller. Use `borrow T* name` for a temporary immutable function borrow and `borrow mut T* name` for an exclusive mutable borrow. Mutable borrowing requires a mutable source binding. Borrowed values cannot be freed, closed, passed to an owning parameter, or returned; the compiler rejects conflicting aliases at a call site. `defer` remains the mechanism to ensure explicit cleanup on supported lexical exits.

These rules currently cover pointer bindings and function boundaries. Aggregates, shared references, automatic drop insertion, and general lifetime inference are not yet implemented.

Pointer reassignment follows the same transfer rule: assigning an owned pointer to another mutable binding moves it and invalidates the source. Overwriting a still-live owned pointer is rejected; release it before reusing that binding.

Explicit shared raw allocations use `memory.shared_allocate<T>(bytes)` and
reference management uses `memory.shared_retain(pointer)`,
`memory.shared_release(pointer)`, and `memory.shared_count(pointer)`. Each retain
creates an independently releasable reference; use `defer
memory.shared_release(reference)` when cleanup should happen at lexical scope
exit. Shared references cannot be passed to the unique-owner deallocator or
resized. The runtime serializes reference-count updates, but shared ownership
does not make access to the pointed-to data thread-safe. Shared handles are
currently limited to local pointer bindings; shared function parameters,
returns, aggregate propagation, and automatic drop insertion remain future
work.

A function parameter may declare `shared T* name` to accept one shared reference
by ownership transfer. Calls consume the caller's named handle. Shared handles
cannot be passed to a normal `T*` owning parameter; use `borrow T* name` when a
callee only needs a temporary non-owning view. Shared return types and shared
aggregate fields are not supported yet.

A function that returns a shared reference declares the ownership domain before
the pointer type, for example `shared byte* function create()`. Returning a
shared value transfers one reference to the caller. The compiler records the
return domain on the MLIR function and rejects returning it through a unique
pointer return type.

Raw pointers do not satisfy owning function parameters or pointer return types.
Declare an explicit `borrow` or `borrow mut` parameter for temporary access; a
borrowed or raw pointer cannot escape through a pointer return. Explicit owning
producers and previously tracked owners may be transferred.




`mutex.create()` returns an owned native mutex. `mutex.lock(handle)` blocks until
acquired, and only the owning thread may call `mutex.unlock(handle)`. Mutexes are
non-recursive; re-locking the same mutex from its owner is a runtime error. The
compiler destroys an owned mutex at lexical exit. Unlock it first, directly or
with `defer mutex.unlock(handle)`. Destroying a locked mutex is a runtime error;
mutex handles are not safe to share by copying and require `borrow` when passed
temporarily to a helper.

## Owned arrays and borrowed views

`memory.owned_array<T>(count)` creates one contiguous, uniquely owned buffer.
The owner exposes `.length` and checked indexing and is released automatically
at scope exit. Returning the array or passing it to an `owned_array<T>`
parameter transfers the allocation.

`memory.array_view<T>(source, start, length)` creates a read-only, non-owning
view into an owned array or another view. The runtime checks the requested range
without overflowing. Views expose `.length`, checked indexing, and can be passed
to `array_view<T>` parameters. Views cannot be returned or written through.
While a view binding is live, the compiler rejects moving, releasing, or
mutating its owner. End the view's lexical scope before performing those
operations. This is conservative lexical lifetime checking; the language does
not yet infer arbitrary reference lifetimes or track views stored in aggregates.
