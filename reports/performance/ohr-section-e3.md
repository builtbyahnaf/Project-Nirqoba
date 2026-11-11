# OHR Section E3 — alignment and SIMD, measured

E3 asks whether stronger alignment for array payloads buys vectorized access, and
what over-alignment costs. Both were measured before anything changed.

## What the code does today

- Heap payloads start at `header + 48` in a block carved at `alignof(std::max_align_t)`
  = 8 (`runtime/src/gc.cpp:61`, asserted at `:429-437`), so a payload is 8-aligned and
  `nrq_gc_get_allocation_profile` reports `payload_alignment = 8`.
- A promoted stack array was floored at 16 bytes ("so LLVM can issue aligned 16-byte
  loads", `TirToMlir.cpp:105-118`).
- `memory.owned_array_aligned` can request more, and still can.

So the compiler was not over-aligning heap arrays at all — the heap is *exactly*
natural, 8. The over-alignment was on the stack.

## Measurement

Probe: `benchmarks/bottleneck_discovery/alignment/vector_alignment.cpp`. Same buffer,
same loop, SSE2 16-byte loads; A = aligned base with `_mm_load_si128`, B = aligned base
with `_mm_loadu_si128`, C = base shifted by 8 with unaligned loads. Best of 7 rounds.

| working set | A aligned loads | B unaligned loads | C misaligned +8 | A/B | C/B |
| --- | ---: | ---: | ---: | ---: | ---: |
| 256 KiB, cache resident | 0.157 | 0.157 | 0.158 | 0.999 | 1.008 |
| 16 MiB, bandwidth bound | 0.426 | 0.299 | 0.297 | 1.423 | 0.991 |

Reading it honestly:

- **Cache resident, alignment is worth nothing**: aligned and unaligned loads are
  identical to three decimals, and a true 8-byte misalignment costs 0.8%. On this core
  the unaligned load has no penalty when the data is in cache and actually misaligned
  by less than the access width.
- **The 16 MiB run cannot answer the question** and is reported to show why: at that
  size the loop is bound by memory bandwidth, so both variants sit at the same wall.
  The aligned variant measured 1.42x *slower* there, which no alignment story explains
  and which I could not attribute to anything but codegen; it is stated as an
  unexplained number, not as evidence in either direction.
- Consequently neither "tighten" nor "strengthen" had a measured benefit to claim for
  heap payloads, and the stack floor had none either.

## What was implemented

The 16-byte stack floor was removed (`TirToMlir.cpp`): slots now take the element's
natural alignment, which is all a correct load requires, and the floor's only cost —
`frame_cost = bytes + alignment - 1` on every promoted array — is gone. An explicit
`memory.owned_array_aligned` request still raises the slot, because that is a
programmer's guarantee rather than a compiler guess. `stack_placed_owned_array` now
asserts `alignment = 8`.

Heap payload alignment was left alone: it is already the natural alignment, no object
is misaligned relative to its own elements, and raising it to 16 would mean carving
pages at 16 bytes instead of 8 — the density cost measured in Section C2 for no gain
shown here.

## Explicitly not done

- **No forced 16/32/64-byte object alignment**, as the roadmap requires.
- **No cache-line padding** on payloads or headers.
- **No AVX2/AVX-512 assumptions**: the probe uses the x86-64 baseline width, because a
  target that changes the vector width must first change the target profile.

## Limitation

One machine, one compiler, baseline SSE2 width, two working-set sizes. A target where
misaligned 32-byte loads split a cache line would need its own measurement; this
document claims nothing about it.
