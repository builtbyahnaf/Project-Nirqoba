# Object Header Compaction & Adaptive Object Representation (AOR)
**Priority:** Critical — Major Runtime Architecture Upgrade  
**Architecture:** Adaptive Object Representation (AOR)  
**Current baseline:** H48 (48-byte ObjectHeader)  
**Target:** 0–16 bytes per object  
**Primary objective:** Maximum execution speed and minimum memory overhead without sacrificing runtime correctness, stability, or quality.

---

## 1. Core Objective

Redesign Nirqoba's GC object representation system to eliminate unnecessary per-object metadata and introduce three adaptive object representations.

The compiler, allocator, and garbage collector must cooperate to select the most efficient representation for each allocation.

### Target representations

| Representation | Header size | Intended usage |
|---|---:|---|
| Headerless | 0 bytes | Fixed-size, uniform-layout objects |
| Compact | 8 bytes | Objects requiring limited individual metadata |
| Extended | 16 bytes | Variable-sized objects, arrays, and special cases |

**16 bytes is the absolute maximum for standard GC-managed object headers.**

The objective is not simply to reduce ObjectHeader size. Measure and minimize total metadata overhead, including page descriptors, bitmaps, side tables, alignment waste, and auxiliary structures.

Native-width object references must remain unchanged unless a separately measured optimization proves otherwise.

## 2. Non-Negotiable Engineering Principles

1. Extreme execution performance is the highest optimization priority.
2. Memory consumption must be minimized without introducing expensive metadata lookups or unnecessary indirection.
3. Do not sacrifice allocation throughput, traversal speed, marking speed, sweeping speed, or GC pause predictability merely to achieve a smaller header.
4. Preserve precise garbage collection. Conservative pointer scanning is prohibited.
5. Preserve SATB marking and generational remembered-set correctness.
6. Preserve deterministic destruction and ownership semantics.
7. Preserve TLAB allocation and efficient batch publication.
8. Never introduce allocation into hot marking or barrier paths unnecessarily.
9. Avoid global locks, unnecessary atomic operations, and cache-unfriendly metadata structures.
10. Production and debug runtimes must maintain equivalent collector semantics and correctness.
11. Preserve existing language behavior, ABI requirements where applicable, and native interoperability.
12. Do not remove existing functionality merely to simplify the redesign.

**Bug-fixing requirement:** The coding agent MUST fix every bug discovered or introduced during implementation. This includes existing bugs exposed by the migration, memory corruption, incorrect reachability, deadlocks, race conditions, invalid destruction, leaks, crashes, regressions, and compiler/runtime inconsistencies. Do not leave known bugs documented as future work. Fix them, add regression tests, and rerun the relevant test suites before proceeding.

## 3. Architecture

### A. Headerless Representation — 0 bytes

Target: Fixed-size objects with uniform layouts.

- Store object-start information in page-local metadata.
- Derive allocation size and layout from page/slot information.
- Store marking, generation, and other mutable GC state in page-local bitmaps or equivalent structures.
- Avoid per-object side allocations.
- Ensure precise object identification and reference tracing.
- Support page recycling and TLAB batch publication.

This should be the preferred representation whenever it is demonstrably faster and cheaper.

### B. Compact Representation — 8 bytes

Target: Objects requiring limited object-specific metadata.

- Use compact layout identifiers where necessary.
- Pack suitable immutable metadata into compact fields.
- Derive page ownership and other recoverable information.
- Avoid pointer compression unless independently justified by benchmarks.
- Ensure atomicity requirements are handled correctly.
- Minimize decoding and indirection overhead.

### C. Extended Representation — 16 bytes

Target: Objects that genuinely require additional individual metadata.

Potential applications:
- Variable-sized objects.
- Arrays and complex array representations.
- Specialized GC-managed objects.
- Objects whose metadata cannot be derived efficiently from their page.

Do not automatically assign extended headers to all arrays. Investigate page-local and side-metadata alternatives first.

## 4. Compiler-Assisted Representation Selection

Implement a compiler/runtime cooperation model.

**Compiler responsibilities:**
- Analyze object layout and field types.
- Determine whether an object has a fixed or variable size.
- Generate compact layout descriptors.
- Identify headerless eligibility.
- Emit representation hints to the allocator.
- Preserve correct layout and destruction information.

