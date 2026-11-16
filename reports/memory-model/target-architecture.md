# Memory Model Migration — Frozen Target Architecture

**Project:** Nirqoba  
**Frozen at:** `0.511-beta`, HEAD `d4292aa`  
**Phase:** 0 — design freeze  
**Companion audit:** [`migration-audit.md`](migration-audit.md)  
**Authority:** This document is the Phase 1 contract. It does not add language
syntax and does not authorize implementation beyond Phase 0.

---

## 1. Purpose

This is the target architecture for the ownership-first hybrid memory model. It
freezes the invariants that later phases must preserve, records the decisions that
were ambiguous in the current tree, and defines the boundary between "already
implemented" and "must be built".

It deliberately does **not** redesign working ownership semantics. The existing
owned/borrowed/shared/GC behavior is the baseline; later phases must represent,
verify, isolate and complete it.

---

## 2. Frozen invariants

These are non-negotiable for Phases 1–9 unless the project authority explicitly
amends this document.

### I1 — Deterministic ownership is the default

Ordinary values have an identifiable owner. Ordinary heap allocation, destruction,
moves and borrowing must not require the tracing collector.

### I2 — A value has one release obligation

At every program point an owning value is either:

- live and owned by exactly one owner;
- moved, with the source unusable;
- borrowed, with no release obligation;
- discharged, with no further release.

Exactly-once destruction is a semantic rule, not a runtime convention.

### I3 — Borrowing never owns and never releases

`borrow` and `borrow mut` values are non-owning. They may not outlive their source,
may not be released, and may not be copied into storage that outlives the source.
Mutable borrows are exclusive for their overlapping paths.

### I4 — Moves transfer, copies of owning values do not exist

Moving invalidates the source. A copy of an owning value is either rejected or is a
deliberate transfer. Shared fields are copied by retain, never by a raw bitwise
copy of the token.

### I5 — Aggregates are the field-contract boundary

Lexical aggregates may hold `owned`, `owned <resource>`, `shared`, `borrow` and
`managed<T>` fields. Generated aggregate destruction walks fields in reverse
declaration order; aggregate copy retains shared fields and is rejected or moved
for owning fields. Unqualified raw pointer fields are rejected.

### I6 — A storage boundary must be able to run the release it accepts

If a container or allocation path cannot run per-element/per-field destruction, it
rejects deterministic-owner payloads. It must never silently:

- drop a token,
- route the payload to the collector,
- free raw bytes while a field still owns a resource.

The Phase 8 policy table remains in force. Containers accept only trivial and
managed-reference payloads unless a later phase adds an explicit destructor path.

### I7 — Shared ownership is explicit and reference-counted

Shared ownership is opt-in, never universal. Strong references are counted;
retain/release pair exactly; the final release destroys exactly once. Shared values
do not make ordinary owned values pay reference-counting costs. Weak references are
introduced only if a concrete standard-library need justifies them.

### I8 — Arena/pool lifetime is explicit

An arena/pool has a defined lifetime boundary. Reset/destroy is legal only when
quiescent. Arena reset must not pretend to run object destructors unless a later
phase implements that. References may not escape an arena boundary. Stack
allocators preserve LIFO.

### I9 — GC is precise, non-moving and explicitly selected

The collector remains available for cyclic/dynamic graphs. It is precise,
non-moving, generational and incremental. Managed references use typed roots, root
frames, write barriers and safepoints. Automatic collection starts disabled;
`array.allocate<T>` is the explicit collector allocation and `gc.collect()` is an
explicit cycle.

**Phase 7 status:** verified at `0.575-beta`. `array.allocate<T>` remains the
only explicit collector allocation; `managed<T>` is its reference type and
`memory.owned_soa<T>` is an explicitly GC-backed container with a legacy name.
The audit in `reports/memory-model/phase-7-audit.md` traces each entry point
through Sema, TIR, MLIR/LLVM and the runtime. No automatic compiler-selected GC
or CDMM is present.

### I10 — Owned-only execution contains no GC infrastructure

An ordinary owned/borrowed program must not contain, at `-O0` or `-O2`:

- GC allocation;
- root registration/unregistration for types with no managed references;
- root frames;
- safepoints;
- mutator registration;
- write barriers;
- GC object headers/metadata;
- GC collection bookkeeping or locks.

**Phase 4 status:** verified at `0.551-beta`. Owned pointer, returned owned
array, borrowed heap view, owned aggregate, reallocation, secure allocation and
plain shared allocation fixtures emit no `nrq_gc` reference at `-O0` or `-O2`.
Registration and unregistration remain only for storage whose payload actually
contains a managed reference. The TIR verifier now ties each allocation strategy
to its ownership category and storage shape.

