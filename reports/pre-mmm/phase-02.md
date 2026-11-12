# Pre-MMM Phase 2 - Lifetime propagation and source-bound borrowed returns

**Status:** In progress (lifetime propagation and source-bound borrowed returns complete)
**Version:** 0.419-beta
**Date:** 2026-11-12
**Commits:** `429f01d`, `994e8bd`, `4eba86a`, `9420e6b`

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

## Source-bound borrowed returns (0.419-beta)

A function that returns a borrow must name the parameter it borrows from:
`borrow [mut] <source> T* function ...`. The returned expression is resolved to
its originating owner and checked against that declared source.

- A direct `return source;` keeps the caller's owner identity.
- Forwarding through another borrowed-return call preserves the original owner,
  so nested helpers do not lose the lifetime.
- With multiple parameters, only the declared source may be returned. Returning
  a different borrowed parameter is rejected.
- A local owner, a temporary allocation, a wrong parameter, a moved source, and
  a source that has left its scope are rejected.
- A borrow assigned to an outer binding is rejected because it would outlive its
  owner.
- An owner whose allocation still has a live borrow cannot be transferred into
  an owning pointer parameter. This closes the hole where `take(owner)` could
  run while a borrowed return into `owner` was still live.
- Assignment to an existing pointer now gives an address-of origin a tracked
  allocation identity, so a scalar address cannot lose its lifetime at the
  assignment boundary.

## Tests

- `positive/borrow_lifetime_nested_calls`: a local owner is borrowed with `&`,
  passed through two calls, and read successfully; the same is repeated with a
  direct `&owner` argument. Output: `21`, `21`.
- `negative/borrow_lifetime_escape_call`: a borrowed-return call carries an inner
  block's borrow into an outer pointer. The checker rejects it with
  `borrow of 'inner' cannot outlive its owner`.
- Existing borrow, borrowed-return, array-view, ownership, and nullable pointer
  regressions remain green.
- `positive/borrowed_return_direct_forward_multi`: direct, forwarded, and
  selected-source returns through three helpers; exits with `42`.
- `negative/borrowed_return_local_owner`: `return &local;` is rejected.
- `negative/borrowed_return_temporary_owner`: returning a fresh allocation is
  rejected.
- `negative/borrowed_return_wrong_parameter`: returning the non-source
  parameter is rejected.
- `negative/borrowed_return_moved_source`: a moved source is rejected.
- `negative/borrowed_return_owner_move_call`: transferring an owner while its
  returned borrow is live reports `cannot transfer an owner while a borrow is
  live`.
- `negative/borrowed_return_expired_source`: storing a returned borrow past its
  source's scope reports `borrow of 'inner' cannot outlive its owner`.

## Verification

- Clean `RelWithDebInfo` build at `0.419-beta`.
- Unit suite: 214/214 passed (the previously flaky
  `GcTest.MinorReclaimsOnlyYoungGarbageAndKeepsOldGarbage` passed this run).
- Integration suite: 346/346 passed, including all seven new borrowed-return
  cases.

## Limitations

- Borrowed parameters are validated modularly. The callee does not receive a
  caller allocation id; it treats its parameter as the origin and the call site
  validates the caller's owner.
- Borrowed returns require an explicit source parameter; the checker does not
  infer a source from the function body.
- Source selection is not branch-sensitive. A return that could select between
  different owners on different paths is rejected unless it matches the one
  declared source parameter.
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
