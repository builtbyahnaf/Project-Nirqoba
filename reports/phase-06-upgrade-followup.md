# Phase 6 Upgrade follow-up

Date: 2026-11-01

## Ownership and contiguous storage

The explicit ownership path now supports unique pointer and owned-array moves,
owned and shared parameter/return contracts, checked borrowed pointer parameters,
read-only array views, and compiler-inserted deterministic cleanup for tracked
owners and supported resources. Resource-bearing structs, tuples, and fixed
arrays transfer as whole values and recursively drop nested owned arrays.
Partial field extraction, replacement, and destructuring are diagnosed so a
nested allocation cannot acquire two independent cleanup paths. `owned_array<T>` stores a contiguous element
buffer and length. `array_view<T>` is a non-owning descriptor that supports
checked nested slicing and function parameters. Views cannot escape through
returns or be written through. Their lexical borrow blocks owner moves, release,
and mutation while the view binding is live.

Ownership state is checked across both `if` branches and conservatively merged;
loop back-edges reject owners left moved. Calls track temporary and named view
borrows against owner transfers and reject duplicate transfers of the same token.
Views cannot be nested into aggregates, returned through aggregate types, or
stored in aggregate/allocated memory. Resource-bearing aggregates move whole;
raw pointer fields with owned, shared, or borrowed ownership are rejected until
field ownership can be declared.

Validation on Windows x64, MSVC, and LLVM/MLIR 23.1.4: RelWithDebInfo build,
CTest 1/1, and integration 83/83 passed after aggregate, branch/loop, and
call-borrow changes. Added coverage exercises nested aggregate return/parameter
cleanup, both branches transferring an owner, possibly-moved branch rejection,
loop back-edge and condition rejection, cleanup on a return path, duplicate and conflicting call arguments, borrowed
view/pointer field restrictions, nested owned-array type rejection, and GC
tracing through an aligned tuple nested in an owned array.

An additional owner-scope regression now checks compiler-generated cleanup on
both `continue` and `break` loop exits; the memory profiler remains at zero live
bytes after the helper returns. The full validation suite passed again: CTest
1/1 and integration 83/83.

## GC safety state

GC remains an opt-in, precise, non-moving mark-and-sweep domain. Allocation
headers and payloads share one allocation. Root descriptors and registration
counts are validated, metadata allocation failures fail safely, and diagnostic
counters saturate instead of wrapping. Pinned references are included in the
optional reference diagnostics. Ordinary correctness tests exercise cycles,
roots, pins, typed arrays, concurrent allocation, allocation pressure, and
reclamation; the unit suite no longer prints timing measurements.

The collector still serializes allocation, root mutation, pinning, and collection
with one mutex. It is not concurrent with mutator pointer writes unless those
writes use the root-store API. TLABs, safepoints, generational or incremental
collection, and concurrent mutation remain unimplemented. Small managed objects
now use aligned 64-KiB bump regions under the collector lock; large objects get
dedicated regions. Sweeping destroys per-object trace metadata and returns a
region only after its live-object count reaches zero. These regions reduce
backing allocator calls but do not reduce global-lock synchronization. TLABs
and concurrent collection need a mutator safepoint protocol before they can be
added safely. A threaded correctness test races root-store
mutation against explicit collection and checks reachability and reclamation;
this does not establish safety for raw uninstrumented writes. No GC benchmark
was run for this follow-up. Root unregistration must match a registration on
the current thread, and generated root frames are checked for valid LIFO
closure.

Root frames now use unique runtime tokens paired with their saved root depth.
This prevents nested empty frames or stale markers from aliasing the same root
count. Unit coverage checks nested frames created at identical depths, and the
full suite passed: CTest 1/1 and integration 84/84. No GC benchmark was run.

Semantic cleanup checks now preserve allocator/resource domains: files cannot
be closed through a generic allocation destructor, generic allocations cannot
be unmapped as files, and unpin accepts only GC-managed pointers. Secure release
accepts explicit allocations or secure buffers; file reads/writes and mutex
operations require their matching owner or a borrowed handle; mapping size
requires a mapped file. Regression coverage includes cross-domain close,
deallocate, secure release, unmap, mutex, mapping-size, and unpin calls.
RelWithDebInfo validation passed: CTest 1/1 and integration 85/85. No GC
benchmark was run.

Managed stack roots are now zeroed and registered in the function prologue,
before any managed allocation or explicit collection. Managed parameter values
are copied into registered slots through the synchronized root-copy API. This
fixes a lowering-order bug where local registrations could appear after frame
closure. The managed-graph regression now enables collection at a one-byte
threshold, forcing collection between its two object allocations. It passed,
and the same graph executable passed 500 consecutive executions. The final
shared-reference release now removes the shared token and
deallocates under one registry lock, so a reset or unique deallocator cannot
take ownership in between. A concurrent retain/release unit test passed.
RelWithDebInfo validation: CTest 1/1 and integration 85/85. No GC benchmark was
run.

Allocator reset now fails while any allocation remains live, preserving owner
validity instead of silently invalidating pointers. A runtime regression checks
that a live arena allocation remains readable after a rejected reset and that
reset succeeds once the owner is released. The full suite passed again: CTest
1/1 and integration 85/85. No GC benchmark was run.

Reallocation now reuses its existing ownership-registry node when updating the
address, so a successful allocator resize cannot lose tracking due to a second
registry-node allocation. A failing custom allocator regression verifies that
the original pointer and owner record remain usable. Full validation passed:
CTest 1/1 and integration 85/85. No GC benchmark was run.

