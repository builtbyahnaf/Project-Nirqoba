# Migration Note — Phase 3: Borrowing and Lifetime Enforcement

**Version:** `0.547-beta`  
**Scope:** MMM Phase 3. No new syntax and no runtime borrow tracking.

Phase 1 recorded borrows and lifetimes in TIR; Phase 2 made moves and destruction
executable. Phase 3 makes the borrow rules enforceable over control flow and
across functions.

## What is now enforced

- Multiple immutable borrows may coexist. A mutable borrow is exclusive against
  every overlapping active borrow.
- `&` passed to an immutable `borrow` parameter is a read-only borrow; the
  callee contract narrows the permission. A `borrow mut` parameter keeps
  exclusivity.
- A live borrow keeps its owner from being moved, released or overwritten.
  Overwrites of disjoint fields remain valid.
- A borrow cannot outlive its owner. A return must name the declared source
  parameter; a local or aggregate borrow cannot escape.
- Branches, merges, loop back-edges and early returns are analyzed over the TIR
  control-flow graph, not with a straight-line scan.
- A borrow restriction lasts while one of its aliases is live. Rebinding or
  overwriting a pointer releases the old restriction without ending the owner.

## What changed for existing programs

Most valid programs are unchanged. Two previously over-rejecting cases now work:

1. Two `&owner` arguments to immutable borrow parameters are compatible instead
   of being treated as two mutable borrows.
2. Taking a scalar's address inside a loop no longer marks the scalar moved after
   the loop.

Programs that relied on an invalid borrow are now rejected with `NRQ3015` and a
note at the conflicting borrow. Internal malformed TIR reports `NRQ4007`.

## What did not change

- No `&mut`/`&` syntax redesign and no user lifetime annotations.
- No runtime borrow flags, allocation, reference counting, GC roots, safepoints
  or write barriers.
- `tir.borrow` and `tir.end_borrow` remain zero-cost markers; `tir.move` remains a
  zero-copy alias; `tir.drop` still lowers to the same release call.
- Ordinary allocation remains GC-backed. Phase 4 and later work were not started.

## Limits

The TIR alias analysis covers address-of values, local copies, loads from
bindings and borrowed call results. It does not model arbitrary pointer
arithmetic or aliases created inside foreign calls. Borrowed containers remain
limited to the existing array-view forms. Where the analysis cannot prove a
borrow dead, it conservatively keeps the restriction.

## Verification

`307` unit tests and `535` full-run cases pass. The new `tests/memory_model`
borrow cases cover immutable/mutable access, branch merges, loop scopes,
disjoint fields, forwarding, parameter-bound returns, recursion and the
corresponding rejection cases.
