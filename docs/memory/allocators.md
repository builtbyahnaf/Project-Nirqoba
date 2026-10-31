# Explicit allocator strategies

Create an allocator, select it for explicit allocations on the current thread,
then restore the previous handle before destroying it:

```nirqoba
let pool = memory.pool_allocator(64);
let previous = memory.use_allocator(pool);
let item = memory.allocate<byte>(8);
memory.deallocate(item);
memory.use_allocator(previous);
memory.destroy_allocator(pool);
```

The available constructors are `memory.general_allocator()`,
`memory.pool_allocator(capacity)`, `memory.arena_allocator(capacity)`,
`memory.stack_allocator(capacity)`, `memory.slab_allocator(capacity)`,
`memory.buddy_allocator(capacity)`, `memory.free_list_allocator(capacity)`,
and `memory.tlsf_allocator(capacity)`. Handles are opaque integers. Capacity
is measured in bytes. Pool capacity is the maximum supported request class;
slab chooses size classes as needed. Arena, stack, buddy, free-list, and TLSF
use a backing region (with a minimum region size).

`memory.get_allocator()` returns the selected handle, or zero when the default
allocator is selected. `memory.use_allocator(handle)` returns the previous
selection. Passing zero restores the default. Selection is thread-local.
`memory.reset_allocator(handle)` invalidates that allocator's outstanding
allocations and resets its storage. `memory.destroy_allocator(handle)` fails
while allocations remain live or the allocator is selected.

General allocation supports arbitrary valid power-of-two alignments. Pool and
slab are intended for repeated small requests, arenas for bulk lifetimes,
stacks for strict LIFO lifetimes, buddy for power-of-two blocks, and free-list
or TLSF for variable-size reuse. Stack allocations must be freed in LIFO order.
These strategies are correctness choices, not measured performance claims.

The current language API selects allocators per thread. Per-allocation,
collection, function, and scope selection are not implemented. User-defined
allocator implementations and reallocation are also not implemented.
