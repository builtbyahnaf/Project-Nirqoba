# OHR Section D — final header implementation, audited

Scope: D1–D3 checked item by item against the roadmap's requirement lists, with the
evidence for each. Section D is closed by this document.

## D1 — one authoritative header

| requirement | status | evidence |
| --- | --- | --- |
| exact documented field layout | done | the offset/field/writer table above the struct in `runtime/src/gc.cpp`, duplicated in this audit |
| compile-time size and alignment assertions | done | `sizeof == 48`, `alignof == alignof(max_align_t)`, `sizeof % alignof(max_align_t) == 0`, and one assertion per field offset |
| explicit metadata accessors | done | `state_word`/`set_state` and the per-field accessors (`marked`, `young`, `remembered`, `age`); payload size through `object_payload_bytes`; descriptor through `nrq_gc_object_layout` |
| no implementation-defined bitfields | done | no bitfields exist; the state encoding is masks and shifts with six non-overlap assertions |
| correct descriptor references | done | interned descriptors are immutable and never freed; the memo compares against the descriptor's owned table, never the caller's |
| minimal common-path decoding | done | one word read per flag decision on the mark and barrier paths (Section B report) |
| efficient payload address calculation | done | `header + 1`, i.e. a fixed +48, no field arithmetic |
| correct payload/header recovery | done | the sweep recovers the header as `reinterpret_cast<ObjectHeader*>(payload) - 1` |
| object-start bitmap integration | done | the bitmap is indexed by payload offset / `alignof(max_align_t)`; the page asserts it starts on its own size boundary |
| correct page lookup | done | the header's `page`, the page directory for chunk lookups, and the exact-object index when the directory rejects a page |
| correct large-object fallback | done | pages above a quantum are not bitmap-walked (`use_object_index`), and their objects are found through the exact index and the unindexed chain |
| only active production representation | done | one `ObjectHeader`; no variant is compiled in, and the candidates live only under `benchmarks/` |

## D2 — collector correctness preserved

No field was removed or relocated: `mark_next`, `page` and the `next`/`tlab_next`
union are adopted fields, per the Section C2 decision record. Each preserved
behaviour below is covered by the suite that must stay green, and the suite is run
in both configurations (§D3).

| preserved behaviour | where it is exercised |
| --- | --- |
| object-start discovery | bitmap walks plus exact index; page alignment assertion |
| marking and tracing | unit suite: root, pin, barrier and shared-descriptor tests |
| mark-worklist overflow | the ring is unchanged and its spill link is intact; stress workloads exceed the 256-entry ring |
| generation and age semantics | state-word tests plus promotion behaviour in the generational tests |
| birth/mark epoch correctness | allocation stamps from the cycle counter; the sweep's retention test is unchanged |
| remembered-set behaviour | barrier and remembered-set unit tests |
| minor and major collection | generational unit tests plus integration programs |
| sweep and reclamation | reclaim paths, free-run reuse and the 342-object cascade scenario |
| page retirement and reuse | page-pool counters and the reuse tests |
| large-object handling | unindexed pages and exact-object lookup |
| concurrent mutator/collector protocol | mutator enter/safepoint/leave and the handoff tests |
| production/debug semantic equivalence | the release preset runs the same 210 unit and 263 integration tests (§D3) |

## D3 — production and debug runtime

| requirement | status | evidence |
| --- | --- | --- |
| production: direct fast paths | done | allocation validates nothing and tracing trusts the descriptor |
| production: no unnecessary diagnostics | done | every diagnostic is an explicit call (`nrq_gc_object_state_bits`, `nrq_gc_object_layout`, the snapshot API) |
| production: no repeated descriptor validation | done | `if constexpr (runtime_debug_enabled)` around every validation site, so the release build contains none of it |
| debug: validate malformed descriptors | done | `layout_is_consistent` on every caller-supplied descriptor in the development configuration |
| debug: detect invalid layout references | done | **added**: `nrq_gc_object_layout` now proves a live object's descriptor before returning it, so inspection reports header corruption instead of returning a pointer the next trace would fault on |
| debug: preserve object inspection | done | layout, raw state word, live counts, page counters |
| debug: diagnose metadata and protocol misuse | done | fatal paths for root underflow, remembered-set overflow, page live-object underflow and now an invalid layout reference |
| no different object semantics between configurations | done | same encodings, same transitions; only validation differs, and both configurations produce an identical test result |

### Measured equivalence

Built with the `windows-msvc-release` preset (`NIRQOBA_ENABLE_ASSERTS=OFF`, so
`NRQ_RUNTIME_DEBUG_ENABLED=0`), separately from the development build:

| configuration | unit | integration |
| --- | ---: | ---: |
| `msvc-dev` (asserts on, `RelWithDebInfo`) | 210 / 210 | 263 / 263 |
| `msvc-release` (asserts off, `Release`) | 210 / 210 | 263 / 263 |

`--version` reports `0.341-beta` from both. This is the equivalence evidence the
roadmap asks for: the two configurations agree across the whole suite, and the
release binary contains no validation code to agree about.

## Gaps found and closed by this audit

1. D1's documented layout existed only in reports; it is now in `gc.cpp` above the
   struct, next to the assertions that enforce it.
2. D3's "detect invalid layout references" had no implementation: allocation
   validated descriptors, but nothing checked a live object's reference. It is now
   checked in the development configuration at the inspection entry point.

## Limitation

The equivalence run is on this machine and toolchain only (MSVC 19.x, x64). It says
nothing about a target where `alignof(std::max_align_t)` is 16; the header is 48
bytes precisely so that it does not need to.
