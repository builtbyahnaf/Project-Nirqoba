# Project Nirqoba — Garbage Collector Runtime Optimization
## Implementation Directive: GC 1.1–GC 1.5

**Project:** Nirqoba  
**Repository:** https://github.com/builtbyahnaf/Project-Nirqoba  
**Author / Project Owner:** Muhammad Tahmid Ahnaf (@builtbyahnaf)  
**Scope:** Runtime GC performance optimization  
**Priority:** Critical → High  
**Implementation Mode:** Direct implementation  
**Benchmarking:** Explicitly prohibited until authorized by the project owner

---

# 1. Mission

Implement the following five approved garbage collector optimizations in the existing Nirqoba runtime:

| Upgrade | Work | Priority |
|---|---|---|
| GC 1.1 | Atomic safepoint fast path | Critical |
| GC 1.2 | Thread-local mutator lifecycle | Critical |
| GC 1.3 | Thread-local shadow-stack roots | Critical |
| GC 1.4 | Optimized write barriers and remembered buffers | High |
| GC 1.5 | Rooted allocation and TLAB improvements | High |

The architectural investigation and design evaluation for these upgrades have already been completed.

**Do not restart the investigation. Do not propose a new GC architecture. Do not spend this task producing alternative designs. Implement the approved changes in the existing architecture.**

The objective is to eliminate unnecessary synchronization from ordinary managed execution while preserving the existing collector's correctness and capabilities.

This is an implementation task, not a research task.

---

# 2. Mandatory Architectural Constraints

The existing garbage collector must remain the foundation.

Preserve:

- Precise tracing and root tracking.
- Non-moving object semantics.
- Generational collection.
- Incremental and concurrent marking/sweeping capabilities.
- Existing TLAB allocation architecture.
- Write-barrier correctness.
- Remembered-set correctness.
- Compiler-generated root-frame instrumentation.
- Existing object lifetime and ownership semantics.
- Existing runtime and compiler ABI compatibility unless a necessary, coordinated internal change is required.

Do not replace the collector with a different GC algorithm.

Do not introduce a new ownership model, memory model, allocator family, or language-level GC API.

Do not remove functionality to make benchmarks or individual code paths faster.

Do not weaken correctness guarantees to achieve performance improvements.

## Core optimization principle

Ordinary managed execution must not repeatedly pay for global synchronization when no collector coordination is required.

Specifically:

- A normal loop iteration must not acquire a global mutex merely to check for a GC request.
- Nested mutator entry/exit operations must not repeatedly perform global registration.
- Thread-local root-frame changes should remain local during normal execution.
- Ordinary reference stores should avoid unnecessary global locking.
- Common-path allocations should use thread-local allocation resources wherever possible.

Shared collector state must remain synchronized wherever correctness requires it.

**Do not pursue lock-free programming for its own sake.** A properly synchronized slow path is preferable to an unsafe lock-free implementation.

---

# 3. GC 1.1 — Atomic Safepoint Fast Path

<priority>CRITICAL</priority>

## Objective

Remove unnecessary mutex acquisition from ordinary safepoint polling.

The normal execution path should perform a cheap atomic check and return immediately when no collection request is pending.

## Required implementation

1. Convert the collection-request state into an appropriate atomic representation if it is not already atomic.
2. Implement a fast path that checks whether collection has been requested before acquiring `mutator_mutex`.
3. Acquire the existing synchronization primitives only when the thread actually needs to participate in the safepoint/parking protocol.
4. Preserve the existing collector-thread exclusion behavior.
5. Preserve correct mutator parking, notification, wake-up, and resumption behavior.
6. Ensure that collection-request publication and clearing use a consistent memory-ordering protocol.
7. Ensure that a request cannot be lost because a mutator observes stale state or races with collector state transitions.

## Expected behavior

Conceptually, the common path should resemble:

```cpp
void nrq_gc_safepoint() {
    if (!collection_requested.load(std::memory_order_acquire))
        return;

    // Existing synchronized safepoint protocol.
    // Only threads that need to park enter this path.
}
```

This is illustrative, not a drop-in implementation. Adapt it to the actual runtime state machine.

## Correctness requirements

- No missed collection requests.
- No deadlocks during simultaneous safepoint requests.
- No premature mutator resumption.
- No collector-thread self-parking.
- No invalid collection completion while mutators are still executing in an unsafe state.
- Correct handling of threads entering or leaving managed execution during a collection request.

---

# 4. GC 1.2 — Thread-Local Mutator Lifecycle

<priority>CRITICAL</priority>

## Objective

Eliminate repeated global synchronization when a thread enters or exits nested managed execution.

Mutator nesting depth is thread-local and should be managed locally.

## Required implementation

