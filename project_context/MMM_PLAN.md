# Nirqoba Memory Model Migration Plan
## Migration to an Ownership-First, Deterministic Hybrid Memory Architecture

**Project:** Nirqoba  
**Milestone:** Pre-PO-28 — Memory Model Migration  
**Priority:** Critical architectural migration  
**Status:** Implementation specification  
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)
**Primary objective:** Establish deterministic ownership as Nirqoba's default memory-management model, with borrowing, arenas, explicit shared ownership, selective garbage collection, and advanced manual memory management as complementary mechanisms.

---

# 1. Mission and Architectural Objective

Nirqoba must transition to an **ownership-first hybrid memory model**.

The language should not require garbage collection for ordinary memory allocation, object lifetime management, or routine application execution.

Instead, its memory architecture must provide several complementary memory-management strategies, each suited to a different lifetime and ownership requirement.

The intended model is:

1. **Deterministic ownership:** Default for ordinary values and objects.
2. **Borrowing:** Non-owning access without unnecessary allocations or copies.
3. **Arena and pool allocation:** Efficient allocation and bulk destruction for grouped lifetimes.
4. **Explicit shared ownership:** Reference-counted ownership when multiple independent owners genuinely need to share a lifetime.
5. **Selective garbage collection:** For genuinely dynamic, cyclic, graph-like, or otherwise unpredictable object lifetimes.
6. **Manual memory management:** An advanced escape hatch for low-level systems programming and specialized use cases.

These mechanisms must coexist without forcing every allocation through one universal memory-management subsystem.

## 1.1 The fundamental rule

**If a program does not use GC-managed objects, its ordinary execution must not depend on the garbage collector.**

Purely owned and borrowed code must not incur GC-specific:

- Allocation paths.
- Root registration.
- Root-frame construction.
- Safepoint insertion.
- Mutator registration.
- Write barriers.
- GC object headers or metadata.
- Collection bookkeeping.
- GC synchronization.
- GC-related synchronization overhead.

This is a semantic and architectural requirement, not merely a performance optimization.

A program may still use the standard runtime for unrelated functionality. The requirement is specifically that ordinary owned/borrowed memory operations must not be routed through GC infrastructure.

## 1.2 What this migration is not

Do not interpret this task as:

- Removing the garbage collector.
- Making garbage collection unavailable by default.
- Renaming GC-managed allocations to owned allocations without changing their semantics.
- Adding a few ownership annotations while retaining universal GC allocation.
- Implementing reference counting as the replacement for GC everywhere.
- Replacing the existing runtime with a completely new one without architectural justification.
- Sacrificing memory safety to improve benchmark results.

The objective is a genuine change in the language's memory semantics and compiler/runtime architecture.

---

# 2. Mandatory Preliminary Investigation

Before modifying code, perform a complete investigation of the existing implementation.

Do not assume that the current allocation model, type representation, or compiler lowering works in any particular way.

Inspect at minimum:

- Lexer and parser.
- Syntax definitions and language documentation.
- AST and semantic analysis.
- Type system and type checking.
- TIR representation and transformations.
- TIR-to-MLIR lowering.
- MLIR passes and optimization pipeline.
- LLVM IR generation.
- Runtime allocation APIs.
- Garbage collector implementation.
- GC root registration and root-frame management.
- Mutator registration and safepoint mechanisms.
- Write-barrier implementation.
- Exception and panic handling.
- Function-call and return-value lowering.
- Container and aggregate representations.
- FFI and external function interfaces.
- Async/task infrastructure, if present.
- Existing tests and benchmarks.

Pay particular attention to:

1. Which allocations are currently managed by GC.
2. Whether ordinary local variables, arrays, structures, strings, and returned objects use GC-backed storage.
3. Whether the compiler inserts root frames for functions that never use GC.
4. Whether GC effects are inferred transitively through function calls.
5. Whether ownership and destruction are already represented in TIR or another intermediate representation.
6. Whether the current runtime can support non-GC allocation without introducing a second unnecessary abstraction layer.
7. Whether existing language features implicitly depend on GC lifetime guarantees.

## 2.1 Required investigation report

Before implementation, produce:

`reports/memory-model/migration-audit.md`

Include:

- Current memory architecture.
- Allocation paths by object category.
- Current object lifetime rules.
- GC dependency map.
- Compiler passes responsible for GC integration.
- Runtime APIs and their callers.
- Existing ownership-related mechanisms.
- Compatibility risks.
- Recommended migration sequence.
- Files expected to change.
- Existing tests that may need semantic updates.

For every important finding, identify the relevant source file and function or class.

Do not fabricate implementation details. If a feature is absent, explicitly report that it is absent.

---

# 3. Target Memory Architecture

The following hierarchy defines the intended architecture.

## 3.1 Deterministic ownership — Default

Ordinary values and objects should have an identifiable owner.

The owner is responsible for the object's lifetime. When ownership ends, destruction and resource cleanup happen deterministically.

Required properties:

- Predictable lifetime.
- Deterministic destruction.
- No dependence on GC scheduling.
- No implicit GC registration.
- No GC safepoints for ordinary owned objects.
- No unnecessary reference counting.
- No hidden heap allocation when stack or inline storage is sufficient.
- Efficient move and return semantics.
- Correct destruction during normal control flow and exceptional control flow.

The implementation should favor stack or inline storage where valid, and ordinary non-GC heap allocation when dynamic storage is necessary.

Do not force every owned object onto the stack. Ownership and storage location are separate concerns.

A heap-allocated owned object is still an owned object.

## 3.2 Borrowing — Non-owning access

Borrowing allows a function, variable, or operation to access an existing object without acquiring ownership or duplicating the object.

The compiler must enforce the lifetime validity of borrowed references.

