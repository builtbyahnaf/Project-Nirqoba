# Managed and explicit memory

Nirqoba currently has explicit raw allocations and an opt-in tracing-GC domain.
`array.allocate<T>(count)` creates a managed array and `managed<T>` marks
references for tracing. Cycles are supported, and `gc.collect()` can reclaim
unreachable objects even when automatic collection is disabled. The runtime
still enables automatic collection by default for compatibility. The Phase 6
Upgrade decision is to make ownership-based memory the language default. This
is a migration target; ownership analysis is not yet implemented. GC objects
remain explicitly managed and must not be silently treated as owned allocations.

The target memory domains are owned values (move and deterministic drop),
borrowed references (non-owning and lifetime checked), explicitly shared
references (reference counted), GC-managed references (traced, opt-in), and
raw/unsafe pointers. Until syntax is specified and the compiler enforces these
rules, programs must use the existing allocation APIs and `defer` cleanup. No
lifetime or use-after-move safety is implied by this target design.

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
Owned/borrowed/shared types, native resource wrapper types, and exception-unwind
cleanup remain unimplemented: the roadmap requires their syntax to be settled in
the language specification first, and this document currently defines no such
syntax. GC does not provide timely finalization.
