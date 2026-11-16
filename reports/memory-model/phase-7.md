# Phase 7 — GC Isolation and Explicit Selection

**Status:** Complete  
**Starting version:** `0.571-beta` (Phase 6 completion, expected `3e01af1`)  
**Actual starting HEAD:** `55e19a4`  
**Final version:** `0.575-beta`  
**Final HEAD at report preparation:** `8ae5766`  
**Phase 8:** not started

---

## 1. Starting state

The branch was `main` with a clean working tree. The actual HEAD was `55e19a4`,
not the expected Phase 6 commit `3e01af1`, because two later user commits were
already on the branch:

1. `f42dc69 reffactor: fix stale data` — the Phase 10 plan addition and
   unrelated README/documentation cleanup.
2. `55e19a4 docs: place the Phase 7 note with its retrospective entries` — the
   version-log placement change requested before implementation.

Both were preserved exactly. `project_context/MMM_PLAN.md` still contains the
Phase 10 CDMM proposal and was not rewritten.

Baseline before Phase 7 changes:

| Check | Result |
| --- | --- |
| CMake version | `0.571-beta` |
| CLI | `nirqoba compiler version 0.571-beta (Phase 7)` |
| Unit tests | 323/323 pass |
| Full runner | 593/593 pass, 0 failed, 0 skipped |

Baseline logs are in the ignored `build/` directory:
`phase7_unit_baseline.log` and `phase7_full_baseline.log`.

---

## 2. Numbered commit list

Commits 1–2 are pre-existing user commits already on the branch. Commits 3–27
are the Phase 7 work. The final report/index documentation commits follow this
list.

| # | Commit | Subject |
| ---: | --- | --- |
| 1 | `f42dc69` | reffactor: fix stale data |
| 2 | `55e19a4` | docs: place the Phase 7 note with its retrospective entries |
| 3 | `3d75288` | docs: audit GC allocation, roots, and isolation for Phase 7 |
| 4 | `ec57960` | fix: reject managed payloads in tracked arena and pool allocations |
| 5 | `60dafa1` | test: cover arena and pool managed-payload rejection |
| 6 | `2ec7beb` | fix: classify shared allocations with managed payloads as GC effects |
| 7 | `0616b1c` | test: verify shared managed allocation enters the mutator region |
| 8 | `97b615d` | build: advance version to 0.575-beta |
| 9 | `46208ce` | test: prove ordinary runtime operations stay outside the collector |
| 10 | `8164f39` | test: extend GC-free O2 coverage for arena, pool, and helper chains |
| 11 | `58b6871` | test: preserve barriers at O2 and isolation with the debug runtime |
| 12 | `482e328` | test: add declaration- and dependency-aware GC isolation checks |
| 13 | `b5eedb3` | test: keep an owned managed field rooted across a collecting helper |
| 14 | `1a0118d` | test: cover shared root copies and repeated cycle reclamation |
| 15 | `3746503` | bench: measure GC roots, barriers, collection, cycles, and isolation |
| 16 | `70c81fb` | bench: record Phase 7 benchmark results and method |
| 17 | `897ba82` | docs: mark Phase 7 complete in the migration plan |
| 18 | `26e6bf3` | docs: record Phase 7 GC isolation and interoperability status |
| 19 | `96df435` | docs: advertise Phase 7 guarantees and current version |
| 20 | `2a892b2` | docs: add the Phase 7 GC migration guide |
| 21 | `769b5dd` | test: keep isolation fixtures on the explicit heap path |
| 22 | `6ff49b3` | docs: record the 0.575-beta GC isolation milestone |
| 23 | `4a31344` | docs: append the Phase 7 progress entry |
| 24 | `c4227b0` | test: prove ordinary managed storage is not a GC allocation |
| 25 | `4698e3c` | test: verify managed array element barriers at O2 |
| 26 | `e8f4e84` | docs: analyze the Phase 7 benchmark results |
| 27 | `8ae5766` | test: keep GC-independent helper bodies outside mutator regions |

