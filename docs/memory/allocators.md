# Explicit allocator strategies

Create an allocator, select it for explicit allocations on the current thread,
then restore the previous handle before destroying it. `defer` makes the
selection local to a lexical block:

```nirqoba
let pool = memory.pool_allocator(64);
{
    let previous = memory.use_allocator(pool);
    defer memory.use_allocator(previous);
    let item = memory.allocate<byte>(8);
    defer memory.deallocate(item);
}
memory.destroy_allocator(pool);
```

`memory.allocate<T>(bytes, allocator_handle)` can override the current
selection for a single allocation without changing thread state.

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
`memory.reset_allocator(handle)` resets its storage only after all allocations
from that allocator have been released. It fails while allocations remain live
or while an allocation from that allocator is still being created on another
thread. `memory.destroy_allocator(handle)` fails while allocations remain live
or the allocator is selected.

General allocation supports arbitrary valid power-of-two alignments. Pool and
slab are intended for repeated small requests, arenas for bulk lifetimes,
stacks for strict LIFO lifetimes, buddy for power-of-two blocks, and free-list
or TLSF for variable-size reuse. Stack allocations must be freed in LIFO order.
These strategies are correctness choices, not measured performance claims.

Selection is thread-local by default. Per-allocation selection is supported;
function and block selection can be paired with `defer` to restore the prior
handle. `memory.set_default_allocator(handle)` installs a process-wide default
for allocations made without a per-allocation handle and returns the previous
default; thread selection still takes precedence, and zero restores the
general-purpose default. `memory.get_default_allocator()` reads it. Selection
for collector-managed containers is not implemented, because those arrays are
owned by the tracing heap rather than by an explicit allocator.

`memory.reallocate<T>(pointer, new_bytes)` resizes an explicit allocation. The
block keeps its allocator and alignment, and the surviving bytes are copied up
to the smaller of the two sizes. Growth of the most recent arena or stack block
resizes in place; every other strategy allocates a replacement and releases the
original. A stack allocator only resizes its most recent block, because freeing
any other block would break its LIFO order, and returns null instead. A null
result means the request failed and the original allocation is still valid. A
byte count of zero frees the block and also returns null, following the C
`realloc` convention. The previous pointer must not be used after a successful
call.

Native embedders can register a callback allocator through
`nrq_allocator_create_custom(const NrqCustomAllocatorVTable*)` in the runtime C
header. The runtime copies the callback table and associates every returned
pointer with that allocator. The deallocation callback receives the original
size and alignment. Allocation returns null on failure. The runtime serializes
callbacks under its allocator registry lock, so callbacks must not re-enter
allocator APIs. The optional destroy callback runs when the allocator handle is
destroyed. Custom allocators cannot currently be reset or declared directly in
Nirqoba source, and container integration is not supported;
`memory.reallocate` reaches them through the generic allocate-copy-free path.

Use `memory.profiler.start()`, `.capture()`, and `.stop()` to track explicit
allocator memory. Secure buffers allocated through `memory.secure_allocate`
are also tracked and can be released with the zeroing
`memory.secure_release` API.
