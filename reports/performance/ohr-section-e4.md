# OHR Section E4 — data-oriented layouts, evaluated and scoped

E4 is explicitly optional in the roadmap and must not transform ordinary collections.
This records what was measured, what an explicit opt-in representation looks like in
Nirqoba today, and what it would take to make it a first-class container.

## Measured: the three layouts, same compiler

Programs: `benchmarks/bottleneck_discovery/soa/{aos,soa,aosoa}.nrqba`. Two million
records, three fields, two hundred scans of the hot column, executable timed from
outside (best of five, milliseconds).

| layout | best | median | checksum |
| --- | ---: | ---: | ---: |
| AoS — `Particle{int x; int y; int mass;}` array | 642 | 652 | 399999800000000 |
| SoA — three owned `int` columns | 260 | 277 | 399999800000000 |
| AoSoA — 8-record blocks, one column each | 308 | 321 | 49999800000000 |

- **SoA is 2.47× the AoS time for this scan** (642 → 260 ms). The scan touches 16 MB
  of dense column instead of 48 MB of 24-byte records, which is the whole effect: it
  is bandwidth, and it scales with how many fields the record has.
- **AoSoA sits between** (2.08×), because a block of eight still fetches the other
  fields' bytes when the block straddles a line, but far less than one record at a time.
- The AoSoA checksum differs by construction — it sums `i` per block, not per record —
  so it is not evidence of different work, only of different data. Worth stating
  plainly rather than leaving as an unexplained difference.

Precision: these are whole-executable times including startup and allocation, taken
with `date +%s%N` around five runs. The gap is far larger than the noise, but the
absolute numbers are not clean cycle counts.

## The explicit representation, and its semantics

Today an opt-in SoA in Nirqoba is a *scope that owns one array per column*. That is
not a hand-wave: it is the form the language can express, and its semantics are the
ordinary semantics of owned arrays, which is why they hold without new rules.

| concern | how it works |
| --- | --- |
| **identity** | The container is the set of columns in their owning scope; there is no element identity to lose, because AoS element identity in Nirqoba is already just an index into an owned array. |
| **references** | A column is an owned array of scalars, or of managed references for a reference field. Reference columns are traced by the existing array reference-offset walk (`collect_managed_reference_offsets` already strides arrays), so GC sees them with no new machinery. |
| **aliasing** | Two columns cannot alias: they are distinct allocations. The aliasing rule that matters — a borrow may not outlive its owner — is the existing borrowed-view rule, unchanged. |
| **mutation** | `x[i] = v` and `c.set_x(i, v)` are the same store to a distinct object; no field of an element is shared with another element. |
| **iteration** | Column-major by construction: a loop over `x[j]` is the scan the benchmark measures. Element-major iteration is `j` over all columns, which the compiler already handles. |
| **views** | A borrowed view of a column is an ordinary borrowed array view, so subview and return rules apply unchanged. A view of an *element* is not expressible, and this is a real limitation: SoA gives up element-address identity. |
| **ownership / lifetime** | The columns are owned arrays in the scope that declares them; destruction is deterministic at scope exit, in declaration order. Nothing is shared, so nothing needs a refcount. |
| **ordinary structs** | Untouched. `Particle` still lays out as it did; SoA is something a program chooses per collection, never a transformation applied to a struct. |

## What is not implemented, and why

A **reusable generic container** (`soa<T>`) needs a type parameter over a record's
field set. The language has no type parameters, so the only honest options are a
concrete container per record (duplication) or a compiler feature (parser, type
system, lowering, GC offsets for a new aggregate kind). Neither is a small change, and
building the compiler feature is what E4 would become if a real workload asks for it.

So E4 lands as: measured evaluation, a defined explicit representation, and a
documented boundary. The measured result — 2.47× for a column scan — is the argument
for the feature; the absence of type parameters is the reason it is not a container
type yet.

## Not done, deliberately

- No automatic AoS→SoA transformation of any struct or collection.
- No change to struct layout, identity, or the deterministic ownership model.
- No new allocator or GC path; the representation is made of owned arrays.
