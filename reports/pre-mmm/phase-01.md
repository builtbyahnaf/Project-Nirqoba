# Pre-MMM Phase 1 - Ownership inside aggregates

**Status:** In progress
**Version:** 0.390-beta
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