Required properties:

- Borrowing does not transfer ownership.
- Borrowing does not extend the owner's lifetime implicitly.
- Borrowed references cannot outlive their owners.
- Mutable access must obey the language's aliasing rules.
- Borrowing should avoid unnecessary copies.
- Borrowed access must not introduce GC tracking.
- Borrowed references must not silently become owning references.

The compiler must distinguish owning references from borrowed references in its internal representations.

Do not implement borrowing as ordinary raw pointers with no lifetime validation.

## 3.3 Arenas and memory pools — Grouped lifetimes

Arenas and pools should support objects that naturally share a lifetime.

Examples include:

- Temporary compiler data.
- Request-scoped allocations.
- Batch processing.
- Frame-based graphics workloads.
- Temporary numerical computation data.
- Large groups of short-lived objects.

Required properties:

- Cheap allocation.
- Predictable bulk destruction.
- Minimal per-object bookkeeping.
- No mandatory GC participation.
- Clear arena ownership and lifetime boundaries.
- Safe handling of references into an arena.
- Explicit behavior when an arena is reset or destroyed.

Arena-backed objects must not remain accessible after their arena lifetime ends.

Do not treat an arena as a GC heap with a different name.

## 3.4 Explicit shared ownership — Reference counting

Some objects genuinely require multiple independent owners.

Provide an explicit shared-ownership mechanism for these situations.

Required properties:

- Shared ownership must be distinguishable from ordinary ownership.
- Reference counts must be updated correctly.
- The object must be destroyed when the last strong owner releases it.
- Weak references should be considered where they solve real ownership-cycle problems.
- Atomic reference counting should be used when ownership can cross threads.
- Single-threaded cases should not automatically pay for unnecessary atomic operations.
- Reference-count overflow and underflow must be handled safely.
- Ownership transitions must be exception-safe.

Do not silently convert ordinary owned objects into reference-counted objects.

Do not assume that reference counting alone solves cyclic ownership.

## 3.5 Selective garbage collection — Opt-in capability

GC remains an important part of Nirqoba.

It should serve workloads where deterministic ownership is inconvenient, overly complex, or insufficiently expressive.

Appropriate use cases include:

- Cyclic object graphs.
- Dynamic graph structures.
- Complex object relationships.
- Runtime-generated object graphs.
- Objects whose lifetime cannot be conveniently expressed through ordinary ownership.
- Selected application-level dynamic data structures.

Required properties:

- Explicitly identifiable GC-managed objects or regions.
- Correct root discovery.
- Correct object tracing.
- Correct collection behavior.
- Correct mutator synchronization.
- Correct write barriers where required.
- Correct handling of cycles.
- Correct interaction with owned and borrowed objects.

GC must remain available without becoming a hidden requirement for ordinary memory allocation.

The existing collector should be preserved and improved where possible. Do not discard working GC infrastructure merely to simplify the migration.

## 3.6 Manual memory management — Advanced capability

Manual memory management should remain possible for specialized low-level code.

It must be explicit and clearly separated from safe ownership-managed behavior.

Requirements:

- Explicit allocation and deallocation.
- Clear ownership responsibility.
- No implicit assumption that manually managed objects are GC-managed.
- Well-defined interaction with FFI.
- Safe-mode restrictions where appropriate.
- Correct behavior when manual allocations interact with other memory-management strategies.

Do not allow manual deallocation of memory still owned by another mechanism.

---

# 4. Ownership Semantics and Language Rules

The migration must establish actual language semantics, not merely runtime conventions.

## 4.1 Ownership identity

Every owning value must have a clearly defined ownership state.

The semantic model should distinguish at least:

- Owned.
- Borrowed.
- Mutably borrowed.
- Shared-owned.
- Weakly referenced, if implemented.
- Arena-bound.
- GC-managed.
- Manually managed.

These categories must not be conflated.

The exact syntax must be derived from the existing Nirqoba language design.

Inspect `SYNTAX.md` and existing language conventions before proposing new syntax.

Do not invent an entirely new syntax family if existing syntax can express the required semantics cleanly.

If the current syntax cannot express a necessary distinction, document the proposed syntax in a separate design proposal before implementing it.

## 4.2 Ownership transfer and move semantics

Implement correct ownership transfer.

When an owned value moves to another owner:

- The previous owner must no longer be permitted to use it as an owner.
- The new owner becomes responsible for destruction.
- The object must not be copied unnecessarily.
- The object must not be destroyed twice.
- The object must not be leaked because both owners believe the other is responsible.
- Destruction must occur exactly once.

Move semantics should work for:

- Local variables.
- Function arguments.
- Function returns.
- Structures and aggregates.
- Arrays and containers.
- Nested owned values.
- Heap-backed objects.

Support efficient return-value optimization and equivalent compiler transformations where valid.

A move must not automatically imply a physical memory copy.

## 4.3 Copy semantics

Define which values are:

- Copyable.
- Move-only.
- Borrowable.
- Shareable.

Copying an object must have explicit and predictable semantics.

Do not accidentally duplicate ownership of a non-copyable resource.

For ordinary small value types, preserve efficient value-copy semantics where appropriate.

The implementation must distinguish a semantic copy from a compiler-generated physical copy that can safely be eliminated.

## 4.4 Borrowing rules

Implement compiler-enforced borrowing.

At minimum:

- A borrow cannot outlive its owner.
- A mutable borrow must obey exclusive-access requirements.
- Multiple immutable borrows may coexist where permitted.
- Mutable and immutable borrows must not violate the language's aliasing model.
- A borrowed value cannot be destroyed by the borrower.
- Borrowed references cannot silently escape into longer-lived storage.
- Returning a borrow must be valid relative to the source lifetime.
- Storing a borrow inside another object must preserve the lifetime relationship.

