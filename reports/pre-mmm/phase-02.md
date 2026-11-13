# Pre-MMM Phase 2 - Lifetime propagation, borrowed returns, aggregate fields, and exclusivity

**Status:** In progress (lifetime propagation, source-bound borrowed returns, aggregate-field borrows, mutable-borrow exclusivity, branch-selected borrow paths, one-sided conditional borrows, null-check refinement, short-circuit null facts, branch-dependent owner identity, interprocedural borrowed-return effects, flow-sensitive expiration/reassignment, compact function-effect summaries, and aggregate escape safety complete)
**Version:** 0.453-beta
**Date:** 2026-11-13
**Commits:** `429f01d`, `994e8bd`, `4eba86a`, `9420e6b`, `b276ae6`, `cc595bf`, `3ef1e49`, `03505c8`, `6f26f35`, `c06ef19`, `3959176`, `3e068c4`, `c4c684a`, `fef0d00`, `58124f3`, `734cad0`, `b809487`, `77c05a9`, `6539655`, `17697c7`, `6251871`, `bc1bc16`, `6acc26a`, `5d348e6`, `7acbfec`, `1504f8d`, `b361992`, `7d185f9`, `19f1f5a`

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
- `p = nullptr` releases a borrow. A borrowed binding may also be rebound to
  another validated borrow; the old owner/path is released and the replacement
  must be live and outlive the binding.

## Rules enforced

1. A borrow is valid only while its originating owner is a live binding in the
   current scope.
2. A borrow may not outlive its owner by being assigned to a binding declared in
   an outer scope.
3. Borrowed arguments may be forwarded through any number of calls; each call
   revalidates the original owner.
4. Borrowing does not transfer ownership. Borrowed values remain non-owning and
   are not cleaned up.
5. Null assignment ends a borrow. Rebinding to another validated borrow is
   allowed when the replacement origin is live and its owner outlives the
   binding; the previous owner/path is released.

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

## Aggregate-field borrows (0.423-beta)

A borrow of a field projection such as `&obj.data` or `&obj.inner.data` now
records the aggregate binding and the full dotted field path. The lifetime owner
is the aggregate, not the projected field's type.

- Direct field borrows and nested projections resolve to the root aggregate and
  share its allocation identity.
- A borrowed field can be forwarded through `borrow` parameters and returned
  through an existing source-bound borrowed return. The caller's field path is
  preserved through the nested call.
- Replacing the borrowed field, one of its ancestors, or one of its descendants
  while the borrow is live is rejected. Replacing a parent aggregate field is
  rejected when a descendant is borrowed.
- Compound field writes (`obj.field += ...`) use the same invalidation rule as
  plain assignment.
- Replacing the whole aggregate, moving an owning aggregate, moving an owned
  field, and letting a field borrow outlive its owner remain rejected.
- Independent sibling fields remain writable while another field is borrowed,
  so the new check is path-specific rather than whole-aggregate-only.
- Borrowing accepts leaf fields whose storage is numeric, boolean or pointer.
  Aggregate-valued fields, owned arrays and managed references remain outside
  the addressable subset.

## Mutable-borrow exclusivity (0.427-beta)

Borrow conflicts are now queried by owner and dotted field path rather than by
allocation alone. A request is described by its allocation id, path, and
mutability; the checker returns the first live borrow that overlaps it.

- Two immutable borrows of the same path remain legal. A mutable request
  conflicts with any overlapping borrow, and an immutable request conflicts with
  an overlapping mutable borrow.
- Disjoint sibling fields do not overlap. Mutable borrows of `p.x` and `p.y`
  may coexist, while `p.x` and `p.inner.value` conflict by path ancestry.
- Creating an address with `&` is rejected when an overlapping mutable borrow is
  live. The operand is checked in a borrow-operand mode so creating the borrow
  does not also report an ordinary read.
- Reading a struct or tuple field, an owned array, or the whole aggregate while
  an overlapping mutable borrow is live is rejected. The aggregate base of a
  member access is not treated as a whole-value read, so disjoint field access
  stays legal.
- Calls track borrows already claimed by earlier arguments. A later argument that
  overlaps an earlier mutable borrow is rejected, including when the same path is
  claimed twice in one call. Disjoint field arguments remain accepted.
- The earlier arguments are also visible to reads and address-of checks inside a
  later argument, so argument order cannot hide a conflicting access.
