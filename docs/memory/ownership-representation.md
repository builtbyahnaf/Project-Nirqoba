# Ownership Representation — Phase 1

**Version:** `0.531-beta`  
**Scope:** Memory Model Migration Phase 1. No new language syntax.

Phase 1 gives the ownership semantics Nirqoba already enforced in semantic
analysis an explicit, verifiable representation in TIR. It does not implement the
move/destruction execution rules of Phase 2 or the full borrow enforcement of
Phase 3. The compiler still lowers the same programs to the same machine code;
what changes is what the compiler can see and reject internally.

## 1. Ownership categories

`ValueOwnership` is the single vocabulary shared by Sema, TIR, lowering and the
verifier:

| Category | Meaning | Destructor |
|---|---|---|
| `Trivial` | No ownership semantics (integers, booleans, plain aggregates) | None |
| `Owned` | Deterministic owner; exactly one release is owed | Required |
| `Borrowed` | Immutable non-owning access bound to a source | None |
| `MutableBorrowed` | Exclusive non-owning access bound to a source | None |
| `Shared` | Reference-counted owner | Release once |
| `GcManaged` | Collector-owned reference | Collector |
| `Raw` | Raw/manual pointer with no compiler-managed lifetime | Caller contract |
| `ArenaBound` | Lifetime owned by an enclosing arena (Phase 5) | Arena reset |
| `Weak` | Non-owning weak reference (Phase 6) | None |

The helpers `is_owning_ownership`, `is_borrowing_ownership`,
`is_transferable_ownership`, `requires_destruction` and `ownership_name` keep
callers from re-deriving these rules differently. `ArenaBound` and `Weak` are
representation categories only: no current builder path produces them.

A category is stored on every TIR value, parameter and return. The verifier
rejects a category that cannot describe its type, for example an `Owned`
integer or a `Borrowed` struct value owned by a slot.

## 2. Ownership transfer

A transfer is a `tir.move` operation:

```text
%7 = tir.move %6 -> %7 kind=return
```

`%6` is the value being transferred, `%7` is the new responsible value, and the
reason is one of `local`, `return`, `argument`, `field` or `element`. Lowering
treats the result as an alias of the source; a move is never a physical copy.
The existing null-store that invalidates the source slot remains, because it is
the execution mechanism. The move operation is what makes the transfer
checkable.

Rules enforced by the verifier:

- the source category must be transferable (`Owned` or `Shared`);
- source and result types must match, and result ownership must equal source
  ownership;
- the result must be a distinct value number;
- the source may not be used after the move;
- a value may not be moved twice.

## 3. Borrowing and lifetimes

`&x` emits an address-of operation followed by a borrow record:

```text
%11 = tir.address_of %3
tir.borrow %11 from %3 mutable path="p.x"
```

The borrowed value carries `Borrowed` or `MutableBorrowed` ownership. The
operation names the owner's storage and the selected field path. A
`LifetimeRecord` with kind `Lexical` is added to the function, so a
transformation that does not understand the operations can still recover the
relationship.

Borrows created in a scope are ended by `tir.end_borrow` at scope exit, before
the owner's cleanup. A borrow returned to the caller is deliberately left open;
its lifetime is recorded on the function as a `Return` relation naming the source
parameter. A borrowed parameter carries a `Parameter` relation.

Verifier rules:

- the borrowed value must have borrowed ownership, and mutability must agree
  with the category;
- the borrow must originate from `address_of`;
- one borrowed value cannot own another borrow;
- a borrow needs a matching lexical lifetime record;
- `end_borrow` must name a live borrow and may not end it twice;
- parameter and return lifetime records must name borrowed parameters.

This is representation and structural checking. Full lifetime inference,
exclusivity enforcement across all control flow and escape analysis remain
Phase 3.

## 4. Destruction obligations

When an owning local or parameter is registered for lexical cleanup, TIR records
a `DestructionObligation`:

| Field | Meaning |
|---|---|
| `id` | Stable function-local identity used by `tir.drop` |
| `owner` | The storage slot whose value must be released |
| `type` | Type of the owned value |
| `release` | Release operation that discharges the obligation |
| `discharged` | A drop and release pair ran |
| `transferred` | Ownership moved, so the new owner owes the release |

The cleanup emits the release call it always did, preceded by a drop:

```text
%20 = tir.load %4
tir.drop %20 obligation=0 release=memory.deallocate
tir.call @memory.deallocate(%20)
```

Verifier rules:

- a drop operand must be owning and may not be borrowed;
- its obligation id must exist in the function table;
- the drop type and release must match the registered obligation, and the
  release family must match the type;
- an obligation may be discharged at most once;
- a transferred obligation may not also be dropped;
- every drop must be immediately followed by the release call it names.

The verifier does not yet require every obligation to be discharged on every
path. That is Phase 2's path-sensitive destruction proof. It does guarantee that
no metadata claims a cleanup that lowering does not perform.

## 5. Transformation contract

Ownership metadata survives translation to MLIR:

- each value keeps `nirqoba.ownership`;
- functions keep `nirqoba.return_ownership`, `nirqoba.return_resource_domain`
  and `nirqoba.return_borrow_parameter`;
- functions gain `nirqoba.lifetime_relations`, a compact summary such as
  `parameter:value:immutable;return:value:immutable`;
- functions with cleanup gain `nirqoba.destruction_obligations`.

`tir.move`, `tir.borrow`, `tir.end_borrow` and `tir.drop` lower without runtime
cost. They participate in the stack-promotion escape scan as observers, not
escapes, so the Phase 2 escape optimisation keeps working.

## 6. Diagnostics

| Code | Meaning |
|---|---|
| `NRQ4006` | Invalid ownership transfer or ownership metadata |
| `NRQ4007` | Invalid borrow or lifetime relationship |
| `NRQ4008` | Invalid destruction obligation or drop/release pairing |

These are internal compiler errors. A user-visible ownership violation is still
reported by Sema as `NRQ3015`/`NRQ3033` with the source location of the
offending expression.

## 7. Non-goals

Phase 1 does not add:

- move syntax or implicit move semantics;
- deterministic destruction order or exception cleanup;
- path-sensitive obligation proofs;
- lifetime inference or full borrow enforcement;
- arena allocation or weak references;
- GC changes. Managed allocation and collection are untouched.

## 8. Compatibility

No source program changes meaning. The only observable change is that malformed
TIR produced by a compiler bug is rejected with an ownership diagnostic instead
of being lowered. Existing valid programs, including all GC programs, continue
to compile and run.