The compiler must reject invalid borrowing at compile time whenever the lifetime violation is statically detectable.

Do not rely on runtime checks as a substitute for statically enforceable ownership rules.

## 4.5 Lifetime inference

Prefer inference where it is safe and unambiguous.

Do not force users to annotate every ordinary local variable or every straightforward borrow.

However, inference must not hide ownership ambiguity.

When the compiler cannot safely establish a lifetime relationship, it should:

1. Reject the program with a useful diagnostic, or
2. Require an explicit lifetime or ownership declaration supported by the language.

Diagnostics should identify:

- The borrowed value.
- Its owner.
- The invalid lifetime relationship.
- The operation causing the escape.
- A possible correction.

## 4.6 Destruction order

Define deterministic destruction order.

At minimum:

- Local owned values must be destroyed when their owning scope exits.
- Nested owned fields must be destroyed correctly.
- Destruction order must be documented and stable.
- Partially initialized aggregates must clean up successfully initialized members.
- Moved-from values must not be destroyed as though they still own their original resources.
- Early returns must preserve destruction guarantees.
- Exceptions and panics must not skip required cleanup.
- Destruction must not happen twice.

Use a consistent reverse-initialization destruction order where appropriate.

Do not leave destruction behavior dependent on incidental lowering order.

## 4.7 Exception and panic safety

Ownership must remain correct under all supported control-flow exits.

Test:

- Normal scope exit.
- Early return.
- Error propagation.
- Exception unwinding, if supported.
- Panic handling.
- Partially completed initialization.
- Failed allocation.
- Failed construction.
- Nested function calls.

A failed operation must not leave ownership in an ambiguous state.

---

# 5. Compiler Architecture Migration

The compiler must understand memory-management strategy as part of program semantics.

Do not implement this migration solely inside the runtime.

## 5.1 Introduce explicit memory-management metadata

Extend the compiler's semantic representation and intermediate representations as necessary.

The internal representation should be capable of expressing:

- Ownership origin.
- Ownership transfer.
- Borrowed references.
- Borrow lifetimes.
- Destruction obligations.
- Arena membership.
- Shared ownership.
- GC-managed allocation.
- Manual allocation.
- Escape relationships.
- Relevant thread-safety constraints.

The representation must survive the required compiler transformations.

Do not rely on source-level syntax information that disappears before lowering.

## 5.2 TIR integration

TIR must preserve enough information to implement ownership correctly.

Review the current TIR model and determine whether ownership should be represented using:

- Explicit ownership annotations.
- Dedicated ownership operations.
- Lifetime markers.
- Destruction operations.
- Borrow and move operations.
- Allocation strategy metadata.

Choose the smallest coherent design that supports the complete target architecture.

Avoid introducing redundant representations of the same ownership fact.

TIR transformations must preserve ownership invariants.

A pass must never:

- Duplicate an owning value without a valid copy operation.
- Remove a required destructor.
- Move an object while leaving a valid owner behind.
- Extend a borrow beyond its owner's lifetime.
- Convert an owned allocation into a GC allocation without semantic justification.
- Eliminate a required cleanup operation.

## 5.3 Ownership and lifetime analysis

Implement or extend compiler analysis to determine:

- Where objects are created.
- Which scope owns them.
- Whether ownership moves.
- Whether an object is borrowed.
- Whether an object escapes its current scope.
- Whether an object requires heap storage.
- Whether destruction is required at a particular control-flow boundary.
- Whether a lifetime is arena-bound.
- Whether an object is explicitly GC-managed.

Use control-flow and data-flow analysis where necessary.

The analysis must account for:

- Branches.
- Loops.
- Recursive functions.
- Function calls.
- Function returns.
- Nested scopes.
- Aggregates.
- Aliasing.
- Early exits.
- Exceptional control flow.

Do not implement ownership analysis using a simplistic linear scan.

## 5.4 Interprocedural ownership summaries

Function boundaries must preserve ownership information.

The compiler should be able to determine whether a function:

- Receives an owned value.
- Borrows a value.
- Mutably borrows a value.
- Returns ownership.
- Returns a borrow tied to an input lifetime.
- Creates GC-managed objects.
- Uses shared ownership.
- Requires arena-bound values.

Propagate relevant effects across function calls.

Do not assume a caller is free of GC effects simply because it does not directly allocate GC objects.

Likewise, do not mark every caller as GC-dependent merely because one unrelated code path in the program uses GC.

Use function-level and call-site-sensitive information where practical.

## 5.5 GC effect separation

This is a critical requirement.

The compiler must distinguish:

- Requires GC allocation.
- Requires GC root tracking.
- Requires GC safepoints.
- Requires mutator participation.
- Requires write barriers.
- Requires GC collection coordination.
- Requires ordinary allocation.
- Requires deterministic destruction.

These are not interchangeable effects.

A function that only allocates owned objects must not automatically be classified as GC-dependent.

A function that uses a GC-managed object must receive the GC behavior necessary for correctness.

Propagate GC effects through calls accurately.

Where possible, specialize or split code paths so that GC-specific behavior is not imposed on unrelated owned-only paths.

## 5.6 MLIR and LLVM lowering

Inspect the existing lowering architecture before modifying it.

Ensure that:

- Owned allocations use appropriate non-GC allocation paths.
- Stack-eligible values remain stack or register candidates.
- Owned heap allocations use deterministic destruction.
- Borrowed references do not create unnecessary allocations.
- Arena allocations use their designated allocator.
- Shared-owned objects use the shared-ownership implementation.
- GC-managed objects continue through the GC-aware lowering path.
- Manual allocations use their explicit allocation path.

GC root frames, safepoints, mutator operations, and write barriers must only be emitted when required by the actual GC effects and object lifetimes.