- Returned mutable borrows keep their mutability when assigned to a pointer.
  Forwarding through nested borrowed-return helpers preserves the caller's field
  path and therefore the conflict.
- At 0.427, cached field paths were cleared to whole-owner access when
  conditional branches assigned different paths. This was conservative: a later
  disjoint access could be rejected rather than relying on one branch's path.
  0.431 retains the distinct paths instead.

## Branch-selected borrow paths (0.431-beta)

A borrowed binding now stores the set of field paths that can be live after a
conditional merge. The representation keeps one primary path and allocates the
alternative vector only when branches actually select different paths, so the
normal single-path case stays compact.

- When both branches leave a live borrow of the same owner, the merged symbol
  records every possible path. Conflict checks test all of them, so a later
  access to any overlapping path is rejected while a path disjoint from all
  alternatives remains independently accessible.
- Nested projections keep their full dotted paths (`.inner.a` and `.inner.b`
  remain distinct, while an ancestor or descendant still overlaps).
- Paths propagate through aliases, mutable borrow arguments, and source-bound
  borrowed returns. A returned borrow from an ambiguous live alias is rejected
  rather than creating a second mutable alias.
- The merge also carries the shared borrow owner and adopts the branch allocation
  identity when both branches agree. The incoming state may still have an
  untracked id, so this is required before a later conflict query can be sound.
- At 0.431, if only one branch had a live borrow, the merged binding fell back
  to whole-owner access; 0.437 retains its path as a conditional borrow. If the
  branches select different owner allocations, the binding is marked unusable.
- Branch paths are compile-time semantic state only. There are no runtime
  counters, locks, descriptors, allocations, or generated checks.

## One-sided conditional borrows (0.437-beta)

When only one branch leaves a live borrow, the merged binding now keeps that
branch's owner, mutability, and field path instead of clearing the projection to
whole-owner access. A new `borrow_conditional` flag records that the borrow is
possible but not present on every path.

- Conflict queries still include the symbol and test its retained path. A later
  read, write, or new borrow overlapping the path is rejected on every reachable
  execution, while a sibling field disjoint from the path remains available.
- A conditional borrow is not usable as a definite borrow. Aliasing it, taking a
  new borrow through it, forwarding it to a call, or returning it is rejected
  with `borrow of '...' may not be live on every branch`.
- The merge adopts the active branch's allocation identity when the other side is
  an ordinary or untracked pointer. If the inactive side owns or shares an
  allocation, the binding remains unusable, so distinct owners are not merged.
- An explicit assignment replaces the conditional state with the new binding's
  state, keeping the ordinary single-path fast path unchanged.
- The state is compile-time only: one boolean on the existing symbol and the
  existing inline path. No runtime counters, locks, descriptors, allocations, or
  generated checks were added.

## Flow-sensitive null refinement (0.441-beta)

A conditional borrow is no longer permanently indefinite. When one branch
proves the binding non-null and the path where the borrow is absent is known to
be null, the branch state clears the conditional marker and treats the borrow as
definite. This is a semantic refinement only; the owner, mutability, and field
path set are unchanged.

- The one-sided merge records `borrow_conditional_absent_null` from the absent
  branch. A non-null check refines only when that flag is true, so a non-null
  pointer to another object cannot be mistaken for the conditional borrow.
- Definite nullness is tracked with a compile-time flag. `nullptr` declarations
  and assignments set it, other pointer assignments clear it, and branch merges
  combine it with logical AND.
- `if (view != nullptr)` refines the then-branch; `if (view == nullptr)` refines
  the else path. If the null branch cannot fall through, the refined state
  continues after the statement.
- The refinement is branch-local. A join restores the conditional marker unless
  every continuing path preserves the same null-absence proof; the null path
  keeps it, and a later assignment replaces it.
- Confirmed conditional borrows still enforce their field-path exclusion. A
  later borrow or read of the same path is rejected; disjoint fields remain
  available. No runtime counters, locks, allocations, or generated checks exist.

## Short-circuit null facts (0.445-beta)

Condition facts now follow the actual true/false evaluation paths of `&&`, `||`,
and logical negation. For `A && B` true, both operand facts hold. For `A && B`
false, a fact is retained only if it is implied by both the lhs-false path and
the lhs-true/rhs-false path. `A || B` is dual. The intersection prevents a
short-circuited right operand from being treated as evaluated on every path.

