# PO-14 — Review Hardening (F2–F6)

**Task:** follow-up hardening of the PO 3.1 / 3.2 review findings
**Date:** 2026-11-04
**Status:** in progress — one stage per finding, each with an implementation commit
and a verification commit
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, MSVC x64 RelWithDebInfo

The PO 3.1/3.2 review of the ownership, alias and escape work produced six
findings. F1 (stack slots materialised at their use site) was fixed as part of
PO 3.5. This report covers F2–F6, one stage each.

---

## F2 — `nocapture` and `readonly` were attached from ownership, not from analysis

**Status:** fixed.

### Problem

Two parameter attributes were asserted from the parameter's ownership class
alone:

```cpp
} else if (param.ownership == ValueOwnership::Borrowed) {
  attrs.set(readonly);                                   // always
  if (param.name != tir_fn->return_borrow_parameter())
    attrs.set(nocapture);                                // always
}
```

Both attributes are statements about the *instructions in the body*, not about
the type: `nocapture` says the function does not copy the pointer into memory
that outlives the call, `readonly` says it does not write through it. The
compiler had a dedicated analysis for exactly the first question —
`summarize_borrowed_parameters`, a greatest fixpoint over the call graph — and
threaded its result into `FunctionTranslator`, where it was used only to decide
stack promotion. It was never consulted for the attributes. For `readonly`
there was no analysis at all.

### Root cause

`translate_tir_to_mlir` builds the argument attribute dictionaries before it
constructs the translator, from `tir_fn->parameters()` only. The escape summary
was in scope at that point (`borrowed_summary`, computed once per module) but
nothing made the connection, so the attribute block degenerated into a mapping
from `ValueOwnership` to a fixed set of attributes.

### Change

- `nocapture` for `Borrowed` and `MutableBorrowed` parameters is now
  `borrowed_parameter_does_not_escape(fn, index, borrowed_summary)`, i.e. the
  existing fixpoint. A returned borrow is still excluded (the predicate already
  handles `return_borrow_parameter`).
- `readonly` for `Borrowed` parameters is now
  `parameter_is_never_stored_through(fn, index)`: a local scan that rejects any
  `Store` using the parameter, a pointer derived from it, or the pointer
  reloaded from the slot it was spilled into as the destination. Returning the
  pointer is allowed, because returning is not writing.
- `runtime_call_consumes_borrow_without_retaining` replaces the hard-coded print
  whitelist with one table that also covers the resource handles
  (`file.read/write/…`, `socket.send/receive/close`, `mutex.lock/unlock/destroy`)
  and the string printers. Those operations use the pointer for the duration of
  the call and never retain it, which is what the escape fixpoint needs to know.
  Anything missing from the table is still treated as an escape — the safe
  direction, where the attribute is withheld rather than wrongly claimed.

### Why it is correct

- Both attributes can now only appear when a function-local or interprocedural
  argument produces them; no ownership class alone can create one.
- A callee cannot write through an immutable borrow either, because handing an
  immutable borrow to a mutable or owning parameter is rejected before lowering
  (`NRQ3015` and the mutable-borrow rules), so the local write scan is
  sufficient for `readonly`.
- The escape fixpoint is unchanged and already conservative: unknown operations,
  unknown callees and stores of the pointer all fail it.
- `noalias` keeps following from ownership, which is what the language actually
  guarantees: a mutable borrow or an owned pointer is the only access path the
  caller has to that memory for the duration of the call.

### Verification

The emitted attribute lines (`define`/`declare`/`attributes`) were captured for
all 108 integration programs before and after the change and diffed:

```
$ build/tmp_liveness/dump_attrs.sh before && … && diff -r before after
(no differences)
```

No program lost or gained a promise, which is the intended result: the
attributes were *correct* before, but only because the type checker closed the
holes that the analysis was there to close. They are now derived from the
analyses that justify them.

New tests pin the derivation:

- `tests/integration/mlir/borrow_attribute_readonly/` — a function whose body
  reads through one borrow and writes through another must still emit
  `ptr readonly captures(none)` for the reader.
