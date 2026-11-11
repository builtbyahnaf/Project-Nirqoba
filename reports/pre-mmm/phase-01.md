# Pre-MMM Phase 1 - Ownership inside aggregates

**Status:** In progress
**Version:** 0.397-beta
**Date:** 2026-11-11

## Implemented

- Borrowed pointer fields (`borrow`, `borrow mut`), 0.384-beta: construction requires a
  borrow, immutable fields are read-only through the borrow, and no destruction is
  generated because the field owns nothing.
- Owned pointer fields (`owned ptr T`), 0.389-beta: the field type is `owned_ptr<T>`,
  so aggregates holding one are move-only, copies invalidate the source, and
  `aggregate.owned_drop` releases the pointer at scope exit. Construction accepts a
  producing expression or transfers an owned local, marking it moved.
- Field destruction now runs in reverse declaration order.
- Reinitializing a moved owned field from a fresh owner (`h.data = memory.allocate<int>(n)`)
  works, with the same raw-to-owned coercion as construction (0.390-beta).
- Aggregates that own pointer fields may be nested; recursive destruction drops them,
  and owned arrays whose elements own resources remain rejected (0.390-beta).
- `shared ptr T` fields use `TypeKind::SharedPointer`: construction takes the initial
  reference, copies retain every shared field (`aggregate.shared_retain`), moves
  transfer without retaining, and destruction releases once per field at any nesting
  depth (0.391-beta).
- `var h: T = uninit;` declares a zero-initialized aggregate, tracks each field
  as not live until assigned, and destroys only initialized fields because untouched
  fields are null (0.396-beta). Whole-value uses of a partially initialized aggregate
  are rejected, and overwriting a partially initialized nested aggregate is rejected
  instead of leaking (0.397-beta).
- Owned arrays as struct fields work (parser capability from 0.384).

## Open

- `shared` pointer fields: need reference-count release at field destruction.
- Resource-domain owned fields (`owned file ptr byte`): need per-domain cleanup
  (file.close, socket.close, mutex.destroy, ...) carried into lowering.
- `uninit` partial initialization and partial destruction.
- Whole-aggregate moves already work; field-level moves and reinitialization verified.

## Tests

- `positive/owned_field_destruction`: allocation and local transfer into owned fields,
  values read back (12, 3), program exits cleanly.
- `negative/owned_field_use_after_move`: copying the aggregate then reading the source
  is rejected with NRQ3033.
- `negative/owned_field_requires_owner`: an owned field initialised from a borrow is
  rejected.
- `positive/owned_field_reinitialization`: move a field out, reinitialize it, and read
  both values (11).
- `positive/owned_field_nested_destruction`: nested struct field owning a pointer,
  read through the aggregate (10).
- `positive/shared_field_copy_scope`: copy, nested field access and reference counts
  across a scope exit (2, 14, 1).
- `positive/shared_field_move_owned`: moving an owning aggregate transfers the shared
  reference without retaining (1).
- `positive/uninit_partial_initialization`: fields initialized one at a time, then
  released (9).
- `positive/uninit_nested_cleanup`: nested aggregate assigned as a whole, recursive
  release (8).
- `positive/uninit_moved_field_reinit`: a field initialized, moved out and
  reinitialized (12).
- `negative/uninit_read_before_init`: reading an uninitialized field is NRQ3033.
- `negative/uninit_requires_var`: `let`/`const` partial aggregates are rejected.
- `negative/uninit_whole_value_copy`: copying a partial aggregate is rejected.
- `negative/uninit_whole_value_move`: moving a partial owned aggregate is rejected.
- `negative/uninit_whole_value_return`: returning a partial aggregate is rejected.
- `negative/uninit_whole_value_argument`: passing a partial aggregate is rejected.
- `negative/uninit_partial_nested_overwrite`: overwriting a partially initialized
  nested aggregate is rejected.
- `positive/uninit_full_then_copy_ok`: a fully initialized aggregate can be copied
  (shared count 2, value 6).
- `positive/uninit_nested_fieldwise_ok`: nested fields initialized one by one (9).
- `positive/shared_field_construction`: construction from `memory.shared_allocate`
  starts at count 1.
