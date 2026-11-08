# 04 — Object Header: Exact Layout and Size Trade-offs

**Domain:** Object representation
**Verdict:** The header is **96 bytes** (not 128). For a 24-byte node that is
**80 % metadata** and a **120-byte span**, i.e. 1.875 cache lines per object and
3,841 pages (240 MiB) for 2.1 M nodes. The 96 bytes are not padding: 32 of them
are a `ReferenceLayout` sized for two `size_t` offsets plus an overflow pointer,
and 8 more are an `element_size` that only arrays need. 64 bytes is a low-risk
improvement; **40 bytes gives exactly one 64-byte cache line per object** and is
the strongest candidate if the header is to be redesigned. 32 and 24 are
reachable but exchange memory for per-object CPU work; they must be measured,
not assumed.

---

## 1. Measured layout [MEASURED]

Probe compiled against the real `gc.cpp` (`header_probe/probe.cpp` prints
`sizeof`, `alignof`, `offsetof` and a live-object span from `object_span_bytes`):

```
sizeof(ObjectHeader)  = 96
alignof(ObjectHeader) = 8        (MSVC x64 max_align_t)
offsetof next              =  0
offsetof mark_next         =  8
offsetof page              = 16
offsetof size              = 24
offsetof element_size      = 32
offsetof trace             = 40
offsetof marked            = 48
offsetof young             = 49
offsetof remembered        = 50
offsetof age               = 51
offsetof birth_epoch       = 56
offsetof reference_offsets = 64   (sizeof = 32)
span(payload=24)           = 120  (measured on a real allocation)
```

`static_assert(sizeof(ObjectHeader) == 96)` at `gc.cpp:353` confirms the
intent, and `object_span_bytes()` (`gc.cpp:359-363`) rounds to
`alignof(max_align_t) = 8`, so payload 24 → 120, payload 8 → 104, payload 64 →
160.

### Why 32 bytes for the reference layout [SOURCE]

`ReferenceLayout` (`gc.cpp:262-296`) is:

```cpp
std::array<size_t, 2> inline_offsets_;      // 16 bytes
std::unique_ptr<std::vector<size_t>> overflow_offsets_;  // 8
size_t size_;                                // 8
```

That is **32 bytes to describe at most two `uint32` offsets** in the common
case. It is the single largest removable block of metadata.

### Field-by-field necessity [SOURCE]

| Field | Bytes | Used for | Can it go? |
|---|---:|---|---|
| `next` / `tlab_next` (union) | 8 | TLAB pending batch; `unindexed_objects` list for large/unindexed objects | No, but one slot is genuinely shared |
| `mark_next` | 8 | spills the 256-entry mark worklist into the object | Only by making the worklist a growable vector |
| `page` | 8 | live/young counters, bitmap, free spans, span release | Replaceable by a 4-byte page index (`gc_pages[index]`) |
| `size` | 8 | span, payload bounds, array length | 4 is enough (≤4 GiB single object; use a large-object side record above that) |
| `element_size` | 8 | array bounds checks, array tracing | Needed **only for arrays**; neighbours don't use it |
| `trace` | 8 | indirect trace callback | Replaceable by a small trace-kind tag + switch/side table |
| `marked`,`young`,`remembered` | 3 | collector state | Pack into one word with `age` and an epoch tag |
| `age` | 1 | promotion counter | 4 bits is plenty (`kPromotionAge` is small) |
| `birth_epoch` | 8 | resets `age` when an object survives into a new epoch | Replaceable by an epoch tag: `age` is valid only if `tag == current` |
| `reference_offsets` | 32 | traced reference offsets | 8 bytes for two `uint32` offsets + count in flags; overflow side table for wide records |

The header allocator comment itself flags the risk: *"a regression here silently
raises the cost of every small allocation"* (`gc.cpp:350-352`).

---

## 2. What the 96 bytes cost [MEASURED/PROJECTION]

For benchmark 02 (2,097,151 nodes, 24-byte payload, 64 KiB pages):

