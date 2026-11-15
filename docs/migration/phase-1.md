# Phase 1 — Ownership Representation Notes

**Version:** `0.531-beta`  
**Scope:** Memory Model Migration Phase 1. No new syntax.

Phase 1 changes the compiler's internal ownership model, not the language you
write. Programs that compiled at `0.515-beta` compile and run the same way,
unless the compiler was producing malformed TIR internally.

## What changed

- Every TIR value, parameter and return has an explicit ownership category.
- Ownership transfers are represented by `tir.move`, with a reason.
- Borrows are represented by `tir.borrow`/`tir.end_borrow`, with owner, path,
  mutability and a lifetime record.
- Lexical cleanups register a destruction obligation, discharge it with
  `tir.drop`, and are paired with the release call they name.
- The verifier rejects inconsistent ownership metadata before lowering.
- Lowering preserves lifetime and obligation metadata as MLIR attributes.

## What did not change

- No move, borrow or destruction syntax was added.
- Move semantics, deterministic destruction ordering and path-sensitive cleanup
  proofs are Phase 2.
- Full lifetime inference and escape enforcement are Phase 3.
- Ordinary allocation is still collector-backed; the allocation migration is
  Phase 4.
- GC programs remain supported and unchanged.

## Diagnostics

User-visible ownership errors keep their Sema codes (`NRQ3015`, `NRQ3033`).
The new codes are internal compiler contract failures:

| Code | Contract |
|---|---|
| `NRQ4006` | Ownership category or transfer is inconsistent |
| `NRQ4007` | Borrow or lifetime relationship is inconsistent |
| `NRQ4008` | Destruction obligation or drop/release pairing is inconsistent |

See `docs/memory/ownership-representation.md` for the full representation
contract.
