# Pre-MMM Phase 7, Part 4 — Container and aggregate internals

**Version:** `0.499-beta`  
**Date:** `2026-11-15`  
**Baseline:** `0.495-beta` (`bb99ffd`)  
**Scope:** owned arrays, mutable and immutable array views, structure-of-arrays
containers, shared storage, aggregate destruction and aggregate shared-field
retention.

## 1. Verdict

The container audit is complete. Two real ownership gaps were found and fixed:

1. A `soa<T>` created as an aggregate field did not force a root frame, so the
   rooted allocation aborted before the aggregate existed.
2. Aggregate destruction did not visit SoA fields, leaving their container roots
   published until the activation frame ended.

The aggregate traversal was also moved off a per-site `std::function`; this is
code-quality work and is not claimed as a speedup. No container representation,
borrow rule, ownership rule or collector policy was redesigned.

Roadmap step 4 is complete. Phase 7 is complete for the subsystems present in
this tree. The deferred hardening items in section 7 are not silently treated as
solved.

## 2. Audit

| Surface | Representation | Ownership | Lifetime | Result |
|---|---|---|---|---|
| `owned_array<T>` | `{data, length}` descriptor over explicit storage | binding or owning aggregate field | deterministic release, or stack placement | Correct; no migration needed |
| `array_view<T>` | borrowed `{data, length}` descriptor | none | source binding, borrow-checked | Correct; no heap lifetime |
| `array_view_mut<T>` | borrowed `{data, length}` descriptor | none | exclusive mutable borrow | Correct; no heap lifetime |
| `soa<T>` | one collector object containing every column | container value owns the root | collector sweep after root release | Rooting and aggregate release fixed |
| `shared<T>` | reference-counted explicit allocation | shared reference count | last release | Correct; aggregate retain/release already covers it |
| aggregate fields | physical field order, declaration-order destruction | whole-aggregate move; shared fields retain on copy | scope/frame cleanup | SoA leaf added to destruction |

### Owned arrays

`memory.owned_array<T>` allocates through
`nrq_memory_allocate_array[_from]`, rejects zero-sized elements and
`count > SIZE_MAX / element_size`, and stores `{data, length}`. The lowering
registers a typed root only when the element type contains a managed reference
and removes that root before `nrq_memory_deallocate_checked`. Stack-placed arrays
unregister without deallocating frame storage. Elements that own nested arrays or
views are rejected by Sema, so destruction cannot recurse through a descriptor
that the runtime does not model.

No gap was found. Counting, bounds checks and release ordering are covered by the
existing owned-array and stack-placement suites.

### Array views

Views are two-word stack descriptors. Construction checks the source range with
`nrq_memory_array_range_check`; Sema tracks the source allocation and borrow
mode, and rejects escapes through returns, aggregate storage and destructuring.
There is no heap allocation or cleanup action to migrate.

### Structure of arrays

`describe_soa` is the single source of column geometry. The payload is one
collector object, and reference columns are described by `(offset, stride)`
pairs. The runtime interns those pairs and walks every row of every managed
column during tracing, so a reference in any column keeps its target alive.

`soa.owned_release` unregisters the container root only. That is intentional:
the collector owns the payload and reclaims it in a later sweep. The release is
not a deterministic free, and Part 4 does not pretend otherwise. Making it
deterministic would require a per-object free path in the collector or a second
allocation model for SoA, neither of which is justified by the current contract.

### Shared storage

Shared allocations keep a reference count and optional root metadata. Aggregate
copy retains every shared field; aggregate destruction releases it in reverse
declaration order. `SharedRootMetadata` stores up to eight offsets inline and
falls back to a vector for deeper layouts. No container-specific gap was found.

## 3. Gap 1 — SoA aggregate initializers had no root frame

Reproduction:

```nrqba
struct Point { int x; int y; }
struct Holder { soa<Point> rows; int tag; }

int function main() {
    var holder = Holder { rows: memory.owned_soa<Point>(2), tag: 1 };
    print(holder.rows[0].x);
    return 0;
}
```

Before the fix this aborted with:

```
rooted GC allocation requires a registered pointer root slot
```

