# BO-01 — Runtime Memory-Effect Contracts (audit)

**Status:** step 1 of 6 — contract table for review; no code change yet
**Version:** 0.275-beta, production configuration (`NRQ_RUNTIME_DEBUG_ENABLED=0`)
**Date:** 2026-11-09

The compiler already ships a reviewed policy table for the non-GC runtime
(`compiler/include/nirqoba/Lowering/RuntimeAttributes.h`: math, print, fatal,
system queries, allocators, bounds checks). The GC protocol entries in section 7
of that table get `nounwind` (and sometimes `willreturn`/`nocapture`) but **no
`memory(...)` attribute**, so LLVM treats each root-protocol call as able to
read and write arbitrary memory. That is the gap this stage closes.

Every classification below is read off the current implementation in
`runtime/src/gc.cpp` (line numbers are HEAD `394ef69`). Nothing is inferred from
a function name.

## 1. Classification of the emitted GC entry points

| Entry point | Dereferences pointer args? | Runtime state touched | May allocate | May collect | May call user code | Can terminate | Proposed argmem |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `nrq_gc_root_frame_begin` | no (no pointer args) | TLS frame chain, trace ring | vector growth only | no | no | yes (`fatal`) | none |
| `nrq_gc_root_frame_end(marker)` | no (integer arg) | TLS frame chain + slot bookkeeping, `stack.mutex` | no (shrink only) | no | no | yes | none |
| `nrq_gc_root_unregister(slot)` | no — address is compared, never stored or dereferenced (`:3146`) | TLS shadow stack or global `roots` map | no | no | no | yes (underflow) | none |
| `nrq_gc_unpin(object)` | no — pin-table lookup by address (`:3610`, `unpin_value :3636`) | TLS transfer buffer, pin table | no (buffered) | no | no | no | none |
| `nrq_gc_allocate`, `_array`, `_array_typed`, `_pinned` | no pointer arguments are dereferenced | heap, allocator stats, TLAB, remembered set | yes | yes (threshold) | no | yes | none |
| `nrq_gc_allocate_array_typed_rooted` | **yes** — writes the inferred root slot it is given (`:2837`) | heap, shadow stack, root table | yes | yes | no | yes | readwrite |
| `nrq_gc_collect`, `nrq_gc_collect_minor` | no pointer args | entire heap, marks, pin table, stats | yes | (is the collection) | trace callbacks (reached via object headers, not via this call's arguments) | no | none |
| `nrq_gc_safepoint`, `mutator_enter`, `mutator_leave` | no | TLS mutator state, collector handshake | no | parks | no | no | none |
| `nrq_gc_enable`, `disable`, `set_threshold`, `init`, `shutdown` | no | global flags, heap setup | yes | no | no | yes | none |
| `nrq_gc_get_*`, `pin_count`, `pin_violations`, `is_enabled`, `transfer_pin_fallbacks` | no | counters, `gc_mutex` | no | no | no | no | none |
| `nrq_gc_root_store(slot, value)` | **yes** — stores through `slot`, reads the referent's header through `value` for the barrier (`:3172`) | shadow stack, remembered set | no | no | no | no | readwrite |
| `nrq_gc_root_store_release(slot, value)` | **yes** — same store plus release (`:3619`) | shadow stack, remembered set, pin table | no | no | no | no | readwrite |
| `nrq_gc_root_copy(dst, src, bytes)` | **yes** — `memcpy` both directions (`:3220`) | remembered set | no | no | no | no | readwrite |
| `nrq_gc_root_register`, `_typed`, `_frame`, `_frame_static` | **yes** — reads slot addresses out of `slots`, reads `descriptor`, may mark slot referents (`:2878`, `:2950`, `:3074`, `:3086`) | TLS shadow stack, root metadata | yes (metadata vectors) | no | no | yes | readwrite |
| `nrq_gc_root_register_array`, `clear_array_references` | **yes** — walks `data` (`:3092`, `:3122`) | global `roots` map | yes | no | no | yes | readwrite |
| `nrq_gc_pin`, `nrq_gc_pin_transfer` | **yes** — `find_object_locked`/`mark_value_locked` read (and during a cycle mark) the object header through `object` (`:3565`, `:3663`) | heap marks, pin table | yes (map insert) | no | no | yes | readwrite |
| `nrq_gc_pointer_is_managed` | **yes** — header/index lookup (`:3520`) | none | no | no | no | no | read only |

`nrq_gc_debug_declare_site` / `nrq_gc_debug_tag_allocation` already have
contracts and are left as they are.

## 2. The effects that are *not* narrowed

- `other` stays `ModRef` for every entry in this table. All of them read or
  write TLS or file-static runtime state (shadow stack, pin table, heap pages,
  statistics), so claiming a read-only or side-effect-free call would be false.
  The only narrowing claimed is `argmem`, plus `errno`/`target_mem` being `none`
  (no GC entry point reads or sets `errno`, and none of them touches target
  extension memory).
- `inaccessiblemem` stays `ModRef` for the entries that allocate or collect:
  their bookkeeping includes memory the program cannot name (page and span
  tables, allocator metadata), and this stage has no proof to claim otherwise.
  For the pure bookkeeping entries it is also left `ModRef` rather than
  introducing a second, weaker claim that buys nothing measurable.
- Nothing is marked `returns_twice`, `nofree`, `nosync`, `willreturn` (where it
  is not already true) or `readonly`/`writeonly`. `nrq_gc_pin_transfer` is *not*
  marked `argmem: none`: it consults `find_object_locked` and marks during a
  cycle, so it genuinely reads and writes the referent.

## 3. Why this is the narrowest provable contract

`memory(argmem: ...)` means "memory reachable through this call's pointer
arguments". A call with no dereferenced pointer arguments therefore cannot be
the reason a caller's local `alloca` is reloaded, and the compiler is free to
keep values in registers across it. That is the entire claim: the argument
classification is read off each implementation, not guessed, and every entry
stays `ModRef` on global/TLS memory so the collector protocol remains fully
visible.

## 4. Verification plan (executed in the later steps of this stage)

1. Emitted IR carries the expected `memory(...)` on each changed declaration.
2. The null slot initialisation that is read by `nrq_gc_root_store` survives;
   the *unrelated* stores around the frame protocol are free to move.
3. A genuinely dead store may be removed where the contract proves it
   unobservable.
4. Global-state effects remain visible (no `memory(none)` on any GC entry).
5. Debug and production builds emit the same declaration effects (the policy
   table is shared; only the ABI marker symbol differs).
6. Focused execution: Stress 02 and 03 built and run in the production
   configuration must reproduce their baseline semantic output.