**Phase 5 status:** verified at `0.563-beta`. Tracked arena and pool programs
emit no `nrq_gc` reference at `-O0` or `-O2`. Arena-bound allocation uses
`nrq_arena_allocate` with no per-pointer registry record; pools keep the
registered path so `memory.pool_release` can return the slot. Bulk reset and
destroy invalidate the region without running object destructors, and payloads
whose cleanup bulk release cannot perform are rejected at compile time.

**Phase 7 status:** verified at `0.575-beta`. The ordinary-fixture set now
includes owned pointers, owned arrays, nested owned aggregates, borrowed heap
views, reallocation, secure allocation, plain shared allocation, helper chains,
arenas, pools and the development runtime. A declaration-aware check compiles
each at `-O0` and `-O2`, inspects emitted LLVM and the object symbol table, and
requires zero `nrq_gc` names; a managed control object must contain GC symbols.
The check is `tests/phase7_isolation.py`.

### I11 — Mixed-strategy interoperability is deliberate

Any storage outside the collector that contains a managed reference must have:

- a defined registration point,
- a defined unregister point, and
- a defined final-release/cleanup rule.

If a path cannot provide all three, it rejects the managed payload. Secure
allocation has no rooted path and rejects managed payloads. **Phase 7 status:**
tracked arena and pool allocation now also rejects managed payloads, because
bulk reset/destroy has no unregistration point; the Sema diagnostic and TIR
verifier rule are both enforced. Ordinary and shared storage containing managed
fields register typed roots and release them on deallocation/final release.

### I12 — Ownership is represented and verified in TIR

TIR must carry enough information to answer, without Sema:

- is this value owned, borrowed, shared, managed or trivial?
- has this value been moved?
- what release obligation exists?
- which borrow/source does this value depend on?
- is this cleanup valid for this type/domain?

The TIR verifier must reject invalid ownership states. Cleanup must not be only a
string-named generic call that no verifier understands.

### I13 — Failure paths are explicit about cleanup

There is no language exception model. A fatal runtime exit does not run destructors.
Until an error/exception model exists, no phase may claim exception-safe cleanup.
Panic/fatal behavior is documented as process termination, not unwinding.

### I14 — No new syntax before semantics

Phases 1–3 establish representation and verification first. User-visible syntax is
added only when the semantics it expresses are already represented, tested and
documented. No speculative ownership syntax.

### I15 — Compatibility is preserved deliberately

Valid `0.511-beta` programs remain valid unless a documented correctness rule
requires rejection. Changes that alter lifetime semantics require a diagnostic, a
migration note and an explicit route to the old behavior where appropriate.

---

## 3. Target strategy model

### 3.1 Deterministic ownership

**Default for:** scalars, aggregates, owned pointers, owned arrays, resource
handles, explicit allocations used as owners.

**Owner:** a lexical binding, an owning aggregate field, or an owning parameter.

**Release:** generated from the type and its resource domain; runs at scope exit,
return, break, continue, or an explicit `defer` that supersedes it.

**Rules:**

- move-only for owning values;
- no use after move;
- no implicit copy;
- partial initialization is zeroed and destroyed safely;
- exactly-once release;
- no GC metadata for non-managed payloads (I10).

### 3.2 Borrowing

**Forms:** `borrow T*`, `borrow mut T*`, `array_view<T>`,
`borrow mut source array_view<T>`.

**Rules:**

- non-owning, no release;
- source-bound lifetime;
- path-sensitive exclusivity;
- borrowed returns name their source parameter;
- escapes through returns/aggregates/containers are rejected unless the container
  has an explicit borrow contract;
- borrow checking must not allocate or enter GC.

**Current state (Phase 3):** the source checker enforces immutable/mutable
exclusivity, owner validity, source-bound returns and aggregate/container
escapes. A TIR CFG pass verifies active borrows at merges and loop back-edges,
tracks alias liveness and rejects writes, moves and drops that invalidate a live
borrow. Borrowing lowers to no runtime operation. General user-defined lifetime
parameters and arbitrary pointer aliases inside foreign calls remain future
work.

### 3.3 Arena and pool

**Target:** allocator instances with an explicit lifetime boundary; arena-scoped
allocation with bulk destruction; escape validation; quiescent reset/destroy.

**Current state:** allocator kinds exist and are tested; object destruction, scoped
language semantics and escape validation do not.