Do not remove GC instrumentation from functions that genuinely need it.

Preserve LLVM optimization opportunities.

The compiler should continue to benefit from:

- Scalar replacement.
- Escape analysis.
- Dead-store elimination.
- Inlining.
- Stack allocation where valid.
- Return-value optimization.
- Loop optimization.
- Vectorization.
- Existing optimization pipeline infrastructure.

Ownership metadata must not unnecessarily block LLVM optimization.

---

# 6. Runtime Architecture Migration

The runtime must expose distinct allocation and lifetime mechanisms.

## 6.1 Owned allocation path

Provide a fast allocation and deallocation path for ordinary owned heap objects.

Requirements:

- No GC registration.
- No GC root-frame operations.
- No GC write barriers.
- No GC safepoints.
- No mandatory GC object header.
- No GC mutex acquisition.
- Predictable destruction.
- Correct alignment.
- Correct allocation and deallocation pairing.
- Appropriate allocator selection.

Use existing allocator infrastructure where suitable.

Do not build a custom general-purpose allocator unless measurements demonstrate that existing options are inadequate.

## 6.2 Destruction and cleanup runtime

Implement the minimum runtime support necessary for deterministic cleanup.

Where destruction can be resolved statically, prefer compiler-generated cleanup rather than dynamic runtime registries.

If dynamic destruction metadata is necessary for a particular type or container, keep it proportional to the actual requirement.

Requirements:

- Exactly-once destruction.
- Correct nested destruction.
- Correct partial initialization cleanup.
- Correct handling of moved values.
- Exception-safe cleanup.
- No GC dependency.

## 6.3 Arena runtime

Provide or extend arena and pool facilities.

Requirements:

- Efficient bump allocation where appropriate.
- Correct alignment.
- Safe capacity growth or explicit capacity failure.
- Efficient bulk release.
- Correct reset semantics.
- No use-after-reset.
- Correct handling of arena ownership.
- No hidden GC dependency.

Do not make arena destruction silently invalidate unrelated external allocations.

## 6.4 Shared ownership runtime

Implement shared ownership with carefully designed control blocks.

Requirements:

- Strong reference count.
- Weak reference support if needed.
- Correct object destruction.
- Thread-safe behavior when shared across threads.
- No atomic overhead in explicitly single-threaded implementations where safely avoidable.
- Correct overflow handling.
- Correct weak-reference expiration.
- No use-after-free during concurrent release.

Benchmark the control-block layout and allocation strategy.

Avoid allocating a separate control block and object when a combined allocation is safe and beneficial.

## 6.5 GC runtime isolation

Keep the GC subsystem functional, but separate its responsibilities from ordinary allocation.

The GC subsystem must continue to support:

- Managed allocation.
- Root discovery.
- Root frames.
- Mutator coordination.
- Safepoints.
- Write barriers.
- Tracing.
- Collection.
- Cyclic graph reclamation.
- Existing correctness guarantees.

The collector must not need to inspect or manage ordinary owned objects unless an explicit, well-defined interoperability mechanism requires it.

Do not allow the collector to reclaim an object still owned by the deterministic ownership system.

## 6.6 Allocation metadata

Review object headers and metadata.

Ordinary owned objects should not carry GC-only metadata unless a separately justified runtime requirement exists.

GC-managed objects may retain the metadata needed by the collector.

Shared-owned objects may carry or reference their own control block.

Arena objects may rely on arena-level metadata.

Avoid one universal object header containing every possible memory-management field.

Prefer strategy-specific metadata and layouts.

---

# 7. Interoperability Between Memory Strategies

The memory strategies must coexist safely.

This section is mandatory. A hybrid model is not complete if its individual strategies work but their interactions are undefined.

## 7.1 Owned object passed to a borrowed parameter

This should be supported when the borrow does not outlive the owner.

The callee must not acquire ownership or destroy the object.

The compiler must enforce the relevant lifetime.

## 7.2 Owned object transferred to another owner

Ownership must transfer exactly once.

The previous owner must no longer be able to perform an owning operation on the object.

## 7.3 Owned object referenced by a GC-managed object

Define an explicit and safe interoperability rule.

A GC-managed object must not retain a non-owning pointer to an owned object beyond the owner's lifetime.

Possible supported strategies include:

- Explicit shared ownership.
- Explicit lifetime-bound borrowing where the compiler can prove safety.
- An explicitly managed handle or registration mechanism with well-defined cleanup.

Do not silently turn the owned object into a GC object.

Do not permit the GC object to outlive the owned object while retaining an invalid reference.

## 7.4 GC-managed object referenced by an owned object

The owned object must not accidentally destroy the GC-managed object.

Its reference must preserve the GC root and lifetime guarantees required by the collector.

If storing a GC reference in an owned object requires root tracking, implement the necessary mechanism explicitly and correctly.

Do not impose GC tracking on every field of every owned object.

## 7.5 Arena references

An arena-bound reference must not outlive the arena.

The compiler should reject invalid lifetime escapes.

If the language supports explicit arena transfer or ownership transfer, define its semantics precisely.

## 7.6 Shared ownership and cycles

Reference counting cannot reclaim strong-reference cycles on its own.

Provide clear guidance and a supported mechanism for cyclic relationships.

Possible approaches include:

- Weak references.
- Explicit GC-managed cyclic structures.
- Carefully designed ownership graphs.

Do not silently invoke GC for all shared-owned objects.

## 7.7 FFI boundaries

FFI must have explicit ownership contracts.

Document how external code receives and returns:

- Borrowed pointers.
- Owned allocations.
- Shared-owned handles.
- GC-managed handles.
- Arena-bound pointers.
- Manually managed memory.

A foreign function must not accidentally retain a pointer after its lifetime ends.

