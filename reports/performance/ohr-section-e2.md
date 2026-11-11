# OHR Section E2 — hot and cold data, investigated

This section was allowed to change something only if a representative workload showed
a benefit. This document reports what was measured, including the parts that say no.

## Two different questions

E2 is usually written down as one question but it is two, and they have different
levers:

- **Hot/cold fields** — within one object, is the frequently read field faster when it
  sits in the first cache line, close to its neighbours? The lever is field order.
- **Hot/cold objects** — across a collection, does the traversal order or object size
  decide how many lines the scan touches? The lever is object size and access pattern,
  not field order.

Conflating them is how "reorder fields for locality" becomes a change that helps
nothing while still costing a layout rule.

## Array stride: no extra alignment is paid

Before the field question, the nesting question: `type_size` for an array is
`length * type_size(element)` (`compiler/lib/Lowering/DataLayout.cpp:117`), and an
aggregate's size is rounded up to its own alignment (`:118-130`). So the element size
is already a multiple of the element alignment, which is exactly the condition for a
tight stride — there is no gap between elements to remove, and nothing to tighten.

Measured on the two layout tests: `Wide` is 24 bytes with align 8, `Mix` is 16 with
align 8; both sizes are multiples of their alignments, so `T[4]` is `4 * size` with no
inter-element padding. The property is structural, not lucky: it follows from the
round-up in `type_size`, which `describe_aggregate` mirrors.

## Field position: measured, no benefit

Probe: `benchmarks/bottleneck_discovery/hot_cold/field_position.cpp`. 1,048,576 records,
eight passes per round, best of five rounds, `cl /O2`. It is a hand-written C++
replica of the layouts the ordering rule produces, so it measures the hardware rather
than a proposed transformation, and nothing in the compiler was changed to run it.

| variant | ns/object | lines/object |
| --- | ---: | ---: |
| A — 64 B record, hot field first | 3.214 | 1 |
| B — 128 B record, hot field first | 5.887 | 2 |
| C — 128 B record, hot field on the second line | 5.445 | 2 |
| D — hot field alone in its own array (SoA ceiling) | 0.245 | 0.125 |
| E — 128 B, hot first, random object order | 15.595 | 2 |
| F — 128 B, hot second, random order | 15.730 | 2 |

What the numbers say (MEASURED, one machine, MSVC x64):

- **Field position inside a record is irrelevant here.** Moving the hot field from the
  first line to the second changed the sequential scan by −7.5% (C/B = 0.925) and the
  random scan by −0.9% (E/F = 0.991). The sequential difference is in the direction
  nobody predicts and is not reproduced by the random case, so it is variance, not an
  effect. A reordering pass that moved the hot field would have no measured basis.
- **Record size is the real lever.** A record crossing two lines costs 1.83× a record
  in one line (B/A), because the scan touches twice as many lines for the same number
  of objects. That is a size question, and size is what E1's ordering already
  minimizes.
- **Object order dwarfs field order.** Random access costs 2.6× sequential (E/B) on a
  128 MB working set. Neither number moves when a field changes line.
- **The ceiling is separation, not arrangement.** SoA is 24× faster than the two-line
  record (D/B) because it fetches one eighth of a line per object. That gap is what
  E4 would have to reach, and it is not reachable by permuting fields inside a record.

## Decision

**No hot/cold field reordering, no indirection, no header change, no change to
reference tracing.** There is no measurement in this baseline that a field-order
transform could improve: the quantity it would move (which line a field lands on) was
worthless in both sequential and random traversal, while the quantities that did
matter (record size, object order, record/field separation) are outside field order.

E2 is closed as investigated with a negative result. It reopens only with a workload
that shows a field-order effect — for example a real program with skewed per-field
access where the hot field is repeatedly read from a record whose other lines are cold
in cache, not just present in memory. The committed probe is the harness that would
have to show it.

## Limitation

The probe is synthetic and does not run through the Nirqoba compiler or its collector;
it isolates the layout question, not the whole program. One machine, one compiler, one
working-set size (128 MB of records). Values are best-of-five per variant, so
comparisons are against warm caches and prefetchers doing their job, which is the
favourable case for the reordering hypothesis — and it still lost.
