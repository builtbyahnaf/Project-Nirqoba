# Pre-MMM Phase 2 - Lifetime propagation across calls

**Status:** In progress (first milestone complete)
**Version:** 0.415-beta
**Date:** 2026-11-12
**Commits:** `429f01d`, `994e8bd`

## Implemented

- Pointer borrows now carry their originating owner. A symbol records the lexical
  scope depth at declaration and, for a borrow, the name of the binding whose
  storage it aliases.
- `&owner` creates a tracked borrow instead of an unrelated raw pointer. The
  alias shares the owner's allocation identity, so deallocation and alias checks
  see one lifetime.
- Borrowed-return declarations retain the caller's owner through the call
  expression, including existing source-bound borrowed returns.
- Borrow arguments are resolved to their original owner at every call. A borrow
  whose owner has left scope, moved, or been replaced is rejected before the call
  is accepted.
- A borrow cannot be stored into a binding that outlives its owner. The checker
  compares the target binding's lexical depth with the owner's depth and rejects
  an outer target.
- `&owner` can be passed directly to a `borrow` parameter; the argument is
  resolved through the address-of expression to the owner.
- `borrow` parameters are their own origin inside the callee, so forwarding a
  borrowed parameter through nested calls remains valid. A caller-side chain
  resolves back to the caller's real owner.
- `p = nullptr` still releases a borrow. Other rebinding of a borrowed pointer
  remains conservatively rejected.

## Rules enforced

1. A borrow is valid only while its originating owner is a live binding in the
   current scope.
2. A borrow may not outlive its owner by being assigned to a binding declared in
   an outer scope.
3. Borrowed arguments may be forwarded through any number of calls; each call
   revalidates the original owner.
4. Borrowing does not transfer ownership. Borrowed values remain non-owning and
   are not cleaned up.
5. Null assignment ends a borrow and is allowed; rebinding to another owner is
   rejected until replacement lifetimes can be proven safe.

## Tests

- `positive/borrow_lifetime_nested_calls`: a local owner is borrowed with `&`,
  passed through two calls, and read successfully; the same is repeated with a
  direct `&owner` argument. Output: `21`, `21`.
- `negative/borrow_lifetime_escape_call`: a borrowed-return call carries an inner
  block's borrow into an outer pointer. The checker rejects it with
  `borrow of 'inner' cannot outlive its owner`.
- Existing borrow, borrowed-return, array-view, ownership, and nullable pointer
  regressions remain green.

## Verification

- Clean `RelWithDebInfo` build.
- Unit suite: 214/214 passed on the full rerun. The first post-clean run
  segfaulted in the known flaky
  `GcTest.MinorReclaimsOnlyYoungGarbageAndKeepsOldGarbage`.
- Integration suite: 339/339 passed.

## Limitations

- Borrowed parameters are validated modularly. The callee does not receive a
  caller allocation id; it treats its parameter as the origin and the call site
  validates the caller's owner.
- Borrowed struct fields still do not carry a checked lifetime. Borrowing from
  aggregate fields is a separate Phase 2 item.
- Mutable-borrow exclusivity across nested calls is unchanged and remains a
  separate Phase 2 item.
- Safe rebinding of a borrowed pointer to a different live owner is still
  rejected conservatively.
- Branch-sensitive owner selection, closure capture, and general escape analysis
  are not implemented.
- The work adds no runtime counters, locks, descriptors, heap allocation, or
  generated checks; borrowing remains compile-time only.