If a foreign library requires a different allocation or destruction mechanism, provide an explicit adapter.

## 7.8 Threads and concurrency

Define which ownership strategies can cross thread boundaries.

Requirements:

- No data races introduced by ownership transfer.
- Correct shared ownership across threads.
- Clear rules for borrowed values crossing threads.
- Correct thread-local arena behavior.
- Correct GC interaction with mutator registration and safepoints.
- No deadlocks caused by ownership cleanup.
- Correct destruction when the final owner is released on a different thread.

A non-thread-safe owned object must not become thread-safe merely because it is heap allocated.

---

# 8. Syntax and Standard-Library Integration

Do not finalize new syntax before reviewing existing conventions.

The migration should first establish semantics and internal representation, then expose the smallest coherent user-facing API.

Potential language-level capabilities include:

- Ownership transfer.
- Borrowed parameters.
- Mutable borrowing.
- Explicit shared ownership.
- Arena creation and scoped allocation.
- Explicit GC-managed allocation.
- Explicit manual allocation.

These are conceptual requirements, not mandatory spellings.

## 8.1 Syntax compatibility

Before introducing syntax:

1. Inspect `SYNTAX.md`.
2. Inspect current parser behavior.
3. Inspect existing ownership-related syntax.
4. Check for conflicts with existing identifiers and operators.
5. Prepare examples.
6. Document ambiguous cases.
7. Add parser and semantic tests.

Do not introduce syntax that conflicts with existing Nirqoba grammar.

Do not make GC opt-in by requiring users to wrap all ordinary code in special ownership syntax.

Ordinary code should naturally use the default ownership model.

## 8.2 Standard-library facilities

Provide a coherent standard-library interface for the supported strategies.

Potential facilities:

- Owned collections.
- Borrowed views and slices.
- Shared ownership wrappers.
- Weak references.
- Arena and pool abstractions.
- Explicit GC-managed collections.
- Manual allocation utilities.

Do not implement unnecessary containers solely to demonstrate the memory model.

Prioritize the minimum facilities required to exercise and validate the architecture.

---

# 9. Migration Strategy

Do not attempt a risky, all-at-once rewrite.

Migrate incrementally while keeping the compiler buildable and testable.

## Phase 0 — Architecture audit and design freeze

Deliverables:

- `reports/memory-model/migration-audit.md`
- `reports/memory-model/target-architecture.md`
- Memory strategy classification.
- Ownership semantic specification.
- Interoperability rules.
- Syntax proposal, only if required.
- Migration dependency map.

Acceptance:

- Every current allocation category has a defined target strategy.
- Existing GC dependencies are identified.
- The new default is unambiguously deterministic ownership.
- No unresolved critical ownership ambiguity remains.

## Phase 1 — Ownership representation

Implement the semantic and intermediate-representation foundations.

Tasks:

1. Introduce ownership categories into the compiler's semantic model.
2. Represent ownership transfer.
3. Represent borrowing and mutability.
4. Represent lifetime relationships.
5. Represent destruction obligations.
6. Preserve ownership metadata through TIR.
7. Add validation for invalid ownership transformations.
8. Add initial diagnostic support.

Acceptance:

- The compiler can distinguish owned and borrowed values internally.
- Ownership transfer is represented explicitly.
- Invalid ownership states can be detected.
- Existing GC functionality remains operational.

## Phase 2 — Deterministic destruction and move semantics

Implement:

1. Move semantics.
2. Ownership transfer across function calls.
3. Ownership transfer through return values.
4. Scope-based cleanup.
5. Nested object destruction.
6. Partial initialization cleanup.
7. Early-return cleanup.
8. Exception and panic cleanup where supported.
9. Exactly-once destruction guarantees.

Acceptance:

- Owned values are destroyed deterministically.
- No double destruction.
- No ownership leaks in tested control-flow paths.
- Move operations avoid unnecessary physical copying.
- Existing language behavior remains compatible where intended.

## Phase 3 — Borrowing and lifetime enforcement

Implement:

1. Immutable borrowing.
2. Mutable borrowing.
3. Borrow lifetime inference.
4. Lifetime validation across calls.
5. Borrow validation across returns.
6. Borrow validation in aggregates and containers.
7. Escape analysis.
8. Useful lifetime diagnostics.

Acceptance:

- Invalid borrowed references are rejected.
- Valid non-owning access does not allocate.
- Borrowing does not introduce GC behavior.
- The compiler handles branches, loops, recursion, and nested calls correctly.

## Phase 4 — Ordinary non-GC allocation

Implement the default owned allocation path.

Tasks:

1. Identify every ordinary allocation site.
2. Replace GC-backed ordinary allocations with the appropriate owned allocation path.
3. Preserve stack and inline allocation opportunities.
4. Implement deterministic heap deallocation.
5. Remove unnecessary GC headers from ordinary owned objects.
6. Remove GC root tracking from owned-only code.
7. Remove GC safepoints from owned-only code.
8. Remove unnecessary mutator registration.
9. Remove unnecessary write barriers.
10. Verify all compiler-generated cleanup.

This phase is the architectural turning point.

Acceptance:

- Ordinary allocation is no longer GC-dependent.
- Owned-only functions do not receive GC root frames.
- Owned-only functions do not receive GC safepoints.
- Owned-only functions do not use GC allocation APIs.
- Owned-only code has no unnecessary GC metadata or synchronization.
- GC-managed allocations continue to work.

## Phase 5 — Arena and pool support

Implement or integrate arena and pool allocation.

Tasks:

1. Establish arena lifetime semantics.
2. Add efficient allocation.
3. Add bulk destruction.
4. Add reset semantics.
5. Enforce arena-bound references.
6. Add compiler validation for arena escapes.
7. Add appropriate runtime diagnostics in debug builds.