`!` swaps the requested truth value and nested logical expressions recurse. The
result updates both nullable-dereference proofs and conditional-borrow
refinement. A null fact also removes a name from the non-null proof set. Only
name-versus-null comparisons contribute facts; arbitrary predicates remain
opaque. The analysis uses small branch-local fact sets and allocates nothing on
the ordinary simple-condition path.

## Branch-dependent owner identity (0.445-beta)

When both arms leave live borrows of different owners, the merge now retains
both possibilities. The primary owner/path stays inline. Secondary owners are
compact `BorrowTarget` records with an allocation id, owner name, path set, and
mutability; the vector is empty for ordinary single-owner borrows.

Conflict, liveness, field-borrow, moved-allocation, and allocation-status
queries inspect every target. A later borrow or read overlapping either selected
owner path is rejected, while disjoint fields on either owner remain usable.
Moving or releasing any possible owner conservatively invalidates the binding.
Aliases, calls, address-taking, assignment from, and returns through an
ambiguous-owner binding are rejected with `selects between multiple owners`, so
no branch's identity is silently chosen. Direct reads and dereferences remain
available because all targets are live borrows of the same value type.

## Interprocedural borrowed-return effects (0.449-beta)

A borrowed return names one source parameter. That source is now resolved to an
index once when the signature is built and reused as a compact effect summary.
Recovering the origin and field paths of a returned borrow therefore does not
rescan the parameter-name vector at each call site, and compilation stays
modular: the callee still owns its parameter origin while the caller supplies
the concrete allocation.

A call returning a borrow can now be passed directly as a borrow argument.
Previously its original owner was discarded by an argument-type check, producing
a spurious `borrow arguments must refer to a named pointer` error. The checker
now treats the result as a sourced borrow, propagates its owner and path, and
participates in the same argument-order exclusivity checks as a direct borrow.
Nested and chained results that overlap are rejected; disjoint fields remain
usable. Stored returned borrows remain aliases after their producer is rebound or
cleared.

## Flow-sensitive borrow expiration and rebinding (0.449-beta)

A borrowed binding can be reassigned to another validated borrow or cleared with
`nullptr`. The old owner/path ends at the assignment. The replacement must have a
live origin, a definite owner, and an owner that outlives the binding; its
allocation id, mutability, owner, and path replace the old state. Assigning a
plain owned or raw pointer is still rejected because it would bypass explicit
borrow semantics. An alias of the old borrow is independent state and continues
to block overlapping accesses.

The conditional merge now keeps the allocation id of the branch that is still
borrowed. A null/ordinary pointer assignment on the inactive branch can allocate
a fresh id; preferring that id previously hid the still-possible borrow from
conflict checks. Keeping the active id makes borrow-then-null merges sound while
still allowing the old field once every continuing arm releases it. The
remaining state is compile-time only.

## Interprocedural borrow-effect summaries (0.453-beta)

Each `FunctionSignature` now stores one compact effect byte per parameter. The
bits describe reads, writes, aliases, and escapes of a borrowed parameter.
Declared effects are available before a body is checked: a borrowed parameter
starts as a read, a `borrow mut` parameter adds a write, and the declared return
source adds escape. When a body is checked, direct uses and nested calls merge
into the current function's summary. A checked signature is cached; recursive
calls reuse the declared summary, and a callee without a completed summary gets
all four effects conservatively. Effects are aggregate and compile-time only,
never caller allocation ids.

Call sites consume the summary when claiming a borrowed argument. A write effect
makes the argument an exclusive mutable claim even if the effect came indirectly
through forwarding, so an existing live borrow is rejected at the boundary. The
summary is representation-light: one byte per parameter and no per-call walk of
the callee body.

## Aggregate borrow escape safety (0.453-beta)

Struct initializers now record the origin of every borrowed field as owner,
allocation id, path set, and mutability. Nested aggregates contribute their
field origins transitively. A declaration validates every recorded owner against
the target binding's scope; an assignment does the same against the existing
target scope. This rejects an outer aggregate that would retain a borrow of an
inner owner.

Aggregates holding borrowed fields cannot escape through a return value, a call
argument, or storage into another aggregate or allocated memory. Returning such
an aggregate is rejected even when the borrow came from a parameter. The
dedicated diagnostics name the lifetime or storage boundary instead of silently
copying a dangling reference. Moving or replacing an owner while an aggregate
field borrow is live is also rejected through the existing live-borrow queries.

