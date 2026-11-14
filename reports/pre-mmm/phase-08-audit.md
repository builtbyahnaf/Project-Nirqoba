# Pre-MMM Phase 8 — Supported-surface ownership audit

**Baseline:** `0.499-beta` (`c0ddcfd`)  
**Audited tree:** `0.511-beta`  
**Scope:** completed ownership types and operations only; no new language features.

## 1. Method

The audit started from the Phase 7 verdict and read the ownership implementation
rather than assuming a policy from the README. It covered:

- `TypeChecker` producer recognition, binding/transfer/return/parameter rules,
  aggregate field contracts, borrow tracking and cleanup-domain checks.
- `TirToMlir` cleanup emission, aggregate drop/retain, typed GC roots, owned-array
  release, SoA release and allocation lowering.
- `runtime/src/gc.cpp` automatic-collection defaults and collector lifecycle.
- The supported compiler surface (`grep` over the call dispatcher), all integration
  tests, `docs/memory/management.md`, `README.md`, `docs/readiness.md` and the
  Phase 7 reports.
- Small compile/run probes for collector arrays, explicit storage, shared storage,
  owned arrays, SoA columns, managed payloads and nested generics.

The probes mattered: the documentation claimed several boundaries were rejected
when the checker accepted them, and one natural generic spelling did not parse.

## 2. Inventory at `0.499-beta`

### 2.1 Types

| Type | Ownership role | Actual baseline behavior | Audit result |
|---|---|---|---|
| Scalars, `char`, `bool`, `string` | trivial / borrowed | `string` is immutable process-lifetime storage from `file.read_text`; no per-value destructor | Correct for the documented borrowed role; deferred limitation retained |
| Fixed array, tuple, struct | lexical aggregate | Struct fields declare `owned`, `owned <resource>`, `shared`, `borrow` or `borrow mut`; tuples reject owned/shared/borrowed pointer elements; unqualified raw pointer fields rejected | Correct; no redesign |
| `owned_array<T>`, `owned_array_aligned<T>` | deterministic bulk owner | `array.owned_release` frees the element block; nested owned arrays/pointers rejected | Correct for owned payloads; shared/borrowed elements were a gap |
| `owned ptr T`, resource pointers | deterministic single owner | Bound producer cleanup, move on transfer, recursive aggregate drop, domain-specific close/release | Correct |
| `managed<T>` | collector reference | Non-owning reference into collector storage; typed roots when live inside owned/explicit/shared storage | Correct |
| `shared ptr T` | reference-counted owner | Retain/release on copy/destruction; shared parameters/returns; inline roots for managed pointees | Correct; aggregate support was under-documented |
| `ptr T` / `borrow T*` / `borrow mut T*` | raw / borrowed view | Unqualified pointer parameters/returns are owning by default; `&owner` and borrow parameters carry tracked lifetimes; borrowed returns are source-bound | Correct |
| `soa<T>` | collector-owned strided container | SoA allocation creates one collector object; managed columns traced; `soa.owned_release` unregisters the root | Correct for owned/managed columns; shared/borrowed columns were a gap |
| `array.allocate<T>` result | collector allocation | Collector allocation is explicit; automatic collection starts disabled | Correct opt-in path |

### 2.2 Producers and cleanup domains

| Producer | Cleanup at lexical exit | Transfer behavior | Audit result |
|---|---|---|---|
| `memory.allocate<T>` | `memory.deallocate` | moves, returns, owning parameters | Correct |
| `memory.reallocate<T>` | `memory.deallocate` | source owner transferred into the resize | Correct |
| `memory.secure_allocate<T>` | `memory.secure_release` | domain-checked | Correct |
| `memory.owned_array<T>` / `_aligned` | `array.owned_release` | move-only descriptor | Correct |
| `memory.owned_soa<T>` | `soa.owned_release` (unregister, sweep later) | move-only descriptor | Correct behavior; producer map was incomplete |
| `memory.shared_allocate<T>` / `shared_retain` | `memory.shared_release` | token moved through shared parameters/returns | Correct |
| `memory.map_file` | `memory.unmap_file` | domain-checked | Correct |
| `file.open`, `socket.connect`, `mutex.create` | `file.close`, `socket.close`, `mutex.destroy` | domain-checked | Correct |
| `array.allocate<T>` | collector sweep only | managed references traced; no finalizer | Correct opt-in GC behavior |