Acceptance:

- Arena allocations are independent of GC.
- Bulk release is correct.
- Invalid lifetime escapes are rejected.
- Allocation and destruction are predictable.

## Phase 6 — Explicit shared ownership

Implement shared ownership and weak references where justified.

Tasks:

1. Define control-block semantics.
2. Implement strong reference counting.
3. Implement weak references if required.
4. Handle destruction on final release.
5. Handle thread-safe sharing.
6. Prevent count overflow and underflow.
7. Define cycle-handling guidance.
8. Add concurrency tests.

Acceptance:

- Shared-owned objects remain alive while strong owners exist.
- Destruction happens exactly once after the final strong owner releases.
- Weak references expire correctly.
- Cross-thread sharing is correct.
- Ordinary owned objects do not pay reference-counting costs.

## Phase 7 — GC isolation and explicit selection

Ensure that GC is a separate, selectively invoked memory-management strategy.

Tasks:

1. Audit all GC allocation entry points.
2. Define the explicit GC-managed allocation path.
3. Verify root registration and root-frame behavior.
4. Verify transitive GC effect propagation.
5. Verify safepoint insertion.
6. Verify write-barrier insertion.
7. Verify mutator coordination.
8. Verify GC object interoperability.
9. Ensure owned-only functions are excluded from GC instrumentation.
10. Preserve the existing collector's correctness.

Acceptance:

- GC-managed programs continue to collect correctly.
- Cyclic structures are reclaimable.
- Ordinary owned objects are not accidentally collected.
- GC-specific behavior is only emitted where required.
- Mixed-strategy programs are correct.

## Phase 8 — Manual memory and FFI integration

Implement or validate:

- Explicit allocation and deallocation.
- External allocator integration.
- Ownership transfer across FFI.
- Borrowed foreign pointers.
- Explicitly managed handles.
- Correct cleanup contracts.

Acceptance:

- Manual memory is clearly separated from ordinary ownership.
- FFI cannot silently violate ownership rules.
- Allocation and deallocation pairing is correct.

## Phase 9 — Optimization, compatibility, and stabilization

Only after semantic correctness is established:

1. Optimize owned allocation.
2. Optimize destruction lowering.
3. Optimize borrow handling.
4. Optimize arena allocation.
5. Optimize shared ownership.
6. Optimize GC isolation.
7. Remove redundant metadata.
8. Reduce unnecessary control-flow cleanup.
9. Verify LLVM optimization compatibility.
10. Measure performance and memory usage.

Do not optimize by weakening ownership guarantees.

---

# 10. Critical Performance Requirements

The ownership-first migration must improve architecture without introducing hidden runtime costs.

## 10.1 Owned-only hot path

A representative owned-only program must not:

- Initialize the GC unnecessarily.
- Register a mutator unnecessarily.
- Create GC root frames.
- Enter GC safepoints.
- Invoke GC allocation functions.
- Update GC root tables.
- Execute GC write barriers.
- Acquire GC-specific locks.

The compiler should emit only the operations required by ordinary ownership semantics.

## 10.2 Borrowed hot path

Borrowed access should normally compile to direct access to the underlying object.

Avoid:

- Hidden heap allocations.
- Hidden copies.
- Runtime lifetime registries.
- Reference-count updates.
- GC registration.

A debug build may perform additional validation if necessary, but release builds must not carry unnecessary runtime checks when the compiler can prove validity statically.

## 10.3 Shared ownership

Shared ownership has legitimate reference-counting overhead.

Optimize it without imposing the same overhead on ordinary ownership.

## 10.4 Arena allocation

Arena allocation should provide a cheap allocation path and efficient bulk destruction.

Measure both allocation throughput and memory retention.

## 10.5 GC

GC performance remains important, but it must be evaluated separately from the default ownership path.

Do not use GC benchmark improvements as evidence that the ownership migration is complete.

---

# 11. Testing and Validation

A memory-model migration is incomplete without extensive semantic and runtime tests.

Create a dedicated test suite:

`tests/memory_model/`

Organize tests by strategy.

## 11.1 Ownership tests

Include:

- Basic owned allocation.
- Stack-owned values.
- Heap-owned values.
- Ownership transfer.
- Move-only objects.
- Nested owned fields.
- Function argument transfer.
- Function return transfer.
- Branch-dependent ownership.
- Loop-dependent ownership.
- Recursive calls.
- Exactly-once destruction.
- Partial initialization failure.
- Early return.
- Exception and panic cleanup where supported.

## 11.2 Borrowing tests

Include:

- Immutable borrow.
- Multiple immutable borrows.
- Mutable borrow.
- Invalid mutable aliasing.
- Borrow after owner destruction.
- Returning a borrow.
- Borrow stored in an aggregate.
- Borrow escaping a scope.
- Borrow across function calls.
- Borrow in loops.
- Borrow through recursive calls.

Each invalid program should produce a compile-time diagnostic where the violation is statically detectable.

## 11.3 Arena tests

Include:

- Basic allocation.
- Bulk destruction.
- Arena reset.
- Nested arena usage.
- Arena-bound references.
- Invalid arena escape.
- Alignment.
- Capacity exhaustion.
- Large batch allocation.

## 11.4 Shared ownership tests

Include:

- One owner.
- Multiple owners.
- Copying a shared handle.
- Releasing owners in different orders.
- Final-owner destruction.
- Weak reference expiration.
- Reference-count underflow protection.
- Overflow protection.
- Cross-thread ownership.
- Cyclic relationship behavior.

## 11.5 GC tests

Preserve and extend existing tests:

- Basic managed allocation.
- Root discovery.
- Root-frame correctness.
- Collection.
- Cyclic graph reclamation.
- Mutator coordination.
- Safepoint correctness.
- Write barriers.
- Multi-threaded GC.
- Mixed owned/GC object graphs.