Valid borrowing remains available: a nested aggregate in the owner's scope can
hold and mutate a borrowed field, and disjoint field paths remain independently
borrowable. The metadata is compile-time-only and adds no runtime tracking.

## Tests

- `positive/branch_borrow_paths_disjoint`: branch-selected `.x`/`.y` paths leave
  `p.z` independently available; output `9`.
- `positive/branch_borrow_paths_nested`: branch-selected `.inner.a`/`.inner.b`
  paths leave `o.z` available; output `9`.
- `positive/branch_borrow_paths_call`: the ambiguous path set survives a
  borrowed-return call while a disjoint `p.z` argument is accepted.
- `negative/branch_borrow_paths_overlap`: a later mutable borrow of `p.x` is
  rejected because `.x` remains one possible branch path.
- `negative/branch_borrow_paths_nested_overlap`: a later borrow of
  `o.inner.a` is rejected through the same nested path.
- `negative/branch_borrow_paths_call_overlap`: the ambiguous path set forwarded
  through a call conflicts with a second argument for `p.x`.
- `negative/branch_borrow_paths_alias_conflict`: returning a borrow from the
  ambiguous live alias is rejected with both possible paths named.
- `negative/branch_borrow_paths_read_overlap`: reading `p.x` is rejected while
  `.x` remains one possible branch path.
- `positive/one_sided_borrow_paths_disjoint`: only one branch borrows `.x`; the
  merged conditional path still leaves `p.z` available; output `9`.
- `positive/one_sided_borrow_paths_nested`: only one branch borrows
  `o.inner.a`; the conditional path leaves `o.z` available; output `9`.
- `negative/one_sided_borrow_paths_overlap`: a later mutable borrow of `p.x` is
  rejected because the one-sided path may be live.
- `negative/one_sided_borrow_paths_read`: reading `p.x` is rejected for the same
  reason.
- `negative/one_sided_borrow_paths_alias`: aliasing the conditional binding is
  rejected as not definitely live.
- `negative/one_sided_borrow_paths_return`: forwarding the conditional binding
  through a borrowed-return call is rejected as not definitely live.
- `positive/conditional_borrow_null_refinement`: a non-null branch refines the
  borrow, accepts an alias and a disjoint `p.z` borrow, and prints `9`, `4`.
- `positive/conditional_borrow_guard_refinement`: an early return on the null
  path carries the refined borrow past the guard; output `9`.
- `positive/conditional_borrow_call_refinement`: a refined immutable borrow is
  accepted by a nested read call; output `7`.
- `negative/conditional_borrow_null_path`: aliasing the conditional borrow in
  the null path is still rejected.
- `negative/conditional_borrow_fallthrough`: refinement does not survive a join
  where both paths continue; the later alias is rejected.
- `negative/conditional_borrow_refined_overlap`: a refined borrow still rejects
  a new borrow of the same field.
- `positive/conditional_borrow_join_refinement`: an unrelated join preserves the
  null-absence proof when every continuing path keeps it; output `9`.
- `positive/conditional_borrow_logical_and`: `&&` true-path refinement; output
  `9`.
- `positive/conditional_borrow_logical_or`: `||` false-path refinement; output
  `9`.
- `positive/conditional_borrow_logical_not`: logical-negation refinement; output
  `9`.
- `positive/conditional_borrow_nested_logical`: nested logical refinement;
  output `9`.
- `negative/conditional_borrow_short_circuit_or`: an `||` true path does not
  grant a definite borrow.
- `negative/conditional_borrow_short_circuit_and`: an `&&` false path does not
  grant a definite borrow.
- `positive/branch_owner_identity_disjoint`: branch-selected owners leave
  `p.y` and `q.z` available; output `5`, `6`.
- `positive/branch_owner_identity_nested_disjoint`: branch-selected nested paths
  leave `p.inner.b` and `q.z` available; output `8`, `9`.
- `negative/branch_owner_identity_alias`: aliasing an ambiguous-owner binding is
  rejected.
- `negative/branch_owner_identity_secondary_conflict`: a later `q.x` borrow is
  rejected through the secondary target.
- `negative/branch_owner_identity_primary_conflict`: a later `p.x` borrow is
  rejected through the primary target.
- `positive/interprocedural_nested_borrow_disjoint`: a borrowed-return result
  passed directly to a call leaves a disjoint field available; output `7`, `8`.