1. Keep the mutator nesting depth in thread-local state.
2. Make nested `nrq_gc_mutator_enter()` operations cheap.
3. Perform global thread registration only when entering managed execution for the first time.
4. Perform global unregistration or lifecycle cleanup only when leaving managed execution for the final time.
5. Preserve TLAB publication and any required pending-allocation handling during lifecycle transitions.
6. Preserve collector visibility of active mutators.
7. Ensure that thread registration and unregistration remain safe during collection requests.

## Expected behavior

Conceptually:

```cpp
void nrq_gc_mutator_enter() {
    if (thread_mutator.depth++ > 0)
        return;

    // Register this thread with the collector.
    // Handle races with active collection requests.
}
```

And:

```cpp
void nrq_gc_mutator_leave() {
    if (--thread_mutator.depth > 0)
        return;

    // Publish required thread-local state.
    // Perform final lifecycle cleanup.
}
```

These examples describe the intended fast-path behavior. They are not permission to ignore registration failures, collection races, or lifecycle invariants.

## Correctness requirements

- Nesting depth must never become negative.
- Registration must happen exactly once per outermost entry.
- Final cleanup must happen exactly once per outermost exit.
- A thread must never become invisible to the collector while it can still access managed objects.
- Collection requests racing with thread entry or exit must be handled safely.
- Thread termination must not leave stale registered-mutator state.
- TLAB state must remain valid across entry, exit, and collection transitions.

---

# 5. GC 1.3 — Thread-Local Shadow-Stack Roots

<priority>CRITICAL</priority>

## Objective

Reduce global synchronization associated with root-frame creation, modification, and destruction.

Move ordinary root-frame bookkeeping to thread-local storage.

## Required implementation

1. Introduce or improve a per-thread shadow-stack representation for managed roots.
2. Keep each thread's root-frame stack local during ordinary execution.
3. Record a stack-depth checkpoint when a root frame begins.
4. Register roots in the owning thread's local shadow stack.
5. End a root frame by restoring the corresponding checkpoint and removing its local roots.
6. Avoid acquiring `gc_mutex` for ordinary root-frame stack modifications.
7. Ensure the collector can safely enumerate registered threads' roots during collection.
8. Implement the required coordination mechanism so a collector never reads a shadow stack while its owning thread is modifying it.
9. Preserve support for global roots and any existing non-stack root registrations.
10. Preserve compiler-generated root-frame behavior and root-slot lifetime rules.

## Important implementation constraint

Thread-local storage does not automatically make collector access safe.

The collector must obtain a consistent view of each thread's roots through a valid synchronization mechanism, such as the existing safepoint protocol or an equivalent coordinated snapshot.

Do not simply remove the mutex protecting root structures.

## Correctness requirements

- Every live managed reference remains visible to the collector.
- No root is removed while still live.
- No expired stack slot is retained as a valid root.
- Nested root frames restore the correct checkpoint.
- Recursive functions and nested scopes work correctly.
- Exceptions and early returns cannot leave root frames unbalanced.
- Thread exit cannot leave dangling root-stack references in collector metadata.
- Global roots continue to work.
- Concurrent collection cannot observe partially modified root-frame state.

---

# 6. GC 1.4 — Optimized Write Barriers and Remembered Buffers

<priority>HIGH</priority>

## Objective

Reduce the cost of managed-reference stores without weakening concurrent marking or generational collection guarantees.

The existing write barrier must be optimized, not bypassed.

## Required implementation

1. Inspect and optimize `nrq_gc_root_store()` and all relevant managed-reference store paths.
2. Introduce a cheap atomic read of the relevant collector phase/state where appropriate.
3. Avoid acquiring `gc_mutex` for stores that can be proven not to require synchronized barrier work.
4. Preserve the collector's concurrent-marking invariants.
5. Preserve the required incremental-update or snapshot-at-the-beginning behavior used by the existing collector.
6. Preserve old-to-young reference tracking.
7. Introduce or optimize thread-local remembered buffers where appropriate.
8. Batch remembered-set updates rather than synchronizing globally for every individual reference store.
9. Ensure buffers are safely published and drained before the collector concludes that remembered references have been processed.
10. Handle buffer overflow, thread exit, collection transitions, and allocation failure safely.

## Fast-path expectations

The implementation should distinguish between:

- Collector idle, no relevant generational barrier work.
- Active marking requiring a marking barrier.
- Old-to-young reference stores requiring remembered-set tracking.
- Collector phase transitions requiring synchronized handling.

Use the cheapest correct path for each case.

## Critical restrictions

Do not replace managed-reference stores with unconditional plain pointer stores.

Do not remove marking barriers merely because the collector is usually idle.

