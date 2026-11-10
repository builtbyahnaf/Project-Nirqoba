# OHR Section B — compact per-instance metadata

Scope: the per-instance metadata core, its encoding, and the evidence for the
encoding. Section B is **not finished**; this report records what is decided, what
is measured, and what remains. Production configuration throughout
(`NRQ_RUNTIME_DEBUG_ENABLED=0`).

## 1. What an instance actually needs

After Section A, an object's header is 48 bytes and splits 40 / 8:

| field | bytes | changes | source |
| --- | ---: | --- | --- |
| `next` / `tlab_next` (union) | 8 | allocation, sweep | per instance |
| `mark_next` | 8 | marking | per instance |
| `page` | 8 | never | derivable from the address for ordinary pages |
| `birth_epoch` | 8 | allocation | per instance |
| `element_count` | 4 | never | per instance, arrays only |
| `state` | 4 | marking, sweeping | per instance |
| `layout` | 8 | never | shared descriptor |

The 8 shared bytes are the descriptor pointer. The other 40 are the target of
Section B, whose objective is an 8–12 byte instance core "where the actual runtime
requirements permit it". Nothing in this section has removed a field yet: the
first five steps changed how the mutable state is encoded, not how much of it
there is.

## 2. B1 — the encoding that was chosen

`state` is one `uint32_t` with masks and shifts the runtime defines:

| bits | field | width |
| --- | --- | --- |
| 0 | marked | 1 |
| 1 | young | 1 |
| 2 | remembered | 1 |
| 8–15 | promotion age | 8 |

Decisions and their reasons:

- **No C++ bitfields.** The layout of a bitfield is implementation-defined; the
  masks are not. `static_assert`-able constants, one word, defined encoding.
- **No 4-bit fields.** The roadmap asks for explicit 4-bit packing where it pays.
  Here it does not: the word has 24 spare bits, so narrowing the age to 4 bits
  would save no storage, and it would cap the age at 15 for no benefit. The age
  saturates at 255, which is far past the promotion age.
- **One word, not four bytes.** Measured below; the alternative is not faster.

## 3. B3 — is the packed word actually cheaper?

The roadmap warns that the smallest representation is not automatically the
fastest, and that frequently updated fields may belong in separate storage. That
is a question about this machine, so it was measured rather than argued.

Probe: `benchmarks/bottleneck_discovery/state_encoding/phase_ab.cpp`. One million
cells, two passes that mirror the collector: a mark pass (test the mark, set it)
and a sweep pass (read the state once, clear the mark, bump the age of a young
survivor). Three encodings: four separate bytes, one packed word, and a hybrid
with the mark byte separate from a packed young/remembered/age word.

**First attempt, and why it was thrown away.** With 4-byte cells the packed word
looked 70% slower (0.77 ms against 1.30 ms). That number is an artefact: a
4-byte cell makes the working set 4 MB instead of 48 MB, so the comparison was
measuring cache density, not the encoding. A real object is a 48-byte span, and
every state field sits in the same cache line whichever encoding is used.

**At the real stride** (48-byte cells, 48 MB working set, five runs each):

| encoding | runs (ms) | median |
| --- | --- | --- |
| four separate bytes | 5.45, 5.69, 5.87, 5.93, 5.97 | 5.87 |
| one packed word | 5.78, 5.80, 5.97, 6.12, 6.56 | 5.97 |
| hybrid (mark byte + packed word) | 7.15, 7.16, 7.27, 7.68, 7.79 | 7.27 |

MEASURED: the packed word is within the run-to-run spread of separate bytes
(1.7% at the median, with overlapping ranges), and the hybrid is about 24% slower
because the sweep then performs two stores per object instead of one. So the
packed word is kept: it costs nothing measurable at the real layout and it lets
the flag paths read one word instead of two.

This is a measurement of the access pattern, not of the collector: it does not
include the mark stack, the page walk or the barriers. It is enough to reject the
hybrid and to stop treating "packed" as a cost that needs justifying.

## 4. B2 — the candidate mapping, field by field

| metadata | representation | why |
| --- | --- | --- |
| descriptor identity | direct pointer (8 bytes) | one load, no table lookup on the trace path. A 32-bit index would need a lookup per traced object; that trade is **not measured** and is left to Section C, where the header is recalculated. |
| allocation size class | not stored | the span is `align(header + element_count * payload_size)`, computed from the descriptor and the length. Storing a class would duplicate descriptor data, which B2 forbids. |
| object kind (record/array) | descriptor flag `NRQ_OBJECT_LAYOUT_ARRAY` | per layout, not per instance. An array of one element is still an array. |
| flags | packed bits in `state` | measured above. |
| generation/age | 8-bit field in `state`, saturating | promotion age is small; saturation is explicit. |
| alignment | not stored | every payload is `max_align_t` aligned by construction; the descriptor's `payload_size` is the only size input. |
| ownership / allocation policy | not stored per instance | the runtime has no per-object ownership distinction: every managed object is collector-owned, and borrowed or externally managed memory never enters the heap. Adding a field for a distinction the runtime does not make would be the "blindly compress into a boolean" mistake the roadmap warns about, in reverse. |

## 5. What Section B still needs

The instance core is still 40 bytes. Reaching the roadmap's 8–12 requires
removing fields, and each removal has a precondition:

1. **`mark_next`** — only the mark worklist's overflow uses it. Replacing the
   intrusive spill with owned storage removes the field.
2. **`next`** — the sweep list and the unindexed-object chain. Both would need
   side storage; the current design deliberately never allocates on a GC path, so
   this needs a preallocated structure, not a vector that can throw.
3. **`page`** — derivable by masking the address for ordinary pages; pages larger
   than 64 KiB need the existing chunk map as a fallback.
4. **`birth_epoch`** — derivable from the page if pages carry the epoch they were
   filled in, which they do not today.

Steps 1 and 2 only pay off together: removing one pointer leaves 40 bytes, and 40
is not a multiple of 16, so the payload would stop being `max_align_t` aligned.
The pair takes the header to 32 bytes, which is why they are one unit of work.

## 6. Verification

- 210 unit tests, 263 integration tests, all passing at the time of writing.
- The encoding is asserted field by field by
  `TheStateWordEncodesEachFieldIndependently`, and the instance/shared split by
  `TheHeaderSplitsIntoInstanceAndSharedBytes`.
