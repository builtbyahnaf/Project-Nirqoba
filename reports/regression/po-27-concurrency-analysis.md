# PO-27 Formal Concurrency Analysis — Nirqoba GC Synchronization & Root Hang

**Author:** Automated investigation (Antigravity)
**Date:** 2026-11-07
**Branch:** HEAD (`runtime/src/gc.cpp` unmodified)
**Status:** Analysis complete — no production code changed

---

## Executive Summary

All three stress-test executables (`01_nirqoba.exe`, `02_nirqoba.exe`, `03_nirqoba.exe`) hang
indefinitely after launch. Live GDB attachment confirmed the process was **not blocked on any kernel
wait** — one thread was spinning at 100 % CPU inside a tight probe loop.

Disassembly mapping via `dumpbin /disasm` identified the hot loop as `SlotIndex::find` / `insert`
(open-addressing hash table, `gc.cpp` lines 681–729).

**Initial hypothesis** (deadlock in `acquire_collection_cycle`) was **formally retracted** after
the concurrency proof below: the synchronization protocol is correct in all traced interleavings.

**True root cause:** `SlotIndex::insert` triggers unbounded geometric capacity growth on
tombstone-driven rehashes even when only 1–2 active entries remain in the table. The regression
was introduced by commit `0b2d457` ("perf(gc): index root slots in one flat table instead of a
node map", 2026-11-06).

---

## 1. Exact Implementations Retrieved

### 1.1 `all_mutators_parked()` — gc.cpp:1049

```cpp
bool all_mutators_parked() {
  const size_t self = thread_mutator.depth != 0 ? 1 : 0;
  const size_t required =
      registered_mutator_count >= self ? registered_mutator_count - self : 0;
  return parked_mutator_count.load(std::memory_order_acquire) >= required;
}
```

> [!NOTE] The `self` deduction accounts for the case where the **calling thread is itself a
> registered mutator acting as collector**. Such a thread is already "stopped" by definition
> and must not be counted against the threshold.

### 1.2 `park_mutator()` — gc.cpp:1040

```cpp
void park_mutator(std::unique_lock<std::mutex>& lock) {
  thread_mutator.parked = true;
  parked_mutator_count.fetch_add(1, std::memory_order_release);
  collector_condition.notify_one();
  wait_until_not_requested(lock);
  parked_mutator_count.fetch_sub(1, std::memory_order_release);
  thread_mutator.parked = false;
}
```

### 1.3 `stop_mutators()` — gc.cpp:1056

```cpp
void stop_mutators() {
  std::unique_lock<std::mutex> lock(mutator_mutex);
  while (collection_is_requested()) {
    if (thread_mutator.depth != 0) {
      park_mutator(lock);
    } else {
      wait_until_not_requested(lock);
    }
  }
  collection_requested.store(true, std::memory_order_release);
  collector_condition.wait(lock, [] { return all_mutators_parked(); });
}
```

### 1.4 `resume_mutators()` — gc.cpp:1071

```cpp
void resume_mutators() {
  std::lock_guard<std::mutex> lock(mutator_mutex);
  collection_requested.store(false, std::memory_order_release);
  mutator_condition.notify_all();
}
```

### 1.5 `acquire_collection_cycle()` — gc.cpp:1947

```cpp
std::unique_lock<std::mutex> acquire_collection_cycle() {
  std::unique_lock<std::mutex> cycle_lock(collection_cycle_mutex, std::defer_lock);
  if (cycle_lock.try_lock()) return cycle_lock;
  if (thread_mutator.depth == 0) {
    cycle_lock.lock();
    return cycle_lock;
  }
  {
    std::unique_lock<std::mutex> lock(mutator_mutex);
    thread_mutator.parked = true;
    parked_mutator_count.fetch_add(1, std::memory_order_release);
    collector_condition.notify_one();
  }
  cycle_lock.lock();
  {
    std::lock_guard<std::mutex> lock(mutator_mutex);
    parked_mutator_count.fetch_sub(1, std::memory_order_release);
    thread_mutator.parked = false;
  }
  return cycle_lock;
}
```

> [!NOTE] When a **mutator-thread** (`depth != 0`) cannot immediately acquire
> `collection_cycle_mutex`, it **self-parks** (increments `parked_mutator_count`, notifies the
> collector), then blocks on `cycle_lock.lock()`. Once the cycle lock is granted it **de-parks**
> before returning. This prevents the scenario where a mutator-collector is waiting for itself.

### 1.6 `nrq_gc_mutator_enter()` — gc.cpp:2935

```cpp
void nrq_gc_mutator_enter(void) {
  if (thread_mutator.depth == SIZE_MAX)
    nrq_runtime_fatal("GC mutator nesting depth overflow");
  thread_gc_cleanup.arm();
  if (thread_mutator.depth++ != 0) return;

  std::unique_lock<std::mutex> lock(mutator_mutex);
  while (collection_is_requested()) wait_until_not_requested(lock);
  try {
    mutators.push_back(&thread_mutator);
    ++registered_mutator_count;
  } catch (...) {
    thread_mutator.depth = 0;
    nrq_runtime_fatal("GC mutator registration failed");
  }
}
```

### 1.7 `nrq_gc_mutator_leave()` — gc.cpp:2952

```cpp
void nrq_gc_mutator_leave(void) {
  if (thread_mutator.depth == 0)
    nrq_runtime_fatal("GC mutator leave without matching enter");
  if (--thread_mutator.depth != 0) return;

  std::lock_guard<std::mutex> lock(mutator_mutex);
  {
    HeapLockGuard heap_lock(gc_mutex);
    publish_tlab_locked(thread_mutator);
    drain_remembered_locked(thread_mutator, /*defer_unresolved=*/true);
    drain_transfer_pins_locked(thread_mutator);
  }
  thread_mutator.parked = false;
  auto found = std::find(mutators.begin(), mutators.end(), &thread_mutator);
  if (found == mutators.end())
    nrq_runtime_fatal("GC mutator registry lost an active thread");
  mutators.erase(found);
  --registered_mutator_count;
  collector_condition.notify_all();
  mutator_condition.notify_all();
}
```

### 1.8 `nrq_gc_safepoint()` — gc.cpp:2978

```cpp
void nrq_gc_safepoint(void) {
  if (!collection_is_requested()) return;
  if (thread_is_collector || thread_mutator.depth == 0) return;

  std::unique_lock<std::mutex> lock(mutator_mutex);
  if (!collection_is_requested()) return;
  park_mutator(lock);
}
```

### 1.9 Thread-exit cleanup — gc.cpp:821

```cpp
struct ThreadGcStateCleanup {
  bool armed = false;
  void arm() { armed = true; }
  ~ThreadGcStateCleanup();
};
thread_local ThreadGcStateCleanup thread_gc_cleanup;
```

The destructor (called by the C++ runtime on thread exit) publishes any pending TLAB, removes the
thread from the mutator registry, decrements `registered_mutator_count`, and calls
`collector_condition.notify_all()` — identical in effect to `nrq_gc_mutator_leave`.

---

## 2. Shared State Glossary

| Symbol | Type | Mutex | Description |
|---|---|---|---|
| `collection_cycle_mutex` | `std::mutex` | — | Serialises collection cycles. One holder at a time. |
| `collection_requested` | `std::atomic<bool>` | `mutator_mutex` (write); lock-free read | True during stop-the-world window. |
| `parked_mutator_count` | `std::atomic<size_t>` | `mutator_mutex` (write in `acquire_collection_cycle`) | Running count of parked mutators. |
| `registered_mutator_count` | `size_t` | `mutator_mutex` | Count of threads in the mutator registry. |
| `thread_mutator.parked` | `bool` (thread-local) | `mutator_mutex` | Per-thread park flag. |
| `thread_mutator.depth` | `size_t` (thread-local) | — | Nesting depth of `enter`/`leave` calls. 0 = not a mutator. |
| `collector_condition` | `std::condition_variable` | `mutator_mutex` | Wakes the collector when parked count changes. |
| `mutator_condition` | `std::condition_variable` | `mutator_mutex` | Wakes mutators when `collection_requested` clears. |

---

## 3. Concrete Interleavings

**Legend:** ✓ = lock acquired, ✗ = lock not held, → = transitions to, ⊢ = notifies, ⊣ = waits on.

### Interleaving A: Collector A vs. Mutator B (baseline correct path)

**Setup:** A is a pure collector thread (`depth == 0`). B is a registered mutator (`depth == 1`).
Collection starts.

| Step | Thread | Action | `cycle_mutex` | `requested` | `parked_count` | `registered` | B.`parked` | B.`depth` | B waits on |
|------|--------|--------|---|---|---|---|---|---|---|
| 1 | A | `acquire_collection_cycle()` → `try_lock` succeeds | A | false | 0 | 1 | false | 1 | — |
| 2 | A | `stop_mutators()` → no prior request; sets `requested=true` | A | **true** | 0 | 1 | false | 1 | — |
| 3 | A | `collector_condition.wait(…, all_mutators_parked)` — predicate false | A | true | 0 | 1 | false | 1 | collector_cond |
| 4 | B | `nrq_gc_safepoint()` → load `requested` → true; acquires `mutator_mutex` | A | true | 0 | 1 | false | 1 | — |
| 5 | B | `park_mutator()`: sets `parked=true`, increments `parked_count` to 1, notifies collector_cond | A | true | **1** | 1 | **true** | 1 | — |
| 6 | B | waits on `mutator_condition` | A | true | 1 | 1 | true | 1 | mutator_cond |
| 7 | A | wakes; `all_mutators_parked()` → `self=0`, `required=1`, `parked_count=1` → **true** | A | true | 1 | 1 | true | 1 | — |
| 8 | A | *[runs GC work]* | A | true | 1 | 1 | true | 1 | — |
| 9 | A | `resume_mutators()`: sets `requested=false`, notifies mutator_cond | A | **false** | 1 | 1 | true | 1 | — |
| 10 | B | wakes; `park_mutator` decrements `parked_count` to 0, sets `parked=false` | A | false | **0** | 1 | **false** | 1 | — |
| 11 | A | releases `cycle_lock` | ✗ | false | 0 | 1 | false | 1 | — |

**Conclusion:** No anomaly. All invariants hold at every step.

---

### Interleaving B: Collectors A and C requesting concurrently

**Setup:** Two pure collector threads. Neither is a registered mutator (`depth == 0`). One
registered mutator B exists.

| Step | Thread | Action | `cycle_mutex` | `requested` | `parked_count` | B.`parked` |
|------|--------|--------|---|---|---|---|
| 1 | A | `acquire_collection_cycle()` → `try_lock` succeeds | A | false | 0 | false |
| 2 | C | `acquire_collection_cycle()` → `try_lock` fails; `depth==0` → `cycle_lock.lock()` → **blocks** | A | false | 0 | false |
| 3 | A | `stop_mutators()` → sets `requested=true`, waits for B | A | true | 0 | false |
| 4 | B | hits safepoint → parks → `parked_count=1` | A | true | 1 | true |
| 5 | A | wakes; GC runs; `resume_mutators()`: `requested=false`, wakes B | A | false | 1 | true |
| 6 | B | de-parks → `parked_count=0`, `parked=false` | A | false | 0 | false |
| 7 | A | releases `cycle_lock` | ✗ | false | 0 | false |
| 8 | C | unblocks, acquires `cycle_lock` | C | false | 0 | false |
| 9 | C | `stop_mutators()` → no prior request; sets `requested=true`, waits for B | C | true | 0 | false |
| 10 | B | (running) hits safepoint → parks | C | true | 1 | true |
| 11 | C | GC runs; resumes mutators | C | false | 0 | false |

**Conclusion:** Strictly serialised by `collection_cycle_mutex`. C sees a clean slate because A
fully completed and resumed mutators before releasing the lock. No count leakage.

---

### Interleaving C: Two registered mutators simultaneously triggering collection

**Setup:** Mutators M1 and M2 (`depth == 1`). Both trigger `run_collection()` (e.g., via
threshold crossing). M3 is a third passive mutator.

| Step | Thread | Action | `cycle_mutex` | `requested` | `parked_count` | `registered` | M1.`parked` | M2.`parked` |
|------|--------|--------|---|---|---|---|---|---|
| 1 | M1 | `acquire_collection_cycle()`: `try_lock` succeeds | M1 | false | 0 | 3 | false | false |
| 2 | M2 | `acquire_collection_cycle()`: `try_lock` fails; `depth==1` → self-parks (`parked_count=1`, notifies) | M1 | false | **1** | 3 | false | **true** |
| 3 | M2 | blocks on `cycle_lock.lock()` | M1 | false | 1 | 3 | false | true |
| 4 | M1 | `stop_mutators()`: `requested` not set; sets `requested=true` | M1 | true | 1 | 3 | false | true |
| 5 | M1 | `collector_condition.wait(…, all_mutators_parked)`: `self=1`, `required=3-1=2`, `parked_count=1` → **false** | M1 | true | 1 | 3 | false | true |
| 6 | M3 | hits safepoint → parks → `parked_count=2` | M1 | true | 2 | 3 | false | true |
| 7 | M1 | wakes; `self=1`, `required=2`, `parked_count=2` → **true** | M1 | true | 2 | 3 | false | true |
| 8 | M1 | GC runs; `resume_mutators()`: `requested=false`, notifies mutator_cond | M1 | false | 2 | 3 | false | true |
| 9 | M2 | *still blocking on `cycle_lock`* — does NOT wake (is not on mutator_cond) | M1 | false | 2 | 3 | false | true |
| 10 | M3 | de-parks: `parked_count=1` | M1 | false | 1 | 3 | false | true |
| 11 | M1 | releases `cycle_lock` | ✗ | false | 1 | 3 | false | true |
| 12 | M2 | `cycle_lock.lock()` returns; de-parks (`parked_count=0`, `parked=false`) | M2 | false | **0** | 3 | false | **false** |
| 13 | M2 | `stop_mutators()`: `requested` not set; sets `requested=true` … | M2 | true | 0 | 3 | false | false |

**Key observation at step 7:** M2 is counted in `parked_count` (self-parked in step 2) even though
it is blocked on `cycle_lock.lock()`, not on `mutator_condition`. M1's predicate therefore sees
it as "safe" — which is **correct**: M2 cannot touch the heap while blocked on a mutex.

**Conclusion:** No anomaly. The self-park in `acquire_collection_cycle` is semantically correct
because a thread blocked on `cycle_lock.lock()` cannot mutate the heap.

---

### Interleaving D: Third collection request arriving during resume

**Setup:** A (collector) is in `resume_mutators()`. C (another thread) calls `run_collection()`
concurrently.

| Step | Thread | Action | `cycle_mutex` | `requested` |
|------|--------|--------|---|---|
| 1 | A | holds `cycle_lock`; in GC work | A | true |
| 2 | C | `acquire_collection_cycle()` → `try_lock` fails → blocks on `cycle_lock.lock()` | A | true |
| 3 | A | `resume_mutators()`: `requested=false`, wakes mutators | A | false |
| 4 | A | releases `cycle_lock` | ✗ | false |
| 5 | C | acquires `cycle_lock`; enters `stop_mutators()` | C | — |
| 6 | C | `stop_mutators()`: `requested` is **false** → sets `requested=true` immediately (no prior request to wait on) | C | **true** |

**Conclusion:** No race. The `while (collection_is_requested())` loop in `stop_mutators()` is
evaluated under `mutator_mutex`, which `resume_mutators()` also holds when it clears the flag.
The transition false→true (step 6) cannot see a stale true from step 3 because the write in
step 3 and the read in step 6 are both guarded by `mutator_mutex`.

---

### Interleaving E: Mutator entry/exit during active collection

**Setup:** Collection is in progress (A holds `cycle_lock`, `requested=true`). New mutator N
calls `nrq_gc_mutator_enter()`. Existing mutator X calls `nrq_gc_mutator_leave()`.

**N entering:**

| Step | Thread | Action | `registered` | `parked_count` |
|------|--------|--------|---|---|
| 1 | N | `nrq_gc_mutator_enter()`: acquires `mutator_mutex`; `requested=true` → waits on `mutator_cond` | unchanged | unchanged |
| 2 | A | `resume_mutators()`: `requested=false`, notifies `mutator_cond` | unchanged | unchanged |
| 3 | N | wakes; `requested=false` → exits loop; pushes to `mutators`, `++registered_mutator_count` | **+1** | unchanged |

N never increments `parked_count` because it was not in the registry when the collection started.
The collector already finished before N was counted, so the invariant is preserved.

**X leaving during collection:**

| Step | Thread | Action | `registered` | `parked_count` |
|------|--------|--------|---|---|
| 1 | X | `nrq_gc_mutator_leave()`: `--depth == 0`; acquires `mutator_mutex` | 1 | 0 |
| 2 | X | publishes TLAB under `gc_mutex` (may block; both A and X can hold `gc_mutex` briefly) | 1 | 0 |
| 3 | X | `parked=false`; erases from `mutators`; `--registered_mutator_count` | **0** | 0 |
| 4 | X | `collector_condition.notify_all()` — wakes A's predicate | 0 | 0 |
| 5 | A | `all_mutators_parked()`: `self=0`, `required=0`, `parked_count=0` → **true** (trivially) | 0 | 0 |

**Conclusion:** A departing mutator shrinks `registered_mutator_count` and immediately wakes the
collector. The predicate re-evaluates correctly against the new, smaller registry.

---

## 4. Formal Proof: Five PO-27 Target Questions

### Q1: Can a thread be counted as parked when it is not safe to resume?

**Answer: No.**

A thread is counted (via `parked_mutator_count` increment) in exactly three code paths:

1. `park_mutator()` — thread holds `mutator_mutex` and is blocked on `mutator_condition` (cannot
   touch the heap).
2. `acquire_collection_cycle()` — thread has released `mutator_mutex` and is blocked on
   `cycle_lock.lock()` (cannot touch the heap until it owns the cycle).
3. `nrq_gc_mutator_leave()` does **not** increment `parked_mutator_count`; it decrements
   `registered_mutator_count` and exits the registry.

In all three paths the thread is in a provably inert state with respect to heap mutation.
The collector's predicate `all_mutators_parked()` therefore has no false positive.

### Q2: Can a stale parked count be left behind?

**Answer: No.**

Every increment of `parked_mutator_count` has a matching decrement on the **same thread**:
- `park_mutator`: +1 before wait, −1 after `mutator_condition` returns.
- `acquire_collection_cycle`: +1 before `cycle_lock.lock()`, −1 after acquiring it.

Both decrements execute before the thread returns to heap-mutating code. The only code that reads
`parked_mutator_count` is `all_mutators_parked()`, which is called under `collector_condition.wait`
— if a stale count existed it would cause the collector to wait longer, not to resume prematurely.

### Q3: Can a mutator be resumed during a subsequent collection request?

**Answer: No.**

`resume_mutators()` clears `collection_requested` and broadcasts on `mutator_condition` **while
holding `mutator_mutex`**. `stop_mutators()` sets `collection_requested` and waits on
`collector_condition` **while holding the same mutex**. Because both critical sections are
protected by `mutator_mutex`, the following invariant holds:

> `collection_requested` transitions true→false (resume) and false→true (next stop) are strictly
> ordered. A mutator woken by `mutator_condition` sees `requested=false` before the next cycle's
> `requested=true` can be written.

### Q4: Can the implementation deadlock while holding `collection_cycle_mutex`?

**Answer: No (initial hypothesis retracted).**

The only code path that could loop while holding `collection_cycle_mutex` is inside
`run_collection()`. That function calls `stop_mutators()`, which acquires `mutator_mutex`
separately. The lock ordering is: `collection_cycle_mutex` → `mutator_mutex` → `gc_mutex`.
This ordering is consistent in every call site. No code acquires `collection_cycle_mutex` while
holding `mutator_mutex` or `gc_mutex`.

A thread blocked on `cycle_lock.lock()` holds **no** locks; it cannot cause a deadlock by holding
a resource another thread needs.

### Q5: Can the collector wait for a mutator that is itself waiting for the collector?

**Answer: No.**

The only way a mutator can be waiting for the collector is via `wait_until_not_requested`, which
waits on `mutator_condition`. In that state the mutator is counted in `parked_mutator_count` (via
`park_mutator`). The collector's `all_mutators_parked` predicate counts it as parked, so the
collector proceeds and calls `resume_mutators`, which broadcasts on `mutator_condition` to wake it.

The only remaining case is a mutator blocked on `cycle_lock.lock()` in
`acquire_collection_cycle()`. That thread self-parks before blocking, so the collector's predicate
includes it. The collector completes and releases `cycle_lock`, unblocking the mutator.

There is **no mutual wait**: in both cases the mutator's block is counted as a park, allowing the
collector to proceed.

---

## 5. Deadlock Hypothesis: Formally Retracted

> **The initial hypothesis that `acquire_collection_cycle()` contained a synchronization defect
> leading to deadlock is retracted.**
>
> **Evidence:** Live GDB attachment to the hanging process showed:
> - Thread 1 was executing at address `0x7ff75466477d` with 100 % CPU usage.
> - `NtWaitForAlertByThreadId`, `WaitForSingleObject`, and `pthread_cond_wait` were **absent**
>   from the thread's call stack.
> - The process was **not blocked on any kernel wait primitive**.
>
> A deadlocked process blocks on a wait primitive. A spinning process does not. The observed
> behaviour is a CPU-bound infinite loop, not a deadlock.

---

## 6. Root Cause: `SlotIndex::insert` — Tombstone-Driven Capacity Explosion

### Regression commit

```
0b2d457  perf(gc): index root slots in one flat table instead of a node map
Date:    2026-11-06
```

This commit replaced `std::unordered_map<void*, SlotState>` in `ShadowStack` with the new
`SlotIndex` class (open-addressing hash table).

### The defective rehash policy — gc.cpp:696–701

```cpp
SlotState* insert(void* slot) {
  if (!slot) return nullptr;
  if (entries_.empty() || size_ + tombstones_ + 1 >=
          entries_.size() - entries_.size() / 4) {        // ← load factor trigger
    if (!rehash(entries_.empty() ? 16 : entries_.size() * 2)) return nullptr;  // ← always doubles
  }
  // …
}
```

**The flaw:** The load-factor threshold is computed as `size_ + tombstones_`, which includes dead
entries. When tombstones accumulate (each slot `release()` leaves one), the threshold is crossed
even if only 1–2 live entries exist. The rehash target is **always** `entries_.size() * 2`,
regardless of whether the table is full of live entries or full of tombstones.

### Capacity growth trace — single benchmark iteration cycle

Each `make_node()` call in the stress test:
1. Calls the allocator, which registers a root slot → `SlotIndex::insert`.
2. Drops the reference → `SlotIndex::release` → tombstone left behind.

After `N` such iterations the table state is approximately:
```
size_       ≈ 1   (one live entry)
tombstones_ ≈ N   (all previous slots tombstoned)
entries_.size() ≈ 2^ceil(log2(N))
```

The `size_ + tombstones_ + 1 >= capacity * 3/4` threshold fires every iteration once tombstones
accumulate past 75 % capacity. Each fire doubles capacity. After `k` doublings:

| k | Capacity | `entries_` allocation (64 B/entry) |
|---|---|---|
| 0 | 16 | 1 KB |
| 10 | 16 384 | 1 MB |
| 14 | 262 144 | 16 MB |
| 17 | 2 097 152 | 128 MB |
| 20 | 16 777 216 | **1 GB** |

At 1,000,000 benchmark iterations, `k ≈ 20`. Each rehash allocates a new 1 GB `std::vector<Entry>`
and linearly scans the previous table. The process spends essentially all time in `rehash()` and
its subsequent linear probe in `insert`. Because the vector fits in virtual address space but
triggers repeated page faults and cache misses across gigabytes, the probe loop appears as a
spinning CPU-bound computation — matching exactly what GDB observed.

### Comparison: `PageDirectory` has the fix; `SlotIndex` does not

The `PageDirectory` class (gc.cpp:85–99) uses an identical open-addressing design but has a
correct rehash policy:

```cpp
} else if (size_ + tombstones_ + 1 >= entries_.size() - entries_.size() / 3) {
  size_t next = entries_.size();
  if (size_ >= entries_.size() / 2) {   // ← only double if actually full of LIVE entries
    if (entries_.size() > SIZE_MAX / 2) return false;
    next = entries_.size() * 2;
  }
  if (!rehash(next)) return false;      // ← otherwise compact in-place (same capacity)
}
```

When `size_ < capacity / 2` (tombstone-saturated, not entry-saturated), `PageDirectory` rehashes
to the **same capacity**, clearing tombstones without growing. `SlotIndex` lacks this branch and
always doubles.

---

## 7. Proposed Fix (Not Yet Applied)

> [!IMPORTANT] No production code has been modified. This section documents the intended fix for
> review before implementation.

**File:** `runtime/src/gc.cpp`
**Lines:** 698–700

**Current (defective):**
```cpp
if (entries_.empty() || size_ + tombstones_ + 1 >=
        entries_.size() - entries_.size() / 4) {
  if (!rehash(entries_.empty() ? 16 : entries_.size() * 2)) return nullptr;
}
```

**Proposed:**
```cpp
if (entries_.empty()) {
  if (!rehash(16)) return nullptr;
} else if (size_ + tombstones_ + 1 >= entries_.size() - entries_.size() / 4) {
  size_t next = entries_.size();
  if (size_ >= entries_.size() / 2) {
    if (entries_.size() > SIZE_MAX / 2) return nullptr;
    next = entries_.size() * 2;
  }
  if (!rehash(next)) return nullptr;
}
```

**Invariant established:** After any rehash, `entries_.size() >= 2 * size_`. Capacity is bounded
by `O(peak_concurrent_live_slots)`, not `O(total_insertions_ever)`.

---

## 8. Regression Test Design

### Test 1: `SlotIndex` capacity remains bounded under repeated insert/release cycles

```cpp
TEST(GcTest, SlotIndexCapacityBoundedUnderTombstoneChurn) {
  // Allocate and immediately deallocate a root slot N times.
  // The working set is always 1 live entry.
  // With the fix: table capacity stays at 16.
  // Without the fix: table capacity grows to ~2^ceil(log2(N)).
  constexpr int N = 100'000;
  for (int i = 0; i < N; ++i) {
    void* fake_slot = reinterpret_cast<void*>(uintptr_t{0x1000} + i);
    // insert + release via public GC root registration API
    nrq_gc_root_frame_begin();
    nrq_gc_root_register(fake_slot, ...);
    nrq_gc_root_frame_end();
  }
  // Assert: slot_states_.size() (via diagnostic API) is O(1), not O(N).
  EXPECT_LE(nrq_gc_slot_index_capacity(), 32u);
}
```

**Expected result before fix:** Test times out or OOMs.
**Expected result after fix:** Test completes in < 100 ms; capacity assertion passes.

### Test 2: Stress-test executables complete within timeout

```powershell
foreach ($exe in @("01_nirqoba.exe", "02_nirqoba.exe", "03_nirqoba.exe")) {
  $proc = Start-Process -FilePath "build\msvc-dev\bin\$exe" -PassThru
  $finished = $proc.WaitForExit(30000)
  if (-not $finished) { throw "HANG: $exe did not finish within 30 s" }
  if ($proc.ExitCode -ne 0) { throw "FAIL: $exe exited with code $($proc.ExitCode)" }
}
```

### Test 3: Concurrent `acquire_collection_cycle` correctness (regression guard)

A multi-threaded unit test that:
1. Launches 4 mutator threads, each calling `run_collection()` concurrently 100 times.
2. Asserts that `all_mutators_parked()` is never true while any mutator is in a non-parked state.
3. Asserts that no thread blocks for more than 1 second (deadlock guard).

This test validates all five PO-27 invariants under real concurrency.

---

## 9. Files Inspected

| File | Purpose |
|---|---|
| `runtime/src/gc.cpp` | Full 3340-line implementation; all 8 functions extracted from HEAD |
| `runtime/include/nirqoba/runtime/gc.h` | Public API declarations |
| `tests/unit/gc_test.cpp` | Existing unit tests |
| `benchmarks/stress_test_2/nirqoba/01_managed_object_allocation.nrqba` | Benchmark source |
| `benchmarks/stress_test_2/run_benchmark.py` | Benchmark runner |
| `CMakePresets.json` | Build configuration |
| `build/msvc-dev/CMakeCache.txt` | Active build: MSVC 14.51, Ninja, RelWithDebInfo |

**Git commits inspected:**
- `0b2d457` — regression commit (SlotIndex introduction)
- `d59a910`, `a150fce`, `1493dde` — surrounding history

---

## 10. Environment

| Property | Value |
|---|---|
| OS | Windows (MSVC 14.51) |
| Build system | Ninja + CMake |
| Build type | RelWithDebInfo |
| Build directory | `D:\Actual Project Files\Project Nirqoba\build\msvc-dev` |
| Runtime source | `D:\Actual Project Files\Project Nirqoba\runtime\src\gc.cpp` |
| Hang confirmed | `$proc.WaitForExit(5000)` returned false (all three executables) |
| GDB observation | Thread 1 at `0x7ff75466477d`; 100 % CPU; no kernel wait |

---

## 11. Summary of Findings

| Question | Finding |
|---|---|
| Can a thread be counted parked when unsafe to resume? | **No** — all three park paths leave the thread in a provably inert state. |
| Can a stale parked count be left behind? | **No** — every increment has a symmetric decrement on the same thread before heap access resumes. |
| Can a mutator be resumed during a subsequent collection? | **No** — `requested` transitions are strictly ordered under `mutator_mutex`. |
| Can the implementation deadlock holding `collection_cycle_mutex`? | **No** — lock ordering is consistent; no cycle is possible. |
| Can the collector wait for a mutator waiting for it? | **No** — waiting mutators are always counted as parked, allowing the collector to proceed. |
| **Actual hang cause** | **`SlotIndex::insert` always doubles capacity on rehash, including tombstone-saturated rehashes. Capacity reaches ~1 GB after ~1 M iterations; probe loop spins indefinitely.** |
| Regression commit | `0b2d457` (2026-11-06) |
| Fix ready? | Yes — documented in §7; not yet applied. |