Do not use a stale collector-phase value to bypass a barrier.

Do not allow a collection to finish before pending thread-local remembered buffers have been published or processed.

Do not change the semantics of managed references.

## Correctness requirements

- No lost references during concurrent marking.
- No missed old-to-young references.
- No premature reclamation.
- Correct handling of null assignments.
- Correct handling of reference replacement.
- Correct behavior when collection starts or finishes during a store.
- Correct remembered-buffer publication and draining.
- Correct handling of threads that terminate with pending buffered entries.

---

# 7. GC 1.5 — Rooted Allocation and TLAB Improvements

<priority>HIGH</priority>

## Objective

Optimize rooted allocation and common-path TLAB usage while preserving root visibility and collection safety.

## Required implementation

1. Inspect the rooted allocation path, especially `nrq_gc_allocate_array_typed_rooted()` and related functions.
2. Remove redundant validation or synchronization from repeated allocations when the root slot's validity can be safely established once for the root-frame lifetime.
3. Reuse validated root handles or equivalent internal representations where appropriate.
4. Keep common-path allocations within thread-local allocation buffers.
5. Restrict global synchronization to operations that actually require shared allocator or collector metadata.
6. Improve TLAB refill and exhaustion handling where the current implementation allows it.
7. Ensure allocations that trigger collection remain safe.
8. Ensure a newly allocated object is visible to the collector before any operation can cause it to be reclaimed.
9. Preserve object-index publication and metadata consistency.
10. Preserve correct behavior when a thread exits, a TLAB is retired, or a collection occurs.

## Correctness requirements

- No object may become temporarily invisible to the collector.
- No invalid or stale root slot may be accepted.
- No object may be reclaimed before its first valid root publication.
- TLAB retirement must not lose allocated objects or metadata.
- Allocation failure must be handled correctly.
- Collection triggered during allocation must not corrupt the current root frame.
- Shared allocator metadata must remain synchronized.

---

# 8. Cross-Cutting Requirements

All five upgrades must work together.

Do not implement them as isolated patches that accidentally invalidate one another.

Pay particular attention to these interactions:

| Interaction | Required guarantee |
|---|---|
| Safepoints + mutator lifecycle | A thread entering or exiting managed execution cannot escape collection coordination. |
| Safepoints + shadow stacks | The collector sees a consistent root snapshot. |
| Shadow stacks + rooted allocation | Newly allocated objects become visible through valid roots. |
| Write barriers + remembered buffers | Buffered references are processed before collection completion. |
| Write barriers + concurrent marking | No reference is lost during a collector phase transition. |
| TLABs + mutator lifecycle | Thread-local allocations are safely published when required. |
| TLABs + collection | Allocated objects and allocator metadata remain consistent. |

## Synchronization policy

Retain synchronization around genuinely shared mutable collector state.

Optimize synchronization placement and frequency rather than blindly eliminating locks.

Use atomics with deliberate memory ordering. Document non-obvious memory-ordering decisions.

Where a lock is retained, explain briefly what shared invariant it protects.

Where a lock is removed from a hot path, explain how the relevant invariant is now maintained.

---

# 9. Implementation Rules

## A. Work directly in the existing codebase

- Read the relevant source files and headers.
- Follow the existing code style and naming conventions.
- Preserve existing APIs unless an internal change is necessary.
- Update all affected call sites consistently.
- Keep compiler-generated GC instrumentation compatible.
- Do not introduce unnecessary dependencies.

## B. No investigation phase

The architectural investigation has already been completed.

You may inspect code as needed to implement a change correctly, but do not turn this assignment into another investigation or design exercise.

Do not:
- Re-evaluate whether the GC architecture should be replaced.
- Produce alternative collector architectures.
- Delay implementation to create a new research report.
- Ask for permission for each individual optimization.
- Replace implementation work with theoretical recommendations.

If an implementation detail requires a decision, choose the safest implementation consistent with this directive and the existing architecture.

If a genuine contradiction or blocker makes an approved upgrade impossible without changing the architecture or language semantics, stop only that affected part, document the exact blocker, and continue with independent upgrades where safe.

## C. No benchmarking

**Benchmarking is explicitly prohibited until Muhammad Tahmid Ahnaf authorizes it.**

Do not:
- Run performance benchmarks.
- Create new benchmark suites.
- Modify existing benchmark workloads or expected results.
- Produce performance comparison tables.
- Claim a measured speedup.
- Tune code based on newly generated benchmark results.

You may compile the project, run existing correctness tests, run GC stress tests, run sanitizers, and inspect generated IR or assembly when necessary for correctness.

Do not execute benchmark programs under another name or disguise performance measurement as testing.

The existing benchmark suite must remain unchanged.

## D. Correctness verification is mandatory

