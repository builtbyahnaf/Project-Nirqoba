# Memory Model Migration — Phase 3 Report

## 1. Phase Overview

- **Phase identifier:** MMM Phase 3
- **Phase title:** Borrowing and Lifetime Enforcement
- **Status:** Complete
- **Starting version:** `0.539-beta`
- **Final version:** `0.547-beta`
- **Objective:** Turn the Phase 1 borrow/lifetime representation and the Phase 2
  ownership rules into enforced, control-flow-sensitive borrowing: immutable and
  mutable borrows, owner validity, source-bound returns, escapes and lifetime
  relationships.

Phase 3 adds no syntax. Existing `borrow` parameters, `borrow mut` parameters,
borrowed fields, `borrow <source> T*` returns, `&` and `*` keep their meaning.
The work is semantic enforcement, a CFG verifier and test/documentation coverage.

## 2. Borrowing Semantics

### 2.1 Immutable borrows

- Several immutable borrows may be active at the same time.
- An immutable borrow cannot write through the borrow and cannot be passed to a
  mutable-borrow parameter.
- The owner cannot be moved, destroyed or overwritten while a live immutable
  borrow depends on it.
- Ending an immutable borrow removes only the read restriction; the owner stays
  alive and keeps its own cleanup.

`&` is permitted to create a borrow, but the **callee contract narrows it**. An
address passed to a `borrow` parameter is a read-only borrow even though the
caller wrote `&`; an address passed to a `borrow mut` parameter stays exclusive.
This is what allows two immutable arguments of one owner while mutable calls
remain exclusive.

### 2.2 Mutable borrows

- A mutable borrow is exclusive: it conflicts with any other overlapping borrow,
  immutable or mutable.
- Reads and writes through the mutable borrow are allowed; reads or writes
  through the owner while it is mutably borrowed are rejected.
- Moving, releasing or overwriting the owner during a mutable borrow is rejected.
- A mutable borrow can be forwarded to another mutable-borrow parameter.
- A mutable borrow cannot be widened into an owning value or silently narrowed
  into an immutable return.

### 2.3 Borrow paths and aggregate fields

Borrows carry a dotted path (`p.x`, `values[]`, empty for a whole binding).
Overlap follows storage containment: a whole binding overlaps every field, an
ancestor overlaps its descendants, and sibling fields are disjoint. This keeps
`p.y = 5` legal while `&p.x` is live, while writes to `p` or `p.x` are rejected.
Aggregate construction records borrowed-field origins; the aggregate inherits
the lifetime of every borrowed field it stores.

## 3. Lifetime Model

Existing lifetime records are retained and strengthened:

- `Lexical`: a borrow ends with its lexical region. The record names the borrow
  value, owner storage, path and mutability.
- `Parameter`: a borrowed parameter's lifetime belongs to the caller. The record
  names the parameter.
- `Return`: a returned borrow is bound to the declared `borrow <source>` return
  parameter. A local borrow cannot be returned; an aggregate holding a borrow
  cannot escape through a return.

Verification requires the lifetime table and the `tir.borrow` operation to agree
on source, path and mutability. A borrow has exactly one lexical record, and a
lexical record without a borrow operation is stale and rejected.

No user-facing lifetime annotation system was added. The source-level borrow
return declaration already identifies the lifetime relationship the language
supports; inference beyond that is not manufactured.

## 4. Control-Flow Analysis

The source checker already snapshots state at `if` branches, merges conditional
borrows and writes through `while`/`for` back-edges. Phase 3 adds the missing
proof on the lowered representation.

`verify_borrow_flow` in `compiler/lib/TIR/BorrowFlow.cpp`:

1. Splits the flat label/branch operation stream into basic blocks.
2. Builds predecessor/successor edges for `branch`, both `branch_cond` targets
   and loop back-edges.
3. Computes a fixed point of active borrows and invalid owner roots at every
   block entry. At a merge, a restriction that may be active on any incoming path
   is preserved; an owner invalid on any path is invalid after the merge.
4. Computes backward liveness over the same CFG. Address-of results, copies,
   loads of bindings that hold the pointer, and borrowed call results forwarded
   from a declared source parameter form an alias set. A borrow is a restriction
   only while one of its aliases is live.
5. Re-verifies each reachable block with its final entry state, so conflicts are
   reported once rather than during every worklist iteration.

The liveness step is required for valid programs: `EndBorrow` is emitted at the
lexical boundary, but a binding that was overwritten or never used again must not
keep blocking a new disjoint borrow. Merges still union possible restrictions,
so a borrow live on only one branch is conservatively treated as possibly live.

## 5. TIR and Lowering

- `tir.borrow` and `tir.end_borrow` remain zero-cost ownership markers. They do
  not emit allocation, reference counting, GC roots, safepoints or barriers.
- `tir.move` still aliases the source; Phase 2 destruction is unchanged.
- `tir.drop` owns its release and expands to the same runtime call.
- Lifetime records and `nirqoba.lifetime_relations` survive translation to MLIR.
- The verifier now rejects:
  - conflicting borrows at a merge or loop back-edge;
  - a borrow of storage whose owner was moved or destroyed on a reachable path;
  - a write overlapping a live borrow;
  - a move/drop while a live borrow depends on the owner;
  - a local borrow returned without a source parameter;
  - a return whose borrow source does not match the declared parameter;
  - mutable/immutable return mismatches;
  - an aggregate holding a borrow escaping through a return or call;
  - ending a borrow that is not active on any reaching path;
  - duplicate or stale lexical lifetime records;
  - a call that moves or mutably borrows an owner with a live conflicting borrow.

