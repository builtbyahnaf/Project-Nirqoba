# Manual ownership (Phase 8)

`memory.manual_allocate<T>(bytes[, allocator])` creates a `Manual` owner.
`memory.manual_release(pointer)` is its only release.

```nrqoba
int function main() {
    var value = memory.manual_allocate<int>(4);
    *value = 41;
    print(*value);
    memory.manual_release(value);
    return 0;
}
```

## Rules

- **Distinct category.** `Manual` is owning, transferable and requires
  destruction, but it is not `Owned`, `Raw`, `Shared`, `ArenaBound` or
  `GcManaged`.
- **Allocator identity.** The allocator selected by the call or passed as the
  second argument is recorded by the runtime ownership registry. A release
  returns the block to that allocator. The payload type supplies the natural
  alignment.
- **Exactly once.** A move invalidates the source binding. Releasing with a live
  borrow, releasing twice, or using the pointer after release is rejected by
  Sema's tracked-binding rules.
- **No implicit conversion.** Ordinary deallocation, secure release, shared
  release and resource close calls reject manual storage; manual release rejects
  every other family. The TIR verifier enforces the same pairing.
- **Fallback.** If the programmer does not release explicitly, lexical cleanup
  emits `memory.manual_release`. This is a safety fallback, not a conversion to
  `Owned`.
- **Function-local.** There is no manual pointer type, so manual storage cannot
  cross an owned parameter or return. Use a `borrow` parameter for temporary
  access, or release before the call.
- **No reallocation.** `memory.reallocate` rejects manual storage. Release and
  allocate a replacement.
- **No bulk regions.** Tracked `memory.arena`/`memory.pool` handles are rejected;
  those lifetimes are owned by reset/destroy or `pool_release`.
- **No deterministic field cleanup.** A manual payload whose fields require
  destruction is rejected because manual release frees storage without running
  field destructors.
- **Managed fields.** A payload containing `managed<T>` uses the typed-root
  registration path. Plain manual storage has no GC dependency.

## Unsupported

Manual storage in aggregates is rejected by the existing aggregate-field rule
(an `owned`/`shared`/`borrow` field accepts only the matching ownership form).
Arbitrary manual alignment, manual reallocation, manual weak/shared conversions,
cross-function manual pointers and FFI transfer are not implemented.
