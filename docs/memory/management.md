# Managed and explicit memory

Nirqoba currently has two distinct allocation paths. `array.allocate<T>(count)`
creates a managed array on the tracing garbage collected heap. `managed<T>`
marks references that the collector must trace. Cycles are supported, and
`gc.collect()` can reclaim unreachable objects even when automatic collection
is disabled. Automatic collection is enabled by default. Disabling it stops
automatic triggers; it does not stop allocation or explicit collection.

`gc.get_statistics()` returns `allocated_bytes`, `collected_bytes`,
`collections`, and `live_objects`. The collector is non-moving. It discovers
references from compiler registered roots and typed object layouts instead of
scanning arbitrary integer data.

Use `memory.allocate<T>(bytes)` for explicitly managed raw memory and pair it
with `memory.deallocate(pointer)`. A failed allocation returns null. The
runtime tracks allocation ownership and rejects invalid and repeated frees.
Raw pointer dereferences after free are not dynamically checked; callers must
keep the allocation alive and avoid using pointers after deallocation.

Explicit allocator profiling can be enabled with
`memory.profiler.start()`, sampled with `memory.profiler.capture()`, and
disabled with `memory.profiler.stop()`. The report covers explicit allocator
allocations only. Secure buffers can be created with
`memory.secure_allocate<T>(bytes)`, cleared with `memory.secure_zero`, and
released with `memory.secure_release`; release zeroes the tracked allocation
before returning it to its allocator.

Managed GC objects are non-moving. `memory.pin(object)` temporarily adds a GC
root and `memory.unpin(object)` removes it. Always unpin when native use ends.
This API does not create a scope guard. File mappings are explicitly released
with `memory.unmap_file`; `memory.mapping_size` reports their length, but
ordinary pointer indexing does not enforce mapping bounds.

`memory.mapped_read<T>(mapping, byte_offset)` and
`memory.mapped_write<T>(mapping, byte_offset, value)` validate the requested
range and stop with a runtime diagnostic when it falls outside the mapping.
`memory.start_ref_count()`, `memory.get_ref_count(object)`, and
`memory.stop_ref_count()` enable on-demand reference diagnostics; these counts
describe observed references and are not ownership counts.

`memory.available_bytes()` reports an estimate from Windows physical memory
status. It does not reserve memory.

Explicit allocations use the default general allocator unless the current
thread selects another allocator. These allocations are not visible to the GC.
See [allocator strategies](allocators.md) for selection and lifetime rules.

Owned, borrowed, shared, and deterministic scope cleanup syntax is not yet
implemented. External resources must still be released through their runtime
APIs; GC does not provide timely finalization.