Generic deallocation releases the global registry lock while the per-allocator
deallocator runs, with a busy owner record preventing double release, resize,
or reset races and a condition variable guarding address reuse. A blocking
callback regression confirms unrelated allocator operations remain available.
Full validation passed: CTest 1/1 and integration 85/85. No GC benchmark was
run.

The allocator now checks stack LIFO eligibility and performs deallocation under
one per-allocator operation lock for generic release, secure release, final
shared release, and zero-byte reallocation. This closes the gap where another
allocation could change stack order between a separate eligibility check and
the actual free.

The tuple-layout regression now passes a struct containing nested managed
references into a function that collects before reading it, covering the
aggregate parameter root-copy path. RelWithDebInfo validation passed: CTest
1/1 and integration 85/85. No GC benchmark was run.

Owned arrays whose element types contain GC-managed references register their
contiguous backing store as a repeated-layout GC root region. Array cleanup
unregisters that region before releasing the buffer. A unit test verifies the
runtime repeated-offset API, and an integration test verifies an owned array
keeps a managed object alive after its last local pointer is overwritten and
collection runs.

The collector's exact-address object index is now a contiguous open-addressed
table rather than a node-based `unordered_map`. It grows during serialized
allocation, uses tombstones during sweep, and performs lookup without per-object
hash-node allocation. The existing allocation-pressure and cycle/root tests
exercise growth, lookup, sweep, and subsequent reuse; no timings were collected.

## Phase 6.9 native resources

File resources and native mutex handles now have explicit close/destroy behavior
and deterministic scope cleanup. Mutexes reject recursive acquisition,
wrong-thread unlock, and destruction while locked. `defer` runs in LIFO order on
normal scope exit, return, break, and continue. Exception/error unwinding is not
implemented because the language has no unwind model. The runtime remains
Windows-only. Scheduler task and device-memory introspection remain unavailable
because those runtime subsystems do not yet exist.

## Remaining roadmap work

- Ownership is lexical and conservative. Partial aggregate moves, closure
  capture, general returned references, and arbitrary interprocedural lifetime
  inference are not supported. Views are deliberately not storable in
  aggregates.
- Array constructors use the selected allocator and natural element alignment;
  allocator-specific containers, explicit SIMD alignment, mutable views, and
  tensor/GPU buffer types remain open.
- The explicit allocator still uses global registry/profile synchronization and
  live-allocation bookkeeping. Generic allocate, deallocate, and reallocate
  callbacks run outside the registry lock; secure release and final shared
  release remain serialized under it. Thread-local fast paths are not
  implemented.
- Specialized pointer resources such as files, mutexes, secure buffers, and
  mappings cannot yet cross function returns because pointer signatures do not
  preserve their cleanup function. The checker rejects those returns rather
  than letting the caller infer `memory.deallocate`.
- The C++/Rust benchmark matrix and runner exist, but comparative runs require
  a Rust toolchain and controlled release configuration. No comparative
  performance claim is made. Phase 6U:G remains a later measurement task; GC
  benchmarking is intentionally deferred.

The benchmark harness now separates 16-byte allocation, 4-KiB allocation, and
alternating mixed-size allocation workloads, in addition to its 64-byte,
scalar, contiguous-array, graph, and four-thread workloads. The C++/Rust runner
checks checksums for each workload and records repeated percentile samples and
environment metadata. This runner was not executed in this follow-up because
the current environment has neither `cl.exe` in the active shell nor
`rustc.exe`; no GC workload was run.

## Explicit allocator synchronization

Stateful operations now use a per-allocator mutex. Allocation selects and pins
the allocator under the registry lock, performs allocator work outside that
global lock, then records ownership and profiler data under the registry lock.
The temporary pin prevents reset or destruction while a callback or allocator
operation is in flight. A blocking custom-allocator regression test verifies
that an explicit allocation prevents concurrent destruction until registration
completes. Different allocator instances can perform allocation work in
parallel; calls through the same allocator remain serialized.

The live-allocation registry, shared reference counts, selection state, and
profiler snapshot still use global synchronization. Generic checked deallocation
marks the owner record and pins the allocator, invokes the allocator outside
the registry lock, then removes the record under the lock. Reallocation uses an
in-flight owner marker and keeps its existing registry node while resizing
outside the lock. Same-allocator operations remain serialized, and address reuse
waits until the old owner operation completes. Secure release and final shared
release still invoke allocator operations while holding the registry lock.
Blocking custom deallocator and reallocator tests verify unrelated allocator
creation proceeds during each callback. The existing allocator benchmark matrix
remains the measurement path; no GC benchmark was run.

Follow-up correctness fixes keep null-pointer reallocation outside the registry
lock and pin the selected allocator until its allocation has been recorded,
avoiding a reentrant-lock deadlock. Allocator reset now rejects an in-flight
allocation callback so region storage cannot be cleared before its returned
pointer is registered. Regressions cover null-pointer reallocation and a
blocking custom allocator racing reset. Full validation passed: CTest 1/1 and
integration 83/83. No GC benchmark was run.

The ownership checker also rejects returns of resources with specialized
destructors until function signatures can carry that cleanup contract. Direct
file-open and named mutex-resource returns are covered. Full validation passed:
CTest 1/1 and integration 84/84. No GC benchmark was run.

Reallocation lock-scope coverage and the full suite passed: CTest 1/1 and
integration 85/85. No GC benchmark was run.
