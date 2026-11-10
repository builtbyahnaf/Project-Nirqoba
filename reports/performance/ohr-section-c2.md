# OHR Section C2 — candidate headers, measured

Scope: the four candidate sizes (48 current, 40, 36, 32), what each costs in
alignment and page occupancy, and what resolving an object's page would cost if the
`page` field were removed. 24 bytes was withdrawn before testing and is not
evaluated. Everything below is measured on this machine with MSVC at `/O2`; the
span formula was first validated against the runtime's own allocation profile.

## 1. Geometry, as the compiler lays it out

Reproduced by `benchmarks/bottleneck_discovery/header_probe/variants.cpp`, which
links the runtime and checks the span formula against
`nrq_gc_get_allocation_profile` for ten payload sizes. MEASURED: the formula
matches the runtime on all ten, so the same arithmetic is used below for the
candidates the runtime cannot yet build.

| candidate | fields | used | `sizeof` | `alignof` | 8-byte align | 16-byte align |
| --- | --- | ---: | ---: | ---: | --- | --- |
| **H48** | next, mark_next, page, epoch64, len, state, layout | 48 | 48 | 8 | ok | ok |
| **H40** | drop `page` | 40 | 40 | 8 | ok | **fails** |
| **H36** | drop `page`, epoch32 | 36 | **40** | 8 | ok | **fails** |
| **H32** | drop `page` and `mark_next` | 32 | 32 | 8 | ok | ok |

**H36 is not a real size.** 36 used bytes round to `sizeof` 40 because the struct's
alignment is 8, so it carries 4 bytes of trailing padding and lands on exactly the
same spans as H40, while additionally narrowing the birth epoch to 32 bits. It buys
nothing and costs wrap behaviour, so it is recorded as measured and rejected.

H40 is legal only where `alignof(std::max_align_t)` is 8 (MSVC). On a target where
it is 16 (libstdc++), a 40-byte header puts every payload at an 8-byte-misaligned
address. H32 and H48 satisfy both.

## 2. Occupancy and footprint

Spans and objects per 65,536-byte page, at 8-byte object start alignment:

| candidate | p=8 span / slots | p=64 span / slots | p=512 span / slots |
| --- | --- | --- | --- |
| H48 | 56 / 1170 | 112 / 585 | 560 / 117 |
| H40 | 48 / 1365 | 104 / 630 | 552 / 118 |
| H36 | 48 / 1365 | 104 / 630 | 552 / 118 |
| H32 | 40 / 1638 | 96 / 682 | 544 / 120 |

Footprint for 100,000 objects (MiB):

| candidate | p=8 | p=64 | p=512 |
| --- | ---: | ---: | ---: |
| H48 | 5.34 | 10.68 | 53.41 |
| H40 | 4.58 | 9.92 | 52.64 |
| H32 | 3.81 | 9.16 | 51.88 |

H48's p=8 and p=64 rows are the runtime's own reported numbers; the others are the
same formula the runtime uses. The header's share of an 8-byte object's span falls
from 86% (48/56) to 83% (40/48) to 80% (32/40). For a 512-byte payload it is noise
in all three cases.

At 16-byte alignment the numbers move: H48 spans 64 / 112 / 560, H32 spans
48 / 96 / 544, and H40 does not exist as an option at all.

## 3. What `page_of()` would cost

The `page` field is read 18 times (see the field audit) and written exactly once at
construction, which is what makes it the derivation candidate. Measured by
`benchmarks/bottleneck_discovery/header_probe/page_of_cost.cpp` over 4.2 million
addresses, minimum of three rounds, two runs:

| strategy | ns per resolution | correct for large pages? |
| --- | ---: | --- |
| header field (today, 48-byte stride) | 2.15 – 2.75 | yes |
| mask only | 0.43 – 0.53 | **no** |
| mask + small hash probe (empty or 8 entries) | 0.92 – 1.19 | no |
| mask + range search over 8 large pages | 2.53 – 2.75 ordinary, 4.89 – 5.21 large | yes |
| mask + quantum map, 512 entries | **60.48** | yes, but disqualified |
| header field at 32-byte stride | 1.49 – 1.60 | yes |

Three findings:

- The mask alone is 4–5× cheaper than the field read, and it is **wrong** for any
  page larger than 64 KiB, because the masked base is inside such a page rather
  than at its start.
- The cheapest correct fallback measured is a range search, and it costs as much as
  the field read on ordinary pages (2.53–2.75 ns) and twice as much on large-object
  pages (4.89–5.21 ns). My first correct-and-cheap idea — a hash table keyed by the
  masked base with one entry per 64 KiB of each large page — measured 60 ns because
  a 512-entry table at that load factor gives long probe chains. A properly sized
  table would fix the constant, but nothing in this measurement shows it beating
  the field read.
- The per-object amortisation is what actually matters: the sweep reclaim path
  touches `header->page` 8 times for one object (audit §page). Resolving once per
  object and keeping it in a local would pay one resolution, so the field is not
  expensive because of its 18 sites — it is expensive because of its 8 bytes in
  every span.

In-context cost is not measured. The microbenchmark reads addresses in a streaming
pattern; a real sweep already has the header in cache when it wants the page, and a
registry probe adds a second cache line that may be cold. That is a risk the
numbers above do not capture, in the direction of making `page_of` look better than
it is.

## 4. Decision

Per the rule that a field only goes when its complete replacement is cheaper and at
least as reliable:

- **`mark_next` stays.** Its replacement candidates are an auxiliary link array (the
  same 8 bytes, worse locality), cycle-transient storage (allocates during marking),
  or draining on overflow (pathological recursion). None has a demonstrated
  advantage; the first has none by construction.
- **`next` / `tlab_next` stays.** Its users are persistent lists, so side storage is
  resident rather than transient, and publication would go from one splice to a
  write per object.
- **`page` stays for now.** The only correct cheap-enough replacement (range search)
  measured at or above the cost of the field it would remove, and the derivation is
  only legal for ordinary pages. H40 remains a recorded candidate for a target where
  `max_align_t` is 8 and where the sweep hoists one resolution per object; it is not
  adopted on these measurements.
- **H32 is the only smaller portable header**, and reaching it requires removing
  `mark_next`, which §4.1 rejects on the current evidence.
- **H36 and 24 are recorded as rejected**, the first because 36 used bytes are 40
  physical bytes, the second withdrawn before testing.

## 5. Verification

- 210 unit, 263 integration, passing at 0.341-beta.
- Span formula cross-checked against `nrq_gc_get_allocation_profile` on ten
  payload sizes: matches.
- `sizeof`/`alignof`/`offsetof` are compiler output for the candidate structs, not
  arithmetic done by hand.

## 6. Decision record — closed

This investigation is **closed**. The outcome is a decision, not a backlog item:

- **H48 is the authoritative production header.** `page`, `mark_next` and the
  `next`/`tlab_next` union are adopted permanently on this architecture. They are
  not placeholders awaiting a replacement, and no code path is to be written in a
  way that assumes their removal.
- **H32 is rejected**, not deferred. Its 16-byte reduction is not sufficient
  justification for relocating mark-worklist storage out of the header or for
  adding page-resolution work to paths that currently load one pointer. The
  measurements behind that rejection are in §1–§3: the only correct cheap-enough
  `page_of` fallback measured at or above the cost of the field it would remove,
  and every `mark_next` replacement either costs the same bytes elsewhere or
  allocates during marking. It stays in this document as a rejected candidate for
  the collector as it exists, not as unfinished implementation work.
- **H40 is documented only.** It is a legal target-specific optimisation where
  `alignof(std::max_align_t)` is 8, and it is not implemented now. Adopting it would
  require a per-target header size and a sweep that hoists one page resolution per
  object; neither exists, and the microbenchmarks above do not justify either.
- **Header-size optimisation is closed** unless a new measurement or a materially
  different collector design changes the trade-off. A future attempt should start
  from the numbers in this document rather than from the target percentage.

The object-representation work therefore continues with H48 as the fixed header, and
the remaining effort goes to implementation: compiler-emitted descriptors, so
generated code allocates through a read-only descriptor instead of the interning
memo, and the acceptance work that depends on it.