Diagnostics use `NRQ4007` for internal TIR borrow violations. User-visible
source errors remain `NRQ3015`, with notes at the conflicting borrow for the
address-of and call-argument paths.

## 6. Interprocedural Contracts

A function's borrow contract is its parameter ownership plus its return
relationship: `Borrowed`, `MutableBorrowed`, `Owned`, `Shared`, and the declared
return source. At a call, TIR checks:

- argument ownership against the callee parameter;
- borrowed arguments passed to owning parameters;
- mutable/immutable parameter compatibility;
- aggregate borrow escapes through by-value arguments;
- active-borrow conflicts when an owning or mutable parameter consumes an owner;
- borrowed call results tied to the declared source argument for liveness.

Sema's existing inferred parameter effects remain the source-level summary;
Phase 3 does not introduce a second runtime effect model.

## 7. Escape Analysis

Rejected:

- returning an address of a local;
- returning a borrow whose origin is not the declared source parameter;
- returning an aggregate containing a borrowed field;
- passing an aggregate containing a borrow to a call that cannot retain it;
- moving or releasing an owner while a borrow is live;
- storing a borrow into a container or aggregate that outlives its owner (Sema
  rejects the aggregate/container construction when the origin is not live);
- mutating through an immutable borrow or escalating it to mutable.

Accepted:

- parameter-bound borrows returned through the declared source;
- nested and forwarded borrows whose source parameter is preserved;
- borrowed aggregate fields used within the owner's scope;
- disjoint field borrows;
- recursive calls passing a borrowed parameter.

## 8. Runtime and GC Constraints

No runtime borrow tracking was added. Borrowing remains compile-time only.
`MlirPipelineTest.BorrowingIntroducesNoRuntimeCalls` lowers an address-of/read
program and asserts the MLIR contains no borrow, allocation or GC call. Ordinary
allocation remains GC-backed; Phase 4 was not started.

## 9. Phase 1/2 Corrections

Two defects found during Phase 3 were fixed because they blocked sound borrowing:

1. **Call-boundary borrow permission.** `&` used to become a mutable borrow even
   when the callee parameter was immutable, so two valid immutable arguments
   were rejected. Sema now narrows the borrow to the parameter contract.
2. **Loop merge and scalar tracking.** Taking a scalar's address inside a loop
   assigned it an allocation id; the branch merge treated the id appearing on
   one path as a different owner and marked the scalar moved. The merge now
   accepts a zero/non-owner id pair as the same storage, while distinct tracked
   owners still merge conservatively.

## 10. Tests and Evidence

- Unit: `307` tests, `307` passed.
- Full runner: `535` cases (`502` integration + `33` memory model), `535`
  passed, 0 failed, 0 skipped.
- New memory-model cases:
  - positive: immutable pairs, mutable writes, branch merges, loop back-edges,
    disjoint fields, forwarded calls, parameter-bound returns, nested scope
    expiration, mutable expiration, loop-local borrows and recursion;
  - negative: conflicting borrows, moving a borrowed owner, releasing a borrowed
    owner, returning a local borrow and returning a borrowed aggregate.
- Unit corruption tests cover: owner destroyed on one branch, live conflicting
  borrows, inexact lifetime records, wrong return source, duplicate lifetime
  records and mutable return mismatch.
- Generated-code evidence: the MLIR borrowing test observes zero runtime calls;
  TIR tests observe `tir.borrow` / `tir.end_borrow` and the CFG diagnostics.
- Regression: GC, allocator, secure-storage, stack-promotion and all existing
  ownership integration suites pass in the same run.

No benchmark was run. The new analysis is compile-time only and does not add a
runtime instruction; no performance superiority is claimed.

## 11. Known Limitations and Risks

- The TIR aliasing model tracks address-of values, copies, loads from local
  bindings and borrowed call results. It does not model arbitrary pointer
  arithmetic or aliases hidden behind external calls; those remain Sema/FFI
  contracts, and Phase 8 is not started.
- A restriction that may be active on one branch is conservatively kept after a
  merge. This can reject a program where the borrow is provably dead on every
  path but the alias set cannot express it.
- Lexical end markers remain at scope boundaries; the liveness analysis decides
  whether a borrow still restricts later code. There is no user-visible early
  `end borrow` statement.
- Aggregates can hold borrowed fields, but there is no general user-defined
  lifetime parameter syntax; returned aggregate borrows are rejected.
- Containers holding borrowed views are limited to the existing supported array
  view forms. General borrowed container elements remain unsupported.
- No runtime borrow checking exists. A foreign function that violates its
  declared contract is outside compile-time enforcement.
- Phase 4 ordinary non-GC allocation, arenas, shared-counted ownership and GC
  isolation were not started.

## 12. Phase Boundary

Phase 3 stops at borrowing and lifetime enforcement. No Phase 4 work, no new
syntax, no runtime borrow checks, no arena or shared-ownership implementation
and no optimization pass was added.