**Allocator responsibilities:**
- Select an appropriate page class.
- Validate compiler-provided representation hints.
- Allocate objects using the appropriate representation.
- Maintain page-local metadata.
- Support large-object allocation separately.

**Garbage collector responsibilities:**
- Retrieve metadata through the cheapest valid path.
- Maintain precise tracing.
- Maintain marking and generational state.
- Handle all three representations through a unified collection protocol.

The compiler must never make an unsafe representation decision based solely on static assumptions when runtime allocation properties are required.

## 5. Implementation Phases

### Phase 1 — Complete Metadata Dependency Audit
**Priority: Critical**

Audit `runtime/src/gc.cpp`, allocator implementations, compiler code generation, object-layout descriptors, barriers, TLABs, and all `ObjectHeader` consumers.

Identify every dependency on:
- `next` / `tlab_next`
- `mark_next`
- `page`
- `birth_epoch`
- `element_count`
- `state`
- `layout`

Produce a dependency map showing which information can be derived, moved, packed, or must remain object-specific.

Also inspect large-page handling, array layouts, finalization, page recycling, and debug instrumentation.

Do not begin destructive header changes before completing this audit.

### Phase 2 — Page-Local Metadata Architecture
**Priority: Critical**

Implement the infrastructure for metadata extraction.

- Page descriptors and efficient page lookup.
- Object-start and allocation metadata.
- Page-local marking bitmaps.
- Generation and remembered-set metadata.
- Layout and slot-size information.
- Large-object directory and fallback handling.
- Safe metadata initialization and page recycling.

Optimize locality and avoid unnecessary indirection.

### Phase 3 — Remove Intrusive Object Dependencies
**Priority: Critical**

Eliminate the need for the existing per-object links.

Replace:
- Intrusive marking worklist with efficient preallocated or segmented external worklists.
- Per-object TLAB publication links with page/span publication ranges where feasible.
- Per-object page pointers with efficient page derivation or directory lookup.

Preserve efficient batch publication, marking throughput, sweep accounting, and thread synchronization.

Do not introduce heap allocation or global locking into hot collector paths.

### Phase 4 — Implement Adaptive Object Representations
**Priority: Critical**

Implement all three representations:

1. Headerless.
2. Compact 8-byte.
3. Extended 16-byte.

Introduce clear representation metadata and validation.

Ensure:
- No object exceeds the 16-byte header limit.
- Headerless objects remain precisely identifiable.
- Arrays and large objects are correctly supported.
- All representations support the required lifecycle operations.
- No representation creates hidden per-object allocation overhead.

Keep H48 available temporarily as a migration reference and fallback until the new architecture passes validation.

### Phase 5 — Compiler and Allocator Integration
**Priority: Critical**

Integrate compiler-generated layout descriptors and representation hints.

Implement allocator selection using:
- Object size.
- Layout uniformity.
- Reference structure.
- Array characteristics.
- Required lifecycle metadata.
- Page-class availability.

Prioritize fast allocation and predictable page placement.

Avoid expensive compiler-generated runtime checks when eligibility can be established statically.

### Phase 6 — Unified GC Integration
**Priority: Critical**

Integrate all representations with:

- Minor and major collection.
- SATB marking.
- Generational barriers.
- Remembered sets.
- TLAB allocation and publication.
- Sweeping and reclamation.
- Deterministic destruction.
- Finalization.
- Pinned objects.
- Large objects and arrays.
- Page recycling.
- Multithreaded collection.

Production and debug runtimes must maintain equivalent collector behavior and safety guarantees.

### Phase 7 — Extreme Performance Optimization
**Priority: Critical**

Optimize the complete architecture, not just header access.

Investigate:
- Cache locality and cache misses.
- Bitmap access and iteration.
- Page lookup latency.
- Mark-worklist throughput.
- Allocation and publication throughput.
- Branch prediction.
- Atomic operation overhead.
- Memory bandwidth.
- TLB pressure.
- Metadata prefetching.
- Sweep and reclamation efficiency.

Remove redundant operations and unnecessary memory accesses.

Do not accept an optimization merely because it reduces instruction count or header size. Measure actual execution performance.

### Phase 8 — Correctness, Stress Testing & Benchmark Validation
**Priority: Critical**