No assistant or bot is listed as a co-author.

---

## 3. Allocation entry-point audit

The full classification is in `reports/memory-model/phase-7-audit.md`. Summary:

| Entry point | Ownership | Runtime path | Roots |
| --- | --- | --- | --- |
| `array.allocate<T>` | Explicit GC-managed | `nrq_gc_allocate_array_typed_rooted` | Rooted allocation + typed layout |
| `memory.owned_soa<T>` | Explicit GC-backed SoA, legacy name | `nrq_gc_allocate_soa_rooted` | Rooted allocation + column layout |
| Managed fields in `memory.allocate<T>` | Ordinary owned storage | `nrq_memory_allocate*` | Typed root; unregistered by release/drop |
| Managed fields in `memory.reallocate<T>` | Ordinary owned storage | `nrq_memory_reallocate` | Re-registration; release unregisters |
| Managed fields in `memory.shared_allocate<T>` | Shared-owned storage | `nrq_memory_shared_allocate_rooted` | Shared root; final release unregisters |
| Managed elements in `memory.owned_array<T>` | Ordinary owned array | `nrq_memory_allocate*` | Array root; release unregisters |
| Managed fields in stack aggregates/tuples | Ordinary storage | Stack + frame descriptor | Frame root |
| `memory.secure_allocate<T>` | Manual/secure | `nrq_memory_secure_allocate` | Managed payload rejected |
| Tracked arena/pool `memory.allocate<T>` | `ArenaBound` | `nrq_arena_allocate` / `nrq_memory_allocate_from` | **Managed payload rejected** |
| Internal GC allocation helpers | Compiler-generated | Collector page/span allocator | Rooted slot / explicit registration |

The audit found that a tracked arena or pool payload containing `managed<T>` was
accepted, registered a typed root, and then had its storage freed by reset or
destroy without root unregistration. A probe confirmed the stale-root state.
This was classified as an unsupported combination and rejected at Sema and TIR.

The audit also found that `function_uses_gc` did not seed from a shared
allocation whose payload contains a managed field. It did seed from ordinary
`memory.allocate`/`memory.reallocate` payloads. The shared case now seeds the
effect summary.

---

## 4. Root and root-frame findings

- Frame roots are opened before managed values can be created and ended in
  LIFO order. Early returns, branches, loops, recursion and nested frames use
  the same marker.
- Managed fields in ordinary owned storage get a typed root for the containing
  storage, not for every field. `memory.deallocate` and aggregate drop
  unregister it.
- Managed fields in shared storage get a shared root; retain does not add a
  second GC root, and final release removes it.
- Managed elements in owned arrays get an array root; release unregisters it.
- Stack-placed ordinary allocation with no managed fields gets no frame.
- The new `gc_owned_root_across_helper` fixture keeps a managed child reachable
  through an owned block across a helper that explicitly collects.
- The new `gc_shared_root_copy_cleanup` fixture retains a shared owner, releases
  one copy, collects, reads through the surviving owner, releases the final
  owner, and observes zero live objects.
- The new `gc_cycle_reclamation_phase7` fixture builds and collects three
  unreachable two-node cycles and observes zero live objects.

No path remains where a registered root outlives its storage without an
explicit rejection. Manual allocator handles from `memory.allocator_create`
remain a manual contract and are not presented as an automatic GC guarantee.

---

## 5. Transitive effects

`function_uses_gc` seeds from managed types, managed fields, GC containers,
typed root registration, GC runtime calls and collection requests.
`summarize_collections` propagates collection points over direct calls to a
least fixpoint. `summarize_mutator_regions` propagates the mutator region over
direct calls.

Verified behavior:

- A GC-independent helper chain remains free of mutator entry and GC calls; the
  isolation script checks the helper body directly.
- A helper that allocates a managed value makes its caller a GC region; the
  existing loop/recursion fixtures and the new shared-managed fixture cover the
  direction of propagation.
