# GC interoperability (Phase 8)

Phase 7's explicit collector contract is unchanged. `array.allocate<T>` and
`managed<T>` still select collector storage; ordinary, shared, arena, pool and
manual storage do not.

## Manual storage with managed fields

A manual payload containing `managed<T>` keeps its ordinary explicit storage but
registers a typed root while it is live:

```nrqoba
struct Holder {
    managed<int> value;
}

int function main() {
    var holder = memory.manual_allocate<Holder>(8);
    holder[0].value = array.allocate<int>(1);
    holder[0].value[0] = 11;
    gc.collect();
    print(holder[0].value[0]);
    memory.manual_release(holder);
    return 0;
}
```

Lowering registers the root after allocation and unregisters it before
`memory.manual_release` frees the block. A manual payload that requires
deterministic field cleanup is rejected, so release cannot silently skip field
destruction.

## Isolation

Plain manual storage emits no `nrq_gc` IR or object symbol. The Phase 7 isolation
script still checks twelve ordinary fixtures at `-O0` and `-O2`; manual-only
fixtures extend the same rule. A managed control retains GC symbols, so the
check is not vacuous.

## AOR page metadata

Phase 2 of the Adaptive Object Representation work keeps precise collection while
moving mutable state out of the object header. Mark and remembered membership are
page-local bitmaps; young and promotion age stay in the header for now. Ordinary
pages also carry live, live-byte, young, born-this-cycle, marked, marked-young,
remembered, and indexed-object counts. Sweep uses those counts to skip all-live
pages and retire all-dead pages without visiting their headers.

A large page or a page whose directory insertion failed keeps using the exact
object index and the unindexed sweep list. The page metadata is reset before a
recycled page can serve another allocation, and development builds validate the
counts at cycle end. This does not change the collector contract: managed storage
is still precise, non-moving, and separate from ordinary/shared/arena/pool/manual
storage.

## No FFI rooting

A foreign pointer is never automatically registered as a GC root. Because no FFI
surface exists, no foreign call can receive a managed handle, bypass a write
barrier or retain a raw pointer past a root lifetime. These remain unsupported
until an explicit contract is designed.