No benchmarking does not mean no testing.

Run the relevant existing correctness tests and add focused correctness tests where necessary for the changes.

At minimum, verify:

- Nested mutator entry and exit.
- Recursive managed calls.
- Nested root frames.
- Early returns and exception unwinding.
- Thread creation and termination.
- Collection requests racing with mutator entry/exit.
- Root visibility during collection.
- Managed reference replacement and null stores.
- Old-to-young reference tracking.
- Concurrent marking while references are modified.
- Remembered-buffer draining.
- TLAB exhaustion and refill.
- Allocation-triggered collection.
- Repeated collections.
- Cyclic object graphs.
- Existing GC stress-test correctness.

Use sanitizers where supported and practical, particularly ThreadSanitizer and AddressSanitizer.

Do not claim tests passed unless they were actually executed and passed.

---

# 10. Code Quality Requirements

- Prefer simple, maintainable synchronization protocols.
- Avoid unnecessary abstractions and excessive indirection.
- Avoid speculative optimizations outside the five approved upgrades.
- Avoid global state additions unless genuinely required.
- Avoid unnecessary heap allocations in GC hot paths.
- Avoid introducing allocations into safepoint polling.
- Keep thread-local state properly initialized and cleaned up.
- Document synchronization invariants near the code they protect.
- Document subtle atomic memory-ordering requirements.
- Handle errors and partial initialization safely.
- Keep debug assertions for critical invariants where appropriate.
- Avoid silently swallowing errors.
- Do not leave TODOs in place of required implementation.

Do not remove comments that document existing correctness invariants unless they are replaced with accurate updated explanations.

---

# 11. Scope Protection

The following are OUT OF SCOPE:

- Replacing the garbage collector.
- Switching to reference counting as the primary GC.
- Introducing a new memory ownership model.
- Introducing a new allocator architecture.
- Rewriting the compiler.
- Changing Nirqoba language syntax.
- Changing user-facing GC APIs.
- Disabling GC instrumentation.
- Weakening root tracking.
- Removing write barriers without a correctness-preserving replacement.
- Implementing unrelated networking, standard-library, GPU, or language features.
- Modifying benchmark workloads or benchmark methodology.
- Claiming performance improvements without authorized measurements.

Do not expand the scope.

---

# 12. Implementation Order

Implement in this order:

1. **GC 1.1 — Atomic safepoint fast path**
2. **GC 1.2 — Thread-local mutator lifecycle**
3. **GC 1.3 — Thread-local shadow-stack roots**
4. **GC 1.4 — Optimized write barriers and remembered buffers**
5. **GC 1.5 — Rooted allocation and TLAB improvements**

Respect dependencies between upgrades.

After each upgrade:
- Compile affected components.
- Run relevant correctness tests.
- Fix regressions before proceeding.
- Keep the codebase in a buildable state.
- Update internal documentation where implementation details have materially changed.

Do not skip ahead if an earlier upgrade leaves the runtime in an unsafe or broken state.

---

# 13. Required Final Deliverables

When implementation is complete, provide a concise but technically meaningful report.

## A. Completion summary

For each upgrade, state:

- Implemented / Partially implemented / Not implemented.
- Files changed.
- Main implementation changes.
- Synchronization removed, reduced, or retained.
- Important invariants preserved.

## B. Correctness report

List:
- Tests executed.
- Tests passed.
- Tests failed.
- Sanitizers executed, if any.
- Known issues and unresolved risks.

Distinguish between tests that were added and tests that were actually run.

## C. Technical explanation

Explain how the five changes interact.

Include:
- Safepoint coordination.
- Mutator registration lifecycle.
- Root-stack enumeration.
- Write-barrier behavior.
- Remembered-buffer lifecycle.
- Rooted allocation and TLAB publication.

## D. Deferred performance validation

Explicitly state:

**No benchmarks were run, as instructed. Performance improvements remain unmeasured and must not be presented as proven.**

Do not invent speedup percentages or estimated performance gains.

## E. Remaining work

List only genuine blockers, incomplete requirements, and correctness concerns.

Do not fill this section with speculative future features.

---

# 14. Final Authority

The project owner is Muhammad Tahmid Ahnaf (@builtbyahnaf).

The five upgrades and their priorities are already approved.

The coding agent is authorized to implement them directly within the scope of this directive.

Do not reinterpret the project vision, replace the approved architecture, or expand the assignment.

**Final objective: make Nirqoba's existing garbage collector substantially more efficient by moving ordinary mutator operations onto thread-local and atomic fast paths, while preserving precise roots, generational correctness, concurrent collection safety, and the existing runtime architecture.**

Implement the code. Verify correctness. Do not benchmark until explicitly authorized.