- Recursive and mutually recursive functions converge because membership only
  grows.
- Undefined/external calls are conservative collection points unless on the
  explicit runtime collection-free list.
- Indirect calls have no complete target set in the current language surface and
  are treated conservatively.

The shared-managed effect was the only missing seed found in this audit.

---

## 6. Safepoints and barriers

Safepoints:

- GC-dependent loops carry a backedge poll; allocation and collection calls are
  collection points.
- `gc_safepoints` asserts `nrq_gc_mutator_enter`, `nrq_gc_safepoint` and
  `nrq_gc_mutator_leave`.
- `gc_region_transitive` keeps one region around a loop that allocates through
  a helper and asserts one safepoint.
- Ordinary loops and helper chains contain no poll.

Barriers:

- `nrq_gc_root_store` is present for managed field/element writes.
- `gc_handoff_order_o2` preserves publication, root store and unpin order at
  `-O2`.
- The new `gc_array_element_barrier` fixture verifies managed array element
  stores at `-O2`.
- Ordinary fields without managed references use ordinary stores; no barrier is
  added merely because storage is heap allocated.
- `owned_pointer_gc_roots` and `owned_array_gc_roots` preserve typed root
  registration/unregistration for managed payloads outside the GC heap.

---

## 7. Mutator coordination and concurrency

- Enter/leave register and detach a thread; ordinary-only threads do not enter
  the mutator set.
- Root frames are thread-local and LIFO.
- The collector parks registered mutators at safepoints and publishes roots.
- Shared control remains on 64 pointer-selected shards; no lock-order inversion
  was found between root metadata, shared control and allocator operations.
- Runtime unit tests cover concurrent allocation/collection, thread exit,
  repeated collection and progress.
- The Phase 7 benchmark harness adds a four-thread allocation run with
  concurrent collection: median `110.662 ns/object` over 400,000 allocations.
- There is no source-level thread-spawn surface, so language fixtures are
  single-threaded. This is recorded as a limitation, not hidden.

---

## 8. Mixed-strategy interoperability

- Owned storage holding a managed object: supported with a typed root; verified
  by `gc_owned_root_across_helper` and the existing owned-root fixtures.
- Shared storage holding a managed object: supported with a shared root; verified
  by `gc_shared_root_copy_cleanup` and `shared_field_gc_roots`.
- Managed object referencing an ordinary owner: no silent promotion; use shared
  ownership or a checked borrow.
- Arena/pool with managed payload: rejected at Sema and TIR.
- Managed cycles: reclaimed when unreachable; `gc_cycle_reclamation_phase7`
  covers repeated reclamation.
- Plain shared payloads: no GC root or layout; checked by the runtime isolation
  unit test.
- FFI ownership remains Phase 8 work; no complete foreign-pointer contract is
  claimed.

---

## 9. Isolation evidence at `-O0` and `-O2`

`tests/phase7_isolation.py` checked twelve ordinary fixtures at both levels:

- owned pointer, owned heap helper, owned array, nested owned aggregate;
- borrowed heap view;
- ordinary reallocation;
- secure allocation;
- plain shared allocation;
- GC-independent helper chain;
- arena allocation/reset/destroy;
- pool allocation/release;
- the development runtime (`--debug-enabled`).

For each fixture:

- emitted LLVM contains no `nrq_gc` name, declarations included;
- the object contains zero `nrq_gc` symbols;
- the executable exits with the expected code;
- the managed control object contains eight `nrq_gc` symbols, proving the check
  is not vacuous.

The script also inspects function bodies: `add_one` in
`gc_plain_helper_no_region` has no GC reference while `make_node` does. This
prevents the caller's required protocol from hiding an instrumented helper.

Additional integration expectations assert exact counts for root stores, root
frames, mutator entry and deallocations at both optimization levels.