**Frozen rule:** do not expose arenas as the default ownership strategy until reset
either runs the required destructors or is documented as storage-only. A
storage-only arena may not accept values with non-trivial release contracts.

### 3.4 Explicit shared ownership

**Target:** strong reference count with retain/release; explicit token transfer;
aggregate retain/release; thread-safe count updates; final-release destruction;
weak references only if justified.

**Current state:** strong refcount and aggregate support exist. Phase 6 at
`0.571-beta` replaces the global count lock with 64 pointer-selected control
shards and atomic counts, removes the control before final deallocation, and
rejects shared release while a borrow is live. Weak references remain
intentionally deferred: there is no source surface or concrete library
requirement, and supported shared payloads reject shared fields, so a
shared-to-shared owning cycle is not constructible. Cyclic graphs remain a GC
concern. Cross-thread sharing is verified through the runtime unit suite and a
C++ contention benchmark, not through a language thread API.

### 3.5 Selective GC

**Target:** explicit managed allocation; precise typed roots; generational,
incremental collection; cycles; explicit `gc.collect`; automatic collection opt-in;
mixed-strategy roots.

**Rule:** the collector is never a fallback for an ownership pattern the compiler
rejects.

### 3.6 Manual memory

**Target:** typed explicit allocation/deallocation, reallocation, secure buffers,
mappings, custom allocators, and a deliberately designed unsafe/FFI escape hatch.

**Current state at `0.583-beta`:** typed explicit allocation, reallocation,
secure/mapping and custom allocators exist. Manual storage has its own
`Manual` category, `memory.manual_allocate<T>` / `memory.manual_release`, and
release-pairing checks. Manual storage is function-local; it cannot cross an
owned parameter or return because there is no manual pointer type. Raw pointer
returns and unqualified raw pointer fields remain restricted. Unsafe scopes and
FFI do not exist; the Phase 8 audit records the absent ABI rather than inventing
one.

---

## 4. Allocation-category target map

| Allocation category | Target strategy | Current state |
|---|---|---|
| Scalar/local | stack/trivial | Complete |
| Owned aggregate field | deterministic owner | Complete |
| `memory.allocate<T>` | deterministic explicit owner | Complete; non-GC, with conditional rooting for managed payloads |
| `memory.manual_allocate<T>` | distinct manual owner | Complete at `0.583-beta`; manual release, function-local, conditional rooting |
| `memory.owned_array<T>` | deterministic bulk owner | Complete; non-GC, with conditional rooting for managed elements |
| `memory.owned_soa<T>` | collector-owned SoA | Complete for managed columns; asynchronous reclaim is documented |
| `memory.shared_allocate<T>` | explicit refcount | Complete at `0.571-beta` for supported non-cleanup payloads; sharded atomic counts and final-release safety |
| `memory.secure_allocate<T>` | secure explicit owner | Partial: no rooted managed payload contract |
| `memory.map_file` | explicit mapped resource | Complete; unmap is explicit |
| `file.open`/`socket.connect`/`mutex.create` | deterministic resource owner | Complete; shutdown registry fallback |
| `file.read_text` | borrowed process-lifetime string | Missing deterministic ownership; explicit exception |
| `array.allocate<T>` | collector-managed storage | Complete; opt-in |
| `memory.arena_allocator` / pool / stack | explicit allocator strategy | Complete allocator; missing bulk object destruction/scoped semantics |
| Manual release | `memory.manual_release` | Complete at `0.583-beta`; ordinary/secure/shared release is rejected |
| FFI allocation | deliberate foreign contract | Blocked: no FFI surface; documented unsupported boundary |
| Async/task allocation | deliberate runtime contract | N/A: no async runtime |

---

## 5. TIR ownership representation — Phase 1 contract

This is a semantic contract, not a final opcode spelling.

### 5.1 Required state

TIR must make the following explicit and checkable:

1. **Ownership category** for values, parameters and returns.
2. **Transfer** of an owned value (move) and invalidation of the source.
3. **Borrow** relationship: mutable/immutable, source value, field path.
4. **Lifetime** relationship for source-bound returns and aggregate borrows.
5. **Destruction obligation** for each owned value, including resource domain.
6. **Discharge** of that obligation by a lexical cleanup, defer, move or transfer;
   a drop owns its release, and an explicit release resolves the obligation.

### 5.2 Verifier rules

At minimum, the verifier must reject:

