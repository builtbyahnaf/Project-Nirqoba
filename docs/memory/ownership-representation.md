# Ownership Representation — Phase 1

**Version:** `0.539-beta`  
**Scope:** Memory Model Migration Phases 1–2. No new language syntax.

Phase 1 gave the ownership semantics Nirqoba already enforced in semantic
analysis an explicit, verifiable representation in TIR. Phase 2 makes that
representation operational: `tir.drop` now generates the release, owner
overwrites destroy the old value first, obligations must resolve, and explicit
`defer`/release paths participate in the same bookkeeping. Full borrow
enforcement remains Phase 3.

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

The cleanup is one operation that owns its release:

```text
%20 = tir.load %4
tir.drop %20 obligation=0 release=memory.deallocate
```

Lowering expands that drop into the `memory.deallocate` call on a copy of the
function. Verified TIR therefore has one destruction record, and MLIR still
receives an ordinary runtime release. A separate release call is only emitted
when the source explicitly writes one (`memory.deallocate(x)` or `defer ...`).

Verifier rules:

- a drop operand must be owning and may not be borrowed;
- its obligation id must exist in the function table;
- the drop type and release must match the registered obligation, and the
  release family must match the type;
- an obligation may be discharged at most once;
- a transferred obligation may not also be dropped;
- a drop must name a release and may not be immediately followed by a duplicate
  release of the same value;
- every obligation must end discharged or transferred;
- a value may not be used after its drop on the same straight-line path.

### 4.1 Destruction order

Lexical cleanup runs in reverse registration order (LIFO). Parameters are
registered before the body, so body owners are destroyed before parameters.
Nested aggregate fields are released in reverse declaration order by
`aggregate.owned_drop`. A moved field or whole value is null, so recursive
destruction treats it as a no-op.

### 4.2 Control flow

Every terminator closes the scopes it leaves: `return` closes all scopes, `break`
closes loop-body scopes, and `continue` closes the body scope before the step.
Conditional releases and conditional moves leave the automatic cleanup armed
because another path may still own the value. Assigning a replacement into such
a slot first emits the pending destruction, which releases the live value on the
path that still owns it and releases null on the path that already moved it.

### 4.3 Partial initialization

An `uninit` aggregate is zero-initialized. Field initialization writes only the
selected fields, and recursive destruction releases the initialized fields while
the zeroed fields are no-ops. No per-object initialization bitmap is needed for
the aggregate forms the language supports.

### 4.4 Explicit cleanup

An explicit `memory.deallocate`, resource release or `defer` destroys the value
itself, so it discharges the obligation rather than transferring it. A binding
whose cleanup is scheduled by `defer` cannot be overwritten, because the
deferred call expects to release the original value.

Exception and panic unwinding are not part of the language. `system.exit`
terminates the process and does not run lexical cleanup; runtime teardown only
retires process-owned resources on a best-effort basis. These are documented
limitations, not supported cleanup paths.

The verifier is deliberately structural and straight-line: it proves each emitted
destruction is well formed and every obligation resolves, but it does not prove
path-sensitive completeness for arbitrary branch graphs. The generated builder
cleanup covers the supported control flow; richer path proofs belong to a later
analysis pass.

## 5. Transformation contract

Ownership metadata survives translation to MLIR:

- each value keeps `nirqoba.ownership`;
- functions keep `nirqoba.return_ownership`, `nirqoba.return_resource_domain`
  and `nirqoba.return_borrow_parameter`;
- functions gain `nirqoba.lifetime_relations`, a compact summary such as
  `parameter:value:immutable;return:value:immutable`;
- functions with cleanup gain `nirqoba.destruction_obligations`.

`tir.move`, `tir.borrow` and `tir.end_borrow` lower without runtime cost. They
participate in the stack-promotion escape scan as observers, not escapes, so the
optimisation keeps working. `tir.drop` lowers to exactly the release call named
by its obligation; it does not add a registry, a GC root, a safepoint or a write
barrier.

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

Phases 1–2 do not add:

- move or destruction syntax;
- exception or panic unwinding cleanup;
- full path-sensitive obligation proofs for arbitrary branch graphs;
- lifetime inference or full borrow enforcement;
- arena allocation or weak references;
- ordinary-allocation migration (Phase 4) or GC changes.

## 8. Compatibility

No source program changes meaning. The only observable change is that malformed
TIR produced by a compiler bug is rejected with an ownership diagnostic instead
of being lowered. Existing valid programs, including all GC programs, continue
to compile and run.