- `positive/interprocedural_nested_borrow_chain_disjoint`: two nested
  borrowed-return calls keep their owner/path and leave a disjoint field
  available; output `11`, `12`.
- `negative/interprocedural_nested_borrow_conflict`: a nested borrowed-return
  result conflicts with a later argument for the same field.
- `negative/interprocedural_nested_borrow_chain_conflict`: the same conflict is
  detected through a chain of borrowed-return calls.
- `negative/interprocedural_returned_alias_conflict`: a stored returned borrow
  remains live and conflicts with a new borrow of its source field.
- `positive/flow_borrow_rebind_releases_path`: rebinding `view` from `p.x` to
  `p.y` releases `p.x`; output `2`, `9`.
- `positive/flow_borrow_branch_local_expiration`: clearing a borrow inside a
  branch releases its path for that branch.
- `positive/flow_borrow_branch_release`: rebinding or clearing on every arm
  releases the old field after the merge.
- `negative/flow_borrow_branch_retains_conflict`: a borrow still possible on the
  other arm blocks the old field after the merge.
- `negative/flow_borrow_rebind_stale_alias_conflict`: an independent alias of the
  old borrow still blocks the released field after rebinding.
- `negative/flow_borrow_null_stale_alias_conflict`: the same stale-alias check
  applies after null expiration.
- `negative/flow_borrow_rebind_short_lived_owner`: rebinding to an inner-scope
  owner reports `borrow of 'inner' cannot outlive its owner`.
- `positive/recursive_borrow_effect`: a recursive borrowed-return function keeps
  its source identity and returns a mutable view; output `9`.
- `positive/aggregate_borrow_same_scope`: a nested aggregate holds and mutates a
  borrowed field in its owner's scope; output `9`.
- `positive/aggregate_borrow_disjoint_path`: an aggregate field borrow of
  `p.x` leaves `p.y` independently borrowable; output `9`.
- `positive/aggregate_borrow_alias_copy`: copying an aggregate aliases the same
  borrowed field within the owner's scope; output `8`.
- `negative/aggregate_borrow_call`: passing an aggregate with a borrowed field
  through a call is rejected.
- `negative/aggregate_borrow_field_storage`: storing such an aggregate in
  another aggregate field is rejected.
- `negative/aggregate_borrow_inner_escape`: assigning an inner aggregate to an
  outer target is rejected.
- `negative/aggregate_borrow_nested_return`: returning a nested aggregate with
  a borrowed field is rejected.
- `negative/aggregate_borrow_owner_replacement`: replacing the owner while an
  aggregate field borrow is live is rejected.
- `negative/aggregate_borrow_return`: returning an aggregate with a borrowed
  field is rejected.
- `positive/mutable_borrow_disjoint_fields`: mutable borrows of `p.x` and `p.y`
  coexist, and a call receives both disjoint field borrows; output `30`.
- `positive/mutable_borrow_forwarding`: a mutable field borrow survives two
  forwarded borrowed-return calls while a disjoint sibling remains borrowable;
  output `15`.
- `positive/mutable_borrow_expiration`: a new mutable borrow of `p.x` is accepted
  after the previous borrow's block ends; output `9`.
- `negative/mutable_borrow_overlap`: a second mutable borrow of `p.x` while the
  first is live is rejected.
- `negative/mutable_borrow_immutable_conflict`: a mutable borrow of `p.x` while an
  immutable borrow of the same path is live is rejected.
- `negative/mutable_borrow_read_conflict`: reading `p.x` while it is mutably
  borrowed is rejected.
- `negative/mutable_borrow_nested_read_conflict`: reading `o.inner.value` while
  that nested path is mutably borrowed is rejected.
- `negative/mutable_borrow_whole_read`: passing the whole aggregate while one of
  its fields is mutably borrowed is rejected.
- `negative/mutable_borrow_call_argument_conflict`: the same mutable alias passed
  twice to one call is rejected.
- `negative/mutable_borrow_forwarded_conflict`: a forwarded mutable field borrow
  conflicts with a later borrow of the same path.
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
- `positive/borrow_field_direct`: `&p.x` stays valid while sibling `p.y` is
  written; exits with `22`.
- `positive/borrow_field_nested`: `&o.inner.value` stays valid while sibling
  `o.other` is written; exits with `22`.
- `positive/borrow_field_forwarded_return`: a field borrow forwarded through two
  borrowed-return helpers remains valid for an independent write; exits with `9`.