Run comprehensive testing against the existing H48 baseline.

Required correctness tests:
- Old-to-young reachability.
- SATB correctness.
- Root registration and removal.
- Cyclic references.
- Deep object graphs.
- Arrays and nested arrays.
- Objects spanning page boundaries.
- Large-object allocation and reclamation.
- TLAB publication.
- Page recycling.
- Pinned objects.
- Finalization and deterministic destruction.
- Multithreaded allocation and collection.
- Debug/production equivalence.
- Memory leak and corruption detection.

Required performance measurements:
- Header and metadata bytes per live object.
- Allocation throughput.
- Object traversal throughput.
- Marking throughput.
- Sweeping throughput.
- Minor and major GC pause times.
- Store-barrier latency.
- Peak RSS.
- Total committed heap memory.
- Cache misses and TLB behavior where available.

Test headerless, compact, and extended representations independently and together.

Compare against H48 using identical workloads, compiler settings, and machine conditions.

### Phase 9 — Production Stabilization
**Priority: Critical**

- Remove temporary migration code and obsolete structures.
- Resolve every outstanding migration-related bug.
- Verify all supported object representations.
- Run the complete existing regression suite.
- Run stress tests repeatedly.
- Verify production and debug runtime builds.
- Update architecture documentation and performance reports.
- Document measured improvements and any remaining limitations honestly.

Only remove H48 fallback support once the new implementation is demonstrably stable.

---

## 6. Performance Acceptance Criteria

The migration is not successful merely because the header is smaller.

| Metric | Requirement |
|---|---|
| Standard object header | Maximum 16 bytes |
| Headerless representation | Zero per-object header |
| Allocation throughput | No material regression |
| Traversal throughput | No material regression |
| Marking throughput | No material regression |
| Sweeping throughput | No material regression |
| GC pauses | No material regression |
| Total memory overhead | Measurable reduction |
| Small-object workloads | Significant memory improvement |
| Large-object workloads | No pathological regression |
| Production/debug | Equivalent semantics |
| Correctness | Full regression suite passes |

Prefer measurable performance improvements over merely matching H48.

If a representation is consistently slower than H48 for its intended workload, redesign it rather than forcing its adoption.

## 7. Execution Rules

1. Inspect the actual repository and current implementation before modifying anything.
2. Follow the existing architecture and naming conventions.
3. Do not invent APIs, structures, or fields without checking their actual dependencies.
4. Implement real code changes, not just architectural documentation.
5. Work phase by phase, with measurable deliverables.
6. Fix every bug encountered or introduced. Add regression tests for every resolved issue.
7. Never disable tests, weaken assertions, suppress sanitizer findings, or bypass correctness checks to obtain better benchmark numbers.
8. Never trade memory safety or runtime correctness for raw speed.
9. Benchmark before and after meaningful optimizations.
10. Record regressions honestly and resolve them before continuing.
11. Keep production and debug runtime implementations synchronized.
12. Maintain clear progress reports, benchmark results, and architectural decision records.

If an architectural decision proves inefficient, revise it based on measured evidence. Do not blindly follow the proposed design when repository evidence demonstrates a better solution, but document significant deviations and their justification.

## 8. Definition of Completion

AOR is complete only when:

- All three representations work correctly.
- The compiler and allocator cooperate in representation selection.
- All collector operations support every representation.
- Standard object headers never exceed 16 bytes.
- Headerless allocation is functional and precisely traceable.
- Total memory overhead is demonstrably reduced.
- Performance is competitive with or better than H48 across the intended workloads.
- No known migration-related bugs remain.
- The full test suite passes.
- Production and debug runtimes are validated.
- Architecture and benchmark documentation are updated.

---

## Final Directive

**Extreme performance is the highest engineering priority. Runtime correctness, memory safety, stability, and maintainability are non-negotiable constraints.**

The goal is not to create the smallest possible object header at any cost.

The goal is to build a substantially faster and more memory-efficient Nirqoba runtime by eliminating redundant object metadata, improving metadata locality, and making the compiler intelligent enough to select the right object representation.

Do not settle for superficial compression. Redesign the architecture where necessary, optimize every hot path, fix every discovered bug, and validate every meaningful improvement with reproducible measurements.

**Build for maximum performance. Preserve everything that already works. Improve everything that can be improved.**