| Quantity | Value | Derivation |
|---|---:|---|
| Span per object | 120 B | measured |
| Objects per page | 546 | 65,536 ÷ 120 |
| Pages for the graph | 3,841 | ceil(2,097,151 ÷ 546); matches harness `PAGES 3841` |
| Heap | 240 MiB | 3,841 × 64 KiB |
| Bitmap zero bits per page | 7,646 | 8,192 − 546 |
| Cache lines touched per object | 1.875 average | 120 ÷ 64 |
| Header share of heap | 80 % | 96 ÷ 120 |

Consequences, in order of certainty:

1. **More pages → more sweep bitmap work.** Measured: 28.65 M zero-bit
   iterations per collection, ~62 ms at the current loop. Shrinking to 64 bytes
   cuts pages by 27 %, hence zero-bit iterations by 27 %.
2. **More pages → more first-touch faults.** Measured: 30.7 ns/object cold-vs-warm
   (`01-allocator-ao.md` §3). 27 % fewer bytes ≈ 27 % fewer faults.
3. **More cache lines per object.** A 120-byte object straddles line
   boundaries: 546 objects × 120 B = 65,520 B in 1,024 lines, i.e. every line is
   fully used but ~87 % of objects cross a line boundary. A 64-byte span gives
   **one line per object, no straddling**.
4. **TLB pressure.** 240 MiB at 4 KiB pages = 61,440 pages; a 64-byte span is
   128 MiB = 32,768 pages.

---

## 3. Candidate layouts, compiled and checked [MEASURED]

`header_probe/layout_candidates.cpp` defines each candidate as a real struct and
prints `sizeof`/`alignof`; the numbers below are compiler output, not arithmetic
on paper:

| Layout | sizeof | align | 24 B span | objects/page | pages for 2.1 M | heap | zero-bit iters/col |
|---|---:|---:|---:|---:|---:|---:|---:|
| **H96** current | 96 | 8 | 120 | 546 | 3,841 | 240 MiB | **28.65 M** (measured) |
| **H64** safe squeeze | 64 | 8 | 88 | 744 | 2,819 | 176 MiB | 21.0 M |
| **H48** + page index, compact layout | 48 | 8 | 72 | 910 | 2,305 | 144 MiB | 17.2 M |
| **H40** one cache line per object | 40 | 8 | **64** | **1,024** | 2,048 | 128 MiB | 15.3 M |
| **H32** + trace tag | 32 | 8 | 56 | 1,170 | 1,793 | 112 MiB | 13.4 M |
| **H24** + vector worklist | 24 | 8 | 48 | 1,365 | 1,537 | 96 MiB | 11.5 M |

Zero-bit projections scale with page count from the measured 28.65 M at 3,841
pages; they are **[PROJECTION]** and become irrelevant once the bitmap loop is
fixed (`03-collector-colo.md` §5) — after that fix the header's sweep benefit is
mostly TLB/cache, not bit tests.

### What each step requires

**H96 → H64 (low risk, no semantic change)**

* `size`, `element_size` → `uint32_t` (with a large-object side record or a
  saturation check at allocation; `span` never exceeds 64 KiB for page objects).
* `marked`/`young`/`remembered`/`age`/epoch tag → one `uint32_t`.
* `ReferenceLayout` → `{uint32_t offset[2]; uint8_t count; uint8_t overflow;}` =
  12 → pad to 16? Keep it 8 by using `uint32_t offset[2]` and putting `count`
  in the flags word, with `overflow` implied by `count == 0xF`. Overflow uses a
  side table keyed by object address (rare).
* `page` stays a pointer.
* Net: 8+8+8+4+4+8+4+8 = 52 → 56. To reach 64 comfortably: keep `page` and
  `size`/`element_size` at 8? Then 8+8+8+8+8+8+4+8 = 60 → 64. Either way, 64 is
  reachable without changing the interface, and leaves 4 bytes of headroom.

**H64 → H48**

