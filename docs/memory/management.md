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

Explicit allocations use the default general allocator unless the current
thread selects another allocator. These allocations are not visible to the GC.
See [allocator strategies](allocators.md) for selection and lifetime rules.

Owned, borrowed, shared, and deterministic scope cleanup syntax is not yet
implemented. External resources must still be released through their runtime
APIs; GC does not provide timely finalization.