- `negative/borrow_field_replacement`: direct replacement of `p.x` is rejected.
- `negative/borrow_field_nested_replacement`: replacing `o.inner` while
  `o.inner.value` is borrowed is rejected.
- `negative/borrow_field_forwarded_replacement`: the path survives forwarding,
  so replacing `p.x` after the returned borrow is rejected.
- `negative/borrow_field_compound_replacement`: `p.x += 1` is rejected while
  `&p.x` is live.
- `negative/borrow_field_owner_escape`: a field borrow assigned to an outer
  binding is rejected as outliving its owner.
- `negative/borrow_field_owner_move`: moving an owning aggregate with a live
  field borrow is rejected.
- `negative/borrow_field_owned_field_move`: moving an owned field with a live
  sibling field borrow is rejected.
- `negative/borrow_field_aggregate_replacement`: replacing the whole aggregate
  with a live field borrow is rejected.

## Verification

- Clean `RelWithDebInfo` build at `0.453-beta`; the `0.451-beta` released
  checkpoint and `0.453-beta` working build use the same source.
- Unit suite: 214/214 passed on the final post-clean run.
- Integration suite: 421/421 passed, including the recursive effect case, the
  aggregate alias/escape cases, the five interprocedural cases, the seven
  flow-sensitive expiration/rebinding cases, and all earlier regression cases.
- Compiler check benchmark (`allocator_strategies`, three trials of 20 timed
  runs after warmup): baseline `0.449-beta` trial medians were 20.838, 21.388,
  and 20.510 ms; current `0.453-beta` medians were 20.955, 20.800, and 20.651 ms.
  Median of trial medians: 20.838 ms baseline vs 20.800 ms current; mean was
  20.855 ms vs 21.087 ms, p95 22.085 ms vs 23.025 ms, and median peak RSS
  16.002 MiB vs 16.016 MiB. The difference is within run-to-run noise and this
  is a single-file smoke benchmark, not a full compiler benchmark.
- One earlier full unit run during development segfaulted in an unrelated GC
  test; immediate full reruns passed, including the final post-clean run. The
  intermittent GC-test failure was not reproduced.

## Limitations

- Borrowed parameters are validated modularly. The callee treats its parameter
  as the origin and the call site validates the caller's owner. Declared
  borrowed-return sources propagate through nested calls, but full mutation and
  alias effect summaries for arbitrary callee bodies are not inferred.
- Borrowed returns require an explicit source parameter; the checker does not
  infer a source from the function body.
- Source selection is not branch-sensitive. A return that could select between
  different owners on different paths is rejected unless it matches the one
  declared source parameter.
- Aggregate field borrowing is limited to leaf fields with numeric, boolean or
  pointer storage. Aggregate-valued fields, owned arrays and managed references
  remain unaddressable.
- A one-sided borrow keeps its field path but is marked conditional. It becomes
  definite only after a non-null guard when the absent path is provably null.
  The refinement is lost at a join that continues on a path without the same
  proof, and it is not available on the null path. Branches that select different
  owner allocations retain both targets, but using the binding where one owner
  must be named remains rejected.
- Owning aggregates still use the existing broad live-borrow guard for owner
  mutation, so an independent field write may be rejected conservatively and the
  diagnostic retains array-view wording.
- Borrowed struct fields are tracked through the containing aggregate value,
  not as independent lexical bindings. Field storage is rejected, while direct
  construction and same-scope nested aggregates remain supported.
- Mutable-borrow exclusivity is enforced at borrow creation, reads, and call
  boundaries. Effect summaries now carry reads, writes, aliases, and escapes
  through nested calls, but the callee does not receive the caller's allocation
  id or live-borrow set; a summary is aggregate, not path-sensitive per owner.
- Reassignment supports another validated borrow or `nullptr`. Rebinding to a
  plain owned or raw pointer remains rejected, and assigning `nullptr` to an
  owner that still has a live borrow remains conservatively rejected rather than
  silently invalidating every alias.
- Branch-sensitive owner selection for borrowed returns, closure capture, and
  general escape analysis beyond the supported aggregate paths are not
  implemented.
- Escape checking for borrowed-field aggregates is deliberately conservative:
  all calls and returns are rejected, even when a callee would only read the
  aggregate locally.
- The work adds no runtime counters, locks, descriptors, heap allocation, or
  generated checks; borrowing remains compile-time only.
