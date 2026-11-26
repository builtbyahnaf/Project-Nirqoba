## Nirqoba Version 2: The CDMM Expansion Architecture Plan

### 1. The Concurrency Frontier (Thread-Safe CDMM)

* [ ] **Thread-Local Allocation Buffers (TLABs):** Implement lock-free thread-local caches for Pools and Arenas to eliminate global allocator lock contention during rapid temporary instantiations.
* [ ] **Cross-Thread Ownership Static Analysis:** Design and implement thread-safety rules (analogous to Rust's `Send`/`Sync` bounds) to mathematically prove Arena pointers or cyclic graphs are not mutated concurrently across thread boundaries.
* [ ] **Concurrent Garbage Collection:** Upgrade the cyclic fallback tier from a stop-the-world collector to a concurrent mark-and-sweep, ensuring the GC never stalls threads executing deterministic Arena/Pool workloads.

### 2. Closure and Lambda Captures

* [ ] **Closure Escape Analysis:** Extend the existing `promote_returned_local` boundaries to analyze variable captures passed into event loops, callbacks, or asynchronous threads.
* [ ] **Implicit Environment Promotion (Closure Conversion):** Automatically rewrite captured local variables into heap-allocated environments when a closure outlives its lexical scope, applying the CDMM Hybrid Selector to route the environment block.

### 3. Heap Fragmentation and Compaction

* [ ] **Generational / Compacting Collector:** Introduce a compaction phase for the cyclic fallback tier to eliminate GC heap fragmentation (the "Swiss cheese" effect) in long-running server workloads.
* [ ] **Address Reconciliation:** Engineer a safe pointer-fixup mechanism to update memory addresses when the compacting collector moves objects, carefully isolating these shifting addresses from Nirqoba's raw C-style pointer semantics.

### 4. Zero-Copy FFI Memory Models

* [ ] **Foreign Memory Borrowing:** Establish syntactically invisible boundary rules for safely mapping massive unowned buffers (e.g., C-allocated arrays or C++ engine arenas) into Nirqoba's memory space.
* [ ] **Destructor Isolation:** Implement strict CDMM teardown barriers to guarantee that `nrq_cdmm_arena_destroy` or cyclic GC sweeps never attempt to free memory addresses owned by foreign `malloc` or external allocators.

Locking this blueprint into the Version 2 roadmap clears the runway for the immediate objective. We can now focus entirely on finishing the basic feature integrations and shipping Version 1.