- use of a moved value;
- a second discharge of one obligation;
- release with the wrong resource domain;
- a borrowed value passed to an owning parameter or release;
- an owning value escaping through a borrow-only path;
- an aggregate/container payload that the receiving storage cannot release;
- a managed reference stored in a boundary with no root contract;
- an obligation that is never discharged or transferred;
- use of a value after its destruction;
- a drop followed by a duplicate release of the same value;
- overlapping borrows that conflict at a branch merge or loop back-edge;
- a borrow of an owner moved or destroyed on a reachable path;
- a write overlapping a live borrow;
- a borrowed return whose source does not match the declared parameter;
- an aggregate holding a borrow escaping through a return or call;
- a mutable/immutable return mismatch;
- a duplicate or stale lexical lifetime record.

### 5.3 Lowering contract

- Every cleanup call must correspond to a verified obligation.
- Lowering must not infer a release obligation from an untyped callee string alone.
- Optimization passes must preserve ownership and cleanup as observable effects.
- A pass that removes or reorders cleanup must prove the obligation is discharged
  elsewhere.

### 5.4 Non-goal

Phase 1 does not add new user syntax, change the ownership rules, or rewrite the
working Sema checks. It gives the existing semantics a verifiable IR form.

### 5.5 Phase 2 conformance

Phase 2 is complete at `0.539-beta`. Automatic cleanup is one `tir.drop` that
lowering expands into its release; re-owned slots register fresh obligations;
overwrites destroy a still-live owner first; and every supported scope exit
(return, multiple returns, break, continue, normal fall-through, recursion)
cleans in reverse registration order. Partial initialization relies on the zeroed
`uninit` aggregate. The analysis remains structural and straight-line rather than
a full path-sensitive discharge proof. Exception and panic unwinding are
unsupported because the language has no such control flow; `system.exit` does not
run lexical cleanup.

---

## 6. Resolved design decisions

### 6.1 Secure managed payloads

**Decision:** Phase 1 rejects `managed<T>` fields in
`memory.secure_allocate<T>` unless/until a rooted secure allocation path with
matching unregister-on-release is implemented.

**Reason:** `nrq_memory_secure_allocate` has no root registration; the current test
passes because a local managed allocation remains rooted. A general contract does
not exist, so accepting the payload is unsafe.

### 6.2 `file.read_text` strings

**Decision:** keep the process-lifetime borrowed string contract for now and
document it as an explicit exception to deterministic ownership. Do not pretend the
arena is deterministically released. An owned string design belongs to a later
standard-library phase.

### 6.3 SoA reclamation

**Decision:** keep `soa.owned_release` as root release plus asynchronous sweep.
Synchronous reclamation is a later optimization/GC change, not a Phase 1
requirement.

### 6.4 Bulk containers and shared/owned-field aggregates

**Decision:** retain Phase 8's reject-by-default policy. A later container design
must add per-element destruction or an explicit owner/arena model. No silent
exception.

### 6.5 Weak references

**Decision:** not implemented in Phase 1 or Phase 6. Add only with a concrete
standard-library requirement and a testable expiration/destruction contract.
Phase 6 reviewed the condition and found no source type, parser production or
library container that needs one. A `memory.shared_allocate<T>` payload cannot
contain a shared field, so the supported shared surface cannot close an owning
shared-to-shared cycle; cyclic graphs remain the explicit collector's domain.

### 6.6 Exceptions and panics

**Decision:** no exception model exists. Fatal exits terminate the process without
running destructors. Phase 2 may add verification for normal exits only; Gate A's
exception item stays open until an error model is designed.

### 6.7 FFI, threads and async

**Decision:** out of the current surface. Do not invent syntax or ownership rules
in Phase 1. Freeze their contracts before Phases 6–8.

---

## 7. Phase gates and order

| Phase | Target | Entry dependency |
|---|---|---|
| 0 | Audit + freeze | Complete |
| 1 | Ownership representation and verifier | This document; GC-isolation gate |
| 2 | Destruction/move validation | Phase 1 TIR contract |
| 3 | Borrow/lifetime representation | Phase 1 value/borrow metadata |
| 4 | Ordinary non-GC allocation isolation | Complete at `0.551-beta`; owned-only IR gate and mixed-path regressions pass |
| 5 | Arena scopes and bulk destruction | Complete at `0.563-beta`; tracked lifetimes, bulk invalidation, escape checks and GC-free IR pass |
| 6 | Shared completion/weak refs | Complete at `0.571-beta`; sharded atomic strong counts, final-release safety, borrow/release diagnostics, weak refs scoped out with GC cycle policy |
| 7 | GC isolation and mixed strategy | Complete at `0.575-beta`; audit, explicit entry-point contract, root/safepoint/barrier/mutator fixtures, mixed-strategy tests, O0/O2 declaration-and-dependency isolation checks and benchmarks pass |
| 8 | Manual/FFI | Complete at `0.583-beta` for the implemented manual surface; FFI is an explicitly unsupported boundary because no source ABI exists |
| 9 | Optimization/docs/stabilization | All semantic gates |

The order is intentionally not a license to rewrite completed behavior. Phases 2
and 3 mostly validate/port existing semantics; Phase 1 supplies the representation
that makes them verifiable.

---

## 8. Phase 1 entry criteria

Phase 1 implementation starts only when:

1. This target architecture is accepted as authoritative.
2. The owned-only GC regression exists and the unconditional unregister is either
   fixed or recorded as an explicit failing prerequisite.
3. The secure managed-payload decision in §6.1 is implemented as a rejection test.
4. TIR ownership/destruction/borrow fields and verifier rules are specified.
5. The exception/panic policy is documented and does not claim unwind cleanup.
6. The `0.511-beta` baseline (258 unit / 489 integration) is recorded as the
   compatibility baseline.
7. No new syntax is introduced.

---

## 9. Non-goals and preserved limitations

This freeze does **not** authorize:

- removing the collector;
- making all allocation owned by default without the IR/verifier work;
- universal reference counting;
- a new container design;
- exception/FFI/thread/async syntax;
- synchronous SoA reclamation;
- publication crash recovery;
- bounded `file.read_text` release;
- busy-registry termination;
- performance optimization before semantic verification.

The following remain explicit follow-up work:

- owned strings;
- weak references;
- per-element container destruction;
- secure managed rooting if retained;
- scoped arena escape checking and bulk object destruction;
- exception/panic cleanup;
- FFI ownership contracts;
- thread/async ownership boundaries;
- sanitizer coverage;
- refreshing historical architecture documentation.

---

## 10. Final statement

Nirqoba's current ownership semantics are substantially implemented. The migration
freeze preserves them and makes them explicit, verifiable and GC-independent where
they claim to be. The first three implementation obligations are therefore:

1. remove the collector from owned-only release;
2. give TIR a verified ownership/destruction representation;
3. reject secure managed payloads until a rooted secure contract exists.

Those are Phase 1 prerequisites, not Phase 1 scope creep.

---

## 11. Phase 9 prerequisite status

`0.515-beta` closes the three prerequisites without changing this architecture:

- **Owned-only release is collector-free.** Owned aggregate, array, pointer and view
  release emit no `nrq_gc_root_unregister` when the payload has no managed reference.
  Managed payloads retain their register/unregister pair. No source syntax changed.
- **Secure managed payloads are rejected.** `memory.secure_allocate<T>` accepts only
  payloads without managed references. There is still no rooted secure allocation
  path, and secure managed rooting remains follow-up work.
- **TIR has a verified structural ownership contract.** Value ownership is stored on
  the function, and the verifier checks ownership/type compatibility, valid release
  families, single consumption, return and borrow sources, callee parameter
  contracts and result-record consistency.

The contract is still structural and flow-insensitive. First-class move/drop TIR
operations, release-on-all-paths proofs, arena object semantics, exception cleanup
and the remaining language gaps are Phase 1 work. The migration has not started.

---

## 12. Phase 1 completion status

Phase 1 is complete at `0.531-beta` without changing this architecture:

- **Categories:** one `ValueOwnership` vocabulary with helpers classifies owning,
  borrowing, transferable and destructible values. `ArenaBound` and `Weak` are
  represented but not produced yet.
- **Transfer:** `tir.move` with a reason records every local, return, argument,
  field and element transfer; the source is invalidated and later use is rejected.
- **Borrow and lifetime:** `tir.borrow` names the owner and storage path, lexical
  borrows end at scope exit, and parameter/return lifetime relations are stored.
- **Destruction:** each owning cleanup has a `DestructionObligation` discharged by
  a `tir.drop` paired with its release call; transfer marks the obligation as owed
  by the new owner.
- **Verification:** the Phase 9 verifier is extended with `NRQ4006`-`NRQ4008`
  rules rather than duplicated.
- **Preservation:** ownership, lifetime and obligation metadata reach MLIR as
  attributes, and ownership markers do not change stack-promotion decisions.

Path-sensitive cleanup proofs, move execution semantics, arena object lifetimes,
weak references, exception cleanup and full borrow enforcement remain Phase 2 and
later work.