Production and development runtimes share one source list. The development
runtime adds observation only; `--debug-enabled` does not change allocation
strategy, roots, barriers or collector semantics. The new debug isolation
fixture runs at `-O0` with the development library and contains no GC reference.
`scripts/compare_configuration_equivalence.py --filter '*gc*'` built and ran the
GC-related corpus in both configurations: **67 agreed, 0 skipped, 0 failed**.

---

## 10. Tests and commands

Commands:

```text
cmd //c "D:\Actual Project Files\Project Nirqoba\build\run_build.bat"
./build/msvc-dev/tests/nirqoba_unit_tests.exe
python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe \
    --configuration RelWithDebInfo
python tests/phase7_isolation.py
python benchmarks/memory-model/phase-7/run_benchmark.py --runs 7
```

Results:

| Check | Result |
| --- | --- |
| Unit | **326/326 pass** |
| Full runner | **606/606 pass**, 0 failed, 0 skipped |
| Isolation script | 12 ordinary fixtures × 2 optimization levels + debug run; 0 failures |
| Managed control | 8 GC object symbols |
| Production/debug equivalence | 67 GC-related programs agreed, 0 skipped, 0 failed |
| Benchmark | 7 fresh-process samples per mode |

The full runner preserves all Phase 1–6 cases. New cases cover arena/pool
rejection (source and malformed TIR), shared managed effects, roots across
helpers and shared copies, cycles, O0/O2 isolation, O2 barriers and debug
runtime isolation.

---

## 11. Benchmarks

`benchmarks/memory-model/phase-7/` records 7 fresh-process samples per mode.
Full methodology and ranges are in
`reports/memory-model/phase-7-benchmarks.md`.

| Measurement | Median |
| --- | ---: |
| Managed 8-byte allocation | 54.476 ns/object |
| Root register + unregister | 64.848 ns/pair |
| Root frame begin + end | 28.101 ns/pair |
| Safepoint poll | 0.377 ns/poll |
| Write barrier | 1.782 ns/store |
| Full collection after 500k objects | 6.221 ms |
| Cycle build | 74.199 ns/object |
| Cycle collection (200k objects) | 2.181 ms |
| Four-thread allocation | 110.662 ns/object |
| Ordinary owned allocation | 57.962 ns/op |
| Borrowed array read | 0.072 ns/read |
| Arena allocation | 45.608 ns/op |
| Pool allocation + release | 141.927 ns/op |
| Plain-shared allocation + release | 243.988 ns/op |

All isolation modes left GC allocated bytes, collection count, live-object
count, page count and layout count unchanged. Peak RSS ranged from about 9.8 MB
for isolation to 44.2 MB for managed allocation. No universal speedup is
claimed, and no Phase 9 optimization was attempted.

---

## 12. Known limitations and deferred work

- Tracked arena and pool payloads cannot contain managed references; bulk
  reset/destroy has no root-unregistration point.
- Manual allocator handles created by `memory.allocator_create` retain a manual
  root-lifetime contract.
- Indirect calls are conservative collection points because there is no complete
  target set in the current language.
- FFI ownership and foreign-pointer rules are Phase 8 work.
- Weak references and a new cycle collector were not added.
- Automatic compiler-directed strategy selection (CDMM) was not added.
- There is no source-level thread-spawn API; concurrency evidence is from runtime
  tests and the benchmark harness.
- `memory.owned_soa<T>` remains an explicitly GC-backed container under a legacy
  name.

---

## 13. Working tree and publication

At report preparation the tree contained only the Phase 7 implementation, test,
benchmark and documentation changes. Build probes and logs are under the ignored
`build/` directory; temporary root artifacts were removed. No push, publish or
release was performed. Phase 8 was not started.

One later cleanup commit restored the pre-existing
`tests/integration/mlir/arena_gc_independence_o2` fixture after it was
accidentally overwritten with the O0 workload. The original Phase 4 fixture is
back in place; the new O2 isolation evidence is carried by the helper, pool and
debug fixtures. This restoration does not change runtime or compiler behavior.