`analyze_gc_root_plan` marked an `array.allocate` result as a rooted allocation
but not a `memory.owned_soa` result. When the SoA value was consumed immediately
as an aggregate field, liveness found no later collection point and omitted the
root frame. The lowering still emitted a rooted allocation with an entry slot
that had never been registered, so the runtime correctly rejected it.

The root-plan analysis now treats `memory.owned_soa` like the other rooted heap
container. The entry frame is emitted before the allocation, the slot is
registered, and the object is published through it. The fix changes no container
layout and no runtime ABI.

## 4. Gap 2 — aggregate destruction skipped SoA fields

`aggregate.owned_drop` recursed through arrays, tuples, structs, owned arrays,
owned pointers and shared pointers, but its field predicate omitted `soa<T>`.
An aggregate containing a SoA field therefore never unregistered that field's
root. The root frame eventually closed at function return, so this was not an
unbounded leak, but it kept the collector object alive for the rest of the
activation and made destruction incomplete.

The drop walk now treats a SoA field as a leaf: it extracts the container
pointer and calls `nrq_gc_root_unregister`. It does not deallocate the collector
payload. The regression case `aggregate_soa_field_reclaimed` samples live
objects while the aggregate exists and then proves a collection reduces the
count after the aggregate goes out of scope.

## 5. Aggregate traversal

Part 3 introduced `SmallVector<int64_t, 8>` for the field path but kept a
recursive `std::function` at each `aggregate.owned_drop` and
`aggregate.shared_retain` site. Part 4 replaces those callables with private
member helpers carrying `llvm::SmallVectorImpl<int64_t>&`. Destruction and
retention now recurse directly with no per-operation callable object.

The measured effect is neutral (section 6). The change is retained because it
removes a hidden allocation and type-erased indirection from the lowering path,
not because it produced a speedup.

## 6. Benchmark

`benchmarks/pre_mmm_phase7_part4/` generates 20 functions with 250 independent
aggregate lifetimes each (5,000 drop sites, eight owned-array fields per
aggregate). The generated executable is not run; this isolates compiler lowering
of `aggregate.owned_drop`. Baseline `0.495-beta` (`bb99ffd`) and current
`0.499-beta` were built in `RelWithDebInfo` and alternated over 15 repeats.

| Measurement | Baseline | Part 4 | Change |
|---|---:|---:|---:|
| Compiler wall | 7,454.763 ms | 7,507.598 ms | **+0.71%** |
| Compiler peak RSS | 417,304,576 B | 417,480,704 B | +0.04% |
| Object size | 1,362,995 B | 1,362,995 B | identical |
| Executable size | 622,592 B | 622,592 B | identical |

The wall difference is within the observed spread and is not claimed as either a
regression or an improvement. The correctness fixes cannot be benchmarked on the
baseline because the baseline aborts on the SoA aggregate cases.

## 7. Verification

| Check | Result |
|---|---|
| RelWithDebInfo build | PASS |
| Compiler version | `0.499-beta (Phase 7)` |
| Unit suite | 258/258 |
| Integration suite | 470/470 |
| SoA field rooted and traced | PASS |
| SoA field root released on aggregate drop | PASS |
| SoA field reached through a tuple | PASS |
| SoA field moved and reinitialized | PASS |
| Existing owned-array, view, shared and cleanup suites | PASS |

No ASan/UBSan run is available in the MSVC configuration. No dedicated
container stress harness exists; the existing GC and cleanup suites are the
available stress coverage. The Phase 4 GC stress flake remains separate.

## 8. Deferred limitations

These are tracked for later hardening and were not compromised to force a
container change:

1. Publication has no crash journal; a crash between object and executable
   renames can leave a partial pair and backups.
2. `file.read_text` buffers remain process-lifetime because the language has no
   owned-string release point.
3. `system.exit` abandons busy registries to OS teardown instead of waiting.
4. SoA storage is collector-owned. `soa.owned_release` unregisters the root but
   does not free the object synchronously.
5. Sockets are deliberately unlinked; async/task and tensor/device subsystems do
   not exist in this tree.
6. No sanitizer run is available with the MSVC toolchain.

## 9. Phase 7 verdict

Phase 7 is **complete** for roadmap steps 1–4. Steps 5 and 6 have no subsystem to
migrate. The remaining items above are documented hardening work, not unfinished
Phase 7 migrations; they do not involve a container or aggregate ownership path
that Part 4 left unmigrated.