* `page` pointer → `uint32_t page_index` into `gc_pages` (65,536 pages = 4 GiB
  heap; larger heaps use the existing `object_index` fallback).
* `size`/`element_size` → `uint32_t` each; flags in 4 bytes; layout 8.
* 8+8+4+4+4+8+4+8 = 48 exactly.

**H48 → H40 (one cache line per object)**

Two independent routes, both 40 bytes:

* *Route A — drop `element_size` from the header*: store it in a 4-byte prefix
  for array objects only, using an `is_array` flag. 8+8+4+4+8+4+4 = 40.
* *Route B — drop `mark_next`*: replace the inline worklist spill with a
  `std::vector<ObjectHeader*>` reserved at cycle start. 8+4+4+4+8+4+8 = 40.

Route A costs nothing per object in the common case but adds a branch in array
bounds checks. Route B costs vector growth and worse worklist locality but
removes 8 bytes from *every* object. Both need measurement before choosing.

**H40 → H32 / H24**

* `trace` pointer → 4-bit kind + runtime switch (and a side table for custom
  trace functions). This also removes an indirect call, which may *help* mark.
* `mark_next` → vector (H24).
* 24 also requires `page_index` to stay, so it is the practical floor without
  a per-object page lookup.

---

## 4. Which size is fastest in total? (the question the report must answer)

There is no size-free answer, because the header trades memory footprint against
per-object CPU work. The evidence supports a clear ordering:

| Target | Risk | Build | Collect | Traversal | Recommendation |
|---|---|---|---|---|---|
| 96 (today) | — | baseline | baseline | baseline | — |
| 64 | Low | −? ; fewer faults | −27 % bitmap + better TLB | better locality | **Do this first** |
| 48 | Moderate | ~same | −40 % bitmap | better | Next step |
| **40** | Moderate | +1 branch (array route) | −47 % bitmap; 1 line/object | best locality | **Best total design** if measured win confirms |
| 32 | High | trace switch may help | marginal over 40 (13.4 vs 15.3 M bits) | 2 objects/line possible | Only with measurements |
| 24 | High | page lookup per object | marginal | — | Not justified by evidence |

**Answer:** 64 is the best value per unit of risk; **40 is the fastest total
architecture** among the candidates, because it makes every object exactly one
cache line and every page hold exactly 1024 objects, which is the only candidate
that removes line straddling entirely. 32 and 24 buy little additional footprint
(16–32 MiB) while adding per-object CPU work, and the sweep bitmap advantage
they offer disappears once the bitmap loop is fixed.

**Important honesty:** only 96 has been measured end-to-end. The rest are
derived from span arithmetic and the measured cold/warm allocation split.
Anyone implementing them must re-measure, because the header's effect on build is
partly a *write* cost (11 stores over 2 cache lines) that can get **worse** if a
compressed header is packed into a single line but requires read-modify-write
bitfield updates.

---

## 5. Implementation constraints (so this does not become a rewrite trap)

1. `object_span_bytes()` is used by sweep, allocation and the harness; all three
   must change together, and `static_assert` must encode the new invariant.
2. `header->page == page` in `find_object_locked` (`gc.cpp:665`) is a
   **validation** check; replacing the pointer with an index changes it to
   `gc_pages[header->page_index].get() == page`, which is an extra dependent load
   in a hot lookup. Measure before adopting.
3. `birth_epoch` → tag: the sweep resets `age` when an object survives into a
   new epoch (`gc.cpp:1644-1666`). A 4-bit epoch tag must be compared before
   using `age`; this is a correctness-sensitive change and needs the generational
   tests to actually run (they currently do not — see `07-unfinished-work-audit.md`).
4. `ReferenceLayout` overflow: relocated and wide records currently allocate a
   `std::vector` per object. A side table must not introduce a hash lookup on
   every trace; keep the inline two-offset fast path branch-free.
5. ABI: the header is not exposed in `gc.h`, but `ObjectHeader` is used by
   `gc_debug_internal.h` and the unit tests; keep one definition and update the
   static assertions in the same commit.