### 2.3 Consumers

| Consumer | Baseline behavior | Audit result |
|---|---|---|
| Owned pointer/array parameter | Requires an owned value, moves the source, rejects borrows/shared and live-view conflicts | Correct |
| Borrow parameter / borrowed return | Source-bound, path-aware, mutable-exclusivity checked | Correct |
| Shared parameter/return | Transfers one reference token; rejects unique/borrowed aliases | Correct |
| Aggregate field initialization | Checks field contract, domain, move legality and borrow lifetime | Correct |
| Aggregate copy/drop | Retains/copies shared fields; recursively drops owned fields; roots managed fields while live | Correct |
| Explicit `defer` cleanup | Overrides inferred cleanup for its owner; tracks moves after defer | Correct |
| `memory.deallocate` / `array.owned_release` / `shared_release` | Reject wrong domain, live borrows and shared/unique confusion | Correct |

## 3. Confirmed gaps

These were accepted by the baseline compiler even though the receiving storage
could not honour the value's ownership contract:

1. **Collector arrays accepted deterministic payloads.** `array.allocate<Holder>`
   accepted a struct with `shared`, owned or borrowed fields. Sweep has no finalizer,
   so the field token was silently dropped.
2. **Explicit, reallocated and secure storage accepted deterministic payloads.**
   `memory.allocate<Holder>`, `memory.reallocate<Holder>` and
   `memory.secure_allocate<Holder>` accepted owning/shared/borrowed fields even though
   release only frees raw bytes.
3. **Shared allocations accepted deterministic payloads.** The final
   `memory.shared_release` freed the block without walking fields.
4. **Owned arrays accepted shared and borrowed payloads.** `array.owned_release`
   freed the element block without per-element field destruction.
5. **SoA accepted shared and borrowed columns.** `soa.owned_release` unregisters the
   container root and never walks column destructors.
6. **Nested generic closers required a space.** `owned_array<managed<T>>` failed to
   parse; `> >` worked. The type system already represented the combination.
7. **Legacy runtime names remained source builtins.** `nrq_gc_*` and untyped
   `nrq_memory_*` bypassed the namespaced/typed policy paths.
8. **Documentation contradicted the checker.** It said raw pointer fields were
   non-owning and shared aggregates were unsupported, while the current tree does the
   opposite: raw pointer fields require a qualifier and shared fields work.

## 4. Confirmed correct behavior (not redesigned)

- `memory.allocate<T>` already produced deterministic lexical cleanup and correct
  move/return/parameter transfers.
- Owned arrays and SoA already had correct release, view lifetime and typed-root
  behavior for their supported payloads.
- Shared pointers already had correct token transfer and aggregate retain/release.
- Borrowed pointers, views and source-bound returns already had path-aware lifetime
  enforcement.
- Collector rooting already traced managed fields in owned pointers, owned arrays,
  shared allocations, SoA and aggregates.
- Automatic collection already started disabled, and explicit `gc.collect()` already
  worked without enabling automatic triggers.

These subsystems were preserved.

## 5. Decisions

- **Reject, do not fall back.** A payload that needs destructor work is rejected at a
  storage boundary that cannot run it. No rejected payload is redirected to the GC.
- **Managed references stay supported.** They require no field destructor and have
  typed-root metadata at every accepted boundary.
- **No container redesign.** The audit did not add per-element finalizers, new
  refcounted containers or new public syntax. The rules constrain existing paths.
- **Proportional coverage.** Each policy rule has a focused positive or negative test;
  the full suite runs at milestones and at the phase boundary.
- **Previously deferred limitations stay separate.** Process-lifetime read strings,
  publication crash recovery and busy-registry termination are not changed by Phase 8.

## 6. Result

The supported surface now has one consistent rule: deterministic ownership and
borrowing are the default, every supported lexical aggregate runs its declared field
contract, every non-destructing storage boundary rejects deterministic owners, and the
collector is available only through explicit managed allocation and explicit GC
controls.
