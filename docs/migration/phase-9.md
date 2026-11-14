# Phase 9 — Ownership Hardening Notes

**Version:** `0.515-beta`  
**Scope:** Pre-MMM Phase 9 hardening. No new syntax.

Phase 9 fixes three contracts that Phase 0 identified as prerequisites for the
dedicated memory-model migration. Existing valid programs keep working except for
one storage rule described below.

## 1. Owned-only destruction is collector-free

Destroying an owned aggregate, owned array, owned pointer or borrowed view no longer
unregisters a collector root when the released type contains no managed reference.
This changes generated code only. No source change is required.

Types that do contain a managed reference keep their existing register/unregister
pair. The collector remains required for `managed<T>` and `array.allocate<T>`; it is
not removed or redesigned.

## 2. Secure storage rejects managed fields

`memory.secure_allocate<T>` now rejects a payload that contains a managed reference
at any depth:

```nirqoba
struct Node { managed<Node> next; }

var node = memory.secure_allocate<Node>(64);
// error[NRQ3015]: secure allocations cannot contain a managed field in 'Node':
// secure release has no rooted managed-reference contract
```

Secure storage zeroes and releases raw bytes. It has no rooted allocation path, so a
managed field would not be traced. This rejection is intentional; there is no
fallback to normal allocation or to the collector.

Plain secure payloads remain supported:

```nirqoba
struct Secret { int value; }

var secret = memory.secure_allocate<Secret>(16);
secret.value = 42;
memory.secure_release(secret);
```

Managed allocation remains available through `array.allocate<T>` and the documented
`managed<T>` forms.

## 3. TIR ownership verification

The compiler's internal TIR verifier now checks ownership, release and borrowing
invariants before lowering. This is an internal compiler contract; it adds no syntax
and no runtime checks. Programs that Sema accepts should not observe a difference.

The verifier rejects malformed compiler input such as borrowed values passed to owned
parameters, invalid return ownership, mismatched release families and double
consumption. Passing an owned value to a borrowed parameter remains valid as an
implicit temporary borrow.

## 4. Compatibility

- No new syntax or runtime API.
- Managed fields in secure storage were previously accepted but not rooted; they are
  now compile-time errors.
- The collector remains opt-in and is still available for supported cyclic/dynamic
  object graphs.
- Full verification at this label: 269 unit tests and 500 integration tests pass on
  Windows x64/MSVC (RelWithDebInfo).

## 5. What Phase 9 does not do

Phase 9 does not implement first-class TIR move/drop operations, path-sensitive
destruction proofs, arena objects, secure managed rooting, exception cleanup, owned
strings or FFI/thread ownership. Those remain Phase 1 or later work.