## 11.6 Mixed-strategy tests

These are especially important.

Test:

- Owned object borrowed by GC-managed code.
- GC-managed object referenced by owned code.
- Shared-owned object referenced by GC-managed code.
- Arena-bound object passed to a borrowed parameter.
- GC-managed graph containing valid owned handles.
- FFI-owned object passed into Nirqoba.
- Owned objects returned from functions that also use GC.
- GC operations in a program containing large owned-only regions.

---

# 12. Compiler Output and Runtime Instrumentation Tests

Source-level tests alone are insufficient.

Inspect generated TIR, MLIR, LLVM IR, and runtime behavior.

Create tests proving that owned-only code does not accidentally use GC infrastructure.

For representative owned-only functions, verify the absence of unnecessary:

- GC allocation calls.
- GC root-frame setup.
- GC root registration.
- Mutator registration.
- Safepoint operations.
- Write barriers.
- GC-specific mutex operations.
- GC object-header initialization.

For GC-dependent functions, verify that the required GC infrastructure remains present.

Also verify that transitive GC effects are handled correctly.

A function that calls a GC-dependent helper must receive the necessary GC behavior when required.

A function that only calls owned-only helpers must remain GC-independent.

Use IR-level assertions where possible instead of relying exclusively on source-level timing measurements.

---

# 13. Benchmarking Plan

Create a dedicated benchmark suite:

`benchmarks/memory_model/`

Keep semantic correctness tests separate from performance benchmarks.

Measure the following strategies independently.

## 13.1 Deterministic ownership

- Stack allocation and destruction.
- Heap allocation and destruction.
- Large numbers of small objects.
- Large objects.
- Nested structures.
- Move-heavy workloads.
- Container allocation and destruction.
- Recursive data structures with deterministic ownership.

## 13.2 Borrowing

- Borrowed traversal.
- Borrowed function arguments.
- Borrowed slices.
- Read-only iteration.
- Mutable iteration.
- Comparison against equivalent owning/copying implementations.

## 13.3 Arenas

- Allocation throughput.
- Bulk destruction.
- Repeated arena reset.
- Mixed object sizes.
- High-volume temporary allocations.

## 13.4 Shared ownership

- Single-threaded reference counting.
- Multi-threaded reference counting.
- Weak-reference operations.
- Final-owner destruction.
- Shared graph traversal.

## 13.5 Garbage collection

- Allocation throughput.
- Collection latency.
- Cyclic graph construction.
- Cyclic graph reclamation.
- Peak memory.
- Retained memory.
- Pause behavior.
- Mutator overhead.

## 13.6 Required benchmark reporting

Record:

- Compiler version.
- Runtime version.
- Build configuration.
- Optimization flags.
- Target architecture.
- Operating system.
- CPU.
- Memory capacity.
- Number of repetitions.
- Median.
- Minimum and maximum.
- Standard deviation where meaningful.
- Peak resident memory.
- Allocation count.
- Destruction count.
- GC collection count where applicable.
- GC pause time where applicable.

Keep benchmark configurations comparable.

Do not compare an optimized Nirqoba build against an unoptimized reference implementation.

Do not claim superiority over C++, Rust, Java, or another language without reproducible measurements and equivalent workload semantics.

---

# 14. Performance and Correctness Acceptance Gates

The migration is not complete merely because the compiler builds.

All gates below must pass.

## Gate A — Semantic correctness

- Ownership transfer works.
- Move semantics work.
- Borrowing rules are enforced.
- Destruction occurs exactly once.
- Partial initialization is safe.
- Early exits are safe.
- Exception and panic cleanup is correct where supported.
- Arena lifetime rules are enforced.
- Shared ownership is correct.
- GC remains correct.

## Gate B — Default memory model

- Ordinary values use deterministic ownership semantics.
- Ordinary heap allocations do not require GC.
- Ordinary objects are destroyed deterministically.
- Borrowed references do not acquire ownership.
- Shared ownership is explicit.
- Arena allocation is independent of GC.
- GC-managed allocation remains available.

## Gate C — GC isolation

An owned-only program must not require:

- GC initialization.
- GC mutator registration.
- GC root frames.
- GC safepoints.
- GC allocation.
- GC write barriers.
- GC synchronization.

Verify this using generated IR and runtime instrumentation.

## Gate D — Mixed-strategy correctness

- Owned/GC interoperability is safe.
- Arena lifetime boundaries are respected.
- Shared ownership works with GC-managed references.
- FFI ownership contracts are correct.
- Thread boundaries preserve ownership guarantees.

## Gate E — Performance

- No significant regression in ordinary owned allocation.
- Borrowing avoids unnecessary copies and allocations.
- Arena allocation provides an efficient bulk-lifetime path.
- Shared ownership costs are isolated to shared-owned objects.
- GC performance remains independently measurable.
- Owned-only code does not pay for GC infrastructure.

Set concrete numerical performance thresholds after establishing a reproducible baseline. Do not invent thresholds before measuring the current implementation.

Any regression must be explained and either fixed or explicitly justified.

---

# 15. Compatibility and Migration of Existing Nirqoba Code

The migration must consider existing programs and runtime behavior.

Inspect current examples, tests, benchmarks, and standard-library usage.

Classify existing allocations into:

1. Ordinary owned values.
2. Borrowed references.
3. Arena candidates.
4. Shared-owned values.
5. GC-managed values.
6. Manual allocations.
7. Ambiguous or currently implicit ownership.

Migrate each category deliberately.

Do not blindly convert every existing GC allocation into an ordinary owned allocation.

Some existing workloads may genuinely depend on GC semantics, particularly cyclic graphs.

Where existing source code changes meaning under the new default:

- Document the change.
- Provide a clear diagnostic.
- Provide a migration example.
- Preserve an explicit route to the old managed behavior where appropriate.
- Update tests and examples.

Do not silently change the lifetime semantics of a program.

If compatibility requires a temporary legacy mode, make it explicit and document its eventual removal conditions.

---

# 16. Documentation Deliverables

Update the following documentation where appropriate:

- `ABOUT.md`
- `SYNTAX.md`
- `ROADMAP.md`
- Relevant language reference documents.
- Relevant runtime documentation.
- Memory-model examples.
- Migration guide.

Create:

`docs/memory-model/overview.md`

`docs/memory-model/ownership.md`

`docs/memory-model/borrowing.md`

`docs/memory-model/arenas.md`

`docs/memory-model/shared-ownership.md`

`docs/memory-model/garbage-collection.md`

`docs/memory-model/interop.md`

`docs/memory-model/migration-guide.md`

The documentation must clearly answer:

- What happens by default when a value is created?
- Who owns an object?
- When is an object destroyed?
- How does ownership move?
- How does borrowing work?
- When should an arena be used?
- When should shared ownership be used?
- When should GC be used?
- How do owned and GC-managed objects interact?
- How does manual memory management work?

Use examples that compile with the actual implemented syntax.

Do not document hypothetical features as implemented.

---

# 17. Required Engineering Constraints

The following constraints are mandatory.

1. Preserve the existing compiler architecture unless a specific architectural limitation requires a change.
2. Do not replace working subsystems without evidence that replacement is necessary.
3. Do not remove or weaken GC correctness to improve owned-memory performance.
4. Do not introduce hidden GC dependencies into ordinary ownership.
5. Do not implement ownership as a superficial syntax layer.
6. Do not silently convert ownership strategies.
7. Do not rely on runtime checks where static enforcement is feasible.
8. Do not break FFI contracts.
9. Do not sacrifice exception safety.
10. Do not introduce data races or use-after-free behavior.
11. Do not optimize before establishing semantic correctness.
12. Do not duplicate LLVM optimization functionality unnecessarily.
13. Do not claim a phase is complete without tests and evidence.
14. Do not leave TODO-only implementations behind a successful test result.
15. Do not hide unsupported features behind misleading documentation.

Prefer small, reviewable changes with focused tests.

Keep the compiler buildable after each major phase.

---

# 18. Required Final Report

Produce:

`reports/memory-model/migration-completion-report.md`

The report must contain:

## A. Architectural changes

- Previous memory architecture.
- New memory architecture.
- How default allocation semantics changed.
- How ownership is represented.
- How borrowing is enforced.
- How deterministic destruction works.
- How arenas work.
- How shared ownership works.
- How selective GC works.

## B. Compiler changes

List modified components and explain their responsibilities.

Include relevant TIR, MLIR, and LLVM lowering changes.

## C. Runtime changes

Document the allocation paths and their separation.

Include GC isolation details.

## D. Compatibility changes

List source-level behavior changes and migration requirements.

## E. Test results

List:

- Tests added.
- Tests modified.
- Tests executed.
- Tests passed.
- Tests failed.
- Known limitations.

## F. Generated-code evidence

Provide representative evidence that owned-only code does not use unnecessary GC infrastructure.

Also provide evidence that GC-dependent code still receives the required instrumentation.

## G. Performance results

Include before/after comparisons for:

- Owned allocation.
- Owned destruction.
- Borrowed traversal.
- Arena allocation.
- Shared ownership.
- GC allocation.
- GC collection.
- Mixed workloads.

## H. Remaining work

List every incomplete feature, known correctness risk, and performance issue.

Do not label the migration complete if any mandatory acceptance gate remains unverified.

---

# 19. Definition of Done

This migration is complete only when all of the following are true:

- Deterministic ownership is the natural default for ordinary Nirqoba objects.
- Ordinary owned objects have predictable lifetimes and deterministic destruction.
- Borrowing is compiler-enforced and avoids unnecessary copies.
- Arenas and pools support grouped lifetimes without GC dependence.
- Shared ownership is explicit and correctly implemented.
- GC remains available for genuinely appropriate workloads.
- Manual memory management remains an explicit advanced capability.
- The compiler understands the different ownership strategies.
- The runtime provides separate, appropriate execution paths.
- Purely owned/borrowed code does not pay for GC infrastructure.
- Mixed memory strategies have defined, tested interoperability.
- Existing GC functionality remains correct.
- Tests, generated-code evidence, benchmarks, and documentation are complete.

**The most important acceptance criterion is architectural: Nirqoba must no longer treat garbage collection as the universal foundation of memory management.**

It must treat deterministic ownership as the default and GC as a powerful, selective tool.

---

# 20. Final Instruction to the Coding Agent

Treat this document as an architectural migration specification, not permission to immediately rewrite the entire memory subsystem.

First inspect the repository and establish the actual current implementation.

Then implement the migration incrementally, following the phases and acceptance gates.

If a requirement conflicts with an existing architectural decision, do not silently choose one. Document the conflict, explain the technical consequences, and propose the smallest correct resolution.

If a feature cannot safely be implemented within the current compiler architecture, identify the necessary architectural work before proceeding.

Preserve all existing working functionality unless a deliberate, tested migration replaces it.

Do not stop after introducing ownership metadata or changing the default allocator. The migration must reach the language semantics, compiler lowering, runtime behavior, and generated machine-code paths.

**Final architectural principle:**

> Ordinary code should be deterministic by default. Borrowing should be cheap. Shared ownership should be explicit. Arenas should handle grouped lifetimes. Garbage collection should solve the problems that genuinely need garbage collection—not become a tax on every program.