- `tests/integration/mlir/borrow_attribute_returned/` — a function that returns
  its borrow keeps `llvm.readonly` and must **not** claim `llvm.nocapture`.
- `tests/integration/mlir/borrow_attribute_resource/` — passing a borrowed
  handle to `file.write` does not disqualify it, because the resource operations
  do not retain the handle.

Suite results after the change: 118/118 unit tests and 192/193 integration tests
pass (the failure is the pre-existing `memory_advanced` version-string drift).

### Known limitations

- The escape fixpoint cannot express "does not escape" for a program that is not
  expressible in the language today: pointer-to-pointer parameters, pointers
  stored in aggregate fields and borrow→owning transfers are all rejected by the
  checker, so no test can distinguish the old code from the new one. The change
  is defensive: it removes the *possibility* of the attributes being right by
  accident.
- `runtime_call_consumes_borrow_without_retaining` is a curated list and an
  assertion about the runtime ABI. A new resource operation that is missing from
  it loses `nocapture` (pessimisation) rather than gaining a false promise.

---

## F3 — The escape scan could not tell a scalar store from a published pointer

**Status:** fixed.

### Problem

`borrowed_pointer_parameter_does_not_escape` accepted every store whose
destination was the borrowed pointer and whose value was not pointer-derived:

```cpp
// Storing data through pointer: lhs is data pointer, operand is not a pointer
if (data_pointers.count(op.lhs.id) && !data_pointers.count(op.operand.id))
  continue;
```

The comment states the intent — "the operand is not a pointer" — but the
condition never checked it. `data_pointers` is the set of values *derived from
this parameter*, so an unrelated pointer, a string, a managed reference or an
aggregate all satisfy `!data_pointers.count(operand)` and were treated as plain
data. Writing a pointer through a borrowed handle publishes it into memory the
caller can still observe, which is an escape by any reading of `nocapture`.

### Root cause

The predicate answers two different questions with one test: "is the destination
the borrowed pointer?" (about capture of *this* argument) and "is the value
being moved plain data?" (about what is being published). Only the first was
checked.

### Change

`store_moves_pointer_free_data(op)` now decides the second question: a store is
plain data only when the value is a number, boolean or character, has no
aggregate elements and is valid. Anything else falls through to `return false`,
i.e. the parameter escapes and loses `nocapture` (and, with F2, cannot be
promoted on the strength of this summary).

The `array_view` variant of the analysis was already correct — it marks a store
whose operand is a data pointer as an escape — so the two variants now agree.

### Why it is correct

- The rule is a strict weakening of the old one: a program that satisfied the
  old condition with a scalar still satisfies the new condition, and every case
  that changed verdict was a pointer-carrying value, which is exactly the case
  the old comment intended to exclude.
- The emitted ``nocapture``/``readonly`` attributes are unchanged across all 108
  integration programs (verified with the same attribute diff as F2), because
  the programs that would exercise the removed hole are rejected by the checker
  before lowering.

### Verification

- `tests/integration/negative/borrowed_view_pointer_store/` pins the checker
  invariant the analysis leans on: storing a pointer through a borrowed view is
  rejected (`owned, shared, and borrowed pointers cannot be stored in aggregate
  fields`). Without that invariant the analysis would have to be far more
  conservative.
- Attribute diff over the whole suite: no differences.
- Suite results: 118/118 unit tests and 193/194 integration tests pass
  (`memory_advanced` version-string drift is pre-existing).

### Known limitations

- The hole is not reachable through the current language, so no program can
  distinguish the old analysis from the new one. The fix removes the reliance on
  a comment for a correctness property.
- `store_moves_pointer_free_data` decides by TIR type. A future opaque
  scalar type would need an explicit entry here, and the safe default is
  "not data".

---

## F4 — `analyze_stack_allocations` was quadratic (in practice cubic) in the body

**Status:** fixed.

### Problem

The stack-promotion analysis ran up to four scans of the whole function *per
candidate allocation*: one to find the allocas, one to find the stores of the
candidate, one to find the loads from the owner slot and one escape scan. The
escape scan is the expensive one, because it tested every descriptor against
every operation:

```cpp
for (const tir::Op& op : function.operations()) {
  ...
  bool uses_descriptor = false;
  for (int id : descriptors) uses_descriptor |= uses(id);
  bool uses_data = false;
  for (int id : data_pointers) uses_data |= uses(id);
```

`descriptors` is not a small set. The scan adds a descriptor for *every* load
from any slot that holds an array view, so in a function that reads through
many views the set grows with the number of views, and the inner loop grows with
it. Cost was Θ(candidates × operations × descriptors), which for
`A` allocations and `V` views is Θ(A·N·V).

### Root cause

Two independent mistakes in the same function, with the second hidden behind the
first:

1. Per-candidate rescans of the body for information that does not depend on the
   candidate (the alloca slots, the stores of each value, the loads from each
   slot, the set of view slots).
2. Inside the escape scan, the set-membership question "does this operation
   reference a descriptor" was answered by iterating the descriptor set and
   testing each element, instead of testing the handful of values the operation
   references.

The analysis is dominance-free and its verdict is a plain fixpoint over one
forward pass, so both can be answered in constant time per operation.

### Change

- The function body is indexed once: `alloca_slots`, `stores_by_value`,
  `loads_by_slot` and `view_slots`. The owned-array branch, the raw-pointer
  branch and the alignment/frame accounting all read the indexes instead of
  rescanning. The raw-pointer branch also stops using `std::any_of` for the
  owner-slot question and reuses `alloca_slots`/`stores_by_value`.
- A new `references_any(op, values)` helper checks the values an operation
  actually references — `operand`, `lhs`, `rhs`, `arguments`, `aggregate_elements`
  — against the descriptor and data-pointer sets by hash lookup. The scan costs
  Θ(references per operation) instead of Θ(|descriptors| × references per
  operation), so the whole analysis is Θ(candidates × operations) = Θ(A·N).

### Why it is correct

`references_any(op, values)` is *exactly* the old
`any_of(values, [&](id){ return uses(op, id); })`: both answer "does `op`
reference some member of `values`". The set of ids in `values` is built in the
same order and at the same point in the scan (the view-load insertion still
happens before the membership test), so the fixpoint visits the same states and
produces the same verdict. Nothing about promotion, alignment or frame budget
changed.

### Verification

The measurement program is synthetic but of a shape the language allows: `A`
`memory.owned_array<int>(4)` allocations, each read through its own
`memory.array_view`, one `total += view[0]` per pair. Timed with
`--emit=mlir -O2` on the development machine (MSVC RelWithDebInfo):

| shape | before F4 | after indexing only | after this change |
| 300 arrays + 300 views | ~2.2 s | ~2.2 s | 0.9 s |
| 600 arrays + 600 views | 9.0 s | 8.4 s | 1.7 s |

The indexing stage alone barely moved because it was not the hot spot. Localising
the cost first (`--emit=tir` was 123 ms and `--emit=mlir` 11.3 s, so the time was
all in lowering) is what exposed the descriptor loop as the real quadratic term.
These are single-run wall-clock numbers on a shared machine, not a benchmark
suite; the shape of the curve is the point, not the last digit.

Behaviour is unchanged where it matters:

- All 119 unit tests pass, including the new
  `MlirPipelineTest.PromotesEveryArrayReadThroughLocalViews`, which lowers 24
  arrays with 24 views and asserts that every array still becomes a stack slot
  and that no `nrq_memory_allocate*` call remains.
- All 194 integration tests run, 193 pass (`memory_advanced` version-string drift
  is pre-existing). The `stack_placed_*` positives still promote and the
  escape-rejection tests still reject.
- The attribute diff over all 108 integration programs is empty, so no function
  changed its memory-effect or capture promises.

### Known limitations

- The analysis is still per-candidate rather than a single fixpoint over all
  candidates at once, so it remains Θ(A·N). At the frame-budget limit
  (`16384` bytes of promoted storage) `A` is bounded by a few thousand
  allocations, which keeps that term small in practice.
- The measurement is a wall-clock comparison on one machine, not a benchmark
  harness; it is reported to show the asymptotic change, not a speedup claim for
  real programs, whose compile time is dominated elsewhere.
