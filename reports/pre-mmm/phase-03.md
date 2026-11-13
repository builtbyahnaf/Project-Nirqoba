# Pre-MMM Phase 3 - Make deterministic destruction complete

**Status:** Complete for currently supported control-flow paths; error/unwind cleanup remains future work
**Version:** 0.461-beta
**Date:** 2026-11-13
**Baseline:** 0.457-beta (`ab8eeab`)

## Scope

Phase 3 audited the existing ownership cleanup path before changing it: semantic
ownership state, TIR deferred expressions, `defer`, return and loop lowering, GC
root frames, and native resource destructors. The implementation keeps the
existing ownership rules and unifies how their destruction actions are tracked.

Supported exits covered by this phase:

- nested lexical blocks and scope fall-through;
- `if`/`else` branches and conditional initialization;
- `return` from any nesting depth;
- `while` and `for` body exits, `break`, and `continue`;
- `for` initializer lifetime at the loop boundary;
- moves into declarations, assignments, aggregate fields, calls, and returns;
- explicit `defer` ordering and ownership interaction;
- native resource fields and handles;
- GC root-frame closure on return edges.

Unsupported and explicitly not marked complete: error propagation and unwinding.
The language has no error/unwind model yet. The documented integration point is
the same lexical cleanup stack; a future unwind edge must consume active entries
down to the handler's scope depth before transferring control.

## Confirmed gaps and defects

### Deferred owner transferred afterwards

```nrqba
let buffer = memory.allocate<byte>(8);
defer memory.deallocate(buffer);
let owner = buffer;   // reproduced a runtime double-free
```

The defer captured the old pointer, while the new owner also received automatic
cleanup. The runtime aborted with `memory.deallocate received an invalid or already
freed pointer`. Sema now rejects moving, returning, field-extracting, aggregate-
packing, or transferring an owner after a defer scheduled its cleanup.

### `for` initializer outlived its scope

An owned value declared in a `for` initializer was registered in the enclosing
block's cleanup vector. It survived loop exit and was destroyed only when the
enclosing function/block ended. The initializer now has its own cleanup scope,
released at the loop exit edge. `continue` keeps it alive; `break`, a false
condition, and `return` destroy it correctly.

### Dead automatic cleanup after a transfer

A returned local, moved local, consumed parameter, or manually released pointer
still emitted an automatic cleanup call that loaded the nulled slot. The call was
safe because checked deallocation accepts null, but it was not a value requiring
destruction. TIR now deactivates the automatic action after a transfer that
dominates the owner's scope exit. Conditional transfers retain the fallback for
the path where the transfer did not happen.

### Explicit defer duplicated automatic destruction

`defer memory.deallocate(buffer)` and the declaration's automatic cleanup both
survived to scope exit. The explicit call cleared the slot, so the automatic call
received null rather than double-freeing, but it was a redundant destruction
action. A defer in the owner's scope, or an unconditional defer for an outer
owner, now supersedes the automatic action. A conditional defer keeps the outer
fallback.

## Implementation

- `compiler/include/nirqoba/TIR/Builder.h`, `compiler/lib/TIR/Builder.cpp`
  - Added a conditional-region depth. A transfer at depth zero dominates its
    owner's cleanup; transfers in branches, loop bodies, conditions, steps, and
    short-circuit RHS evaluation keep a fallback.
  - Added automatic-cleanup suppression and assignment re-tracking. A binding
    reinitialized after a move/reactivation gets a live cleanup for the new owner.
    The cleanup action loads the slot at exit, so it follows reassignment.
  - Same-scope defers suppress the matching automatic action; unconditional
    defers may suppress an outer action. Conditional defers keep the outer
    fallback and still clear the slot when executed.
  - `for` now pushes a deferred-expression scope with its initializer, releases
    it at the exit label, and pops it after the loop. The loop context begins
    inside that scope, so `break`/`continue` release only body scopes directly.
- `compiler/include/nirqoba/Sema/TypeChecker.h`,
  `compiler/lib/Sema/TypeChecker.cpp`
  - Track pending deferred releases by allocation id.
  - Reject later moves, returns, field moves, aggregate packing, and owning call
    transfers of that owner with `NRQ3033`.
  - Reassignment to a fresh owner remains legal; the defer releases the captured
    value and the automatic entry follows the new binding value.
- `tests/unit/tir_test.cpp`
  - Six new TIR tests assert cleanup counts, for-initializer order, transfer
    elision, parameter defer, and conditional fallbacks.

## Ordering semantics

Within a scope, automatic entries are recorded at initialization and explicit
defers at their statement. Scope exit consumes the stack in reverse: last
declaration/defer first. Nested scopes finish before enclosing scopes. Parameters
are initialized before the function body, so body values are destroyed first and
parameters last in reverse parameter order.

An explicit defer replaces the automatic action only when both are guaranteed to
run on the same path. If the defer is conditional or in an inner scope, the outer
automatic fallback remains. This preserves cleanup on the untaken path without
double-destroying on the taken path.

Aggregate destruction walks fields in reverse declaration order. Uninitialized
and moved fields hold null; typed resource destructors accept null and perform no
release.

## GC and native resources

GC root frames remain activation state. Every TIR `Return` closes the frame after
the cleanup calls and after the returned managed value is pinned. Loop exits do
not close an activation frame because they do not leave the function. Early
returns from nested branches and loops execute the same return cleanup and frame
close sequence.

Native resources are not GC-managed storage. Their typed destructors
(`file.close`, `socket.close`, `mutex.destroy`, mapping/secure release) are
emitted independently from `nrq_gc_root_unregister` for owned collector arrays.
Managed references do not enter deterministic deallocation.

## Verification

| Check | Result |
|---|---|
| Clean `RelWithDebInfo` build | PASS |
| Unit suite | 220/220 |
| Integration suite | 448/448 |
| Deferred-owner negatives | PASS (`defer_then_move`, `defer_then_return`, `defer_then_call_transfer`) |
| Transfer cleanup positives | PASS (returned local, moved local) |
| Defer ownership positives | PASS (explicit owner, parameter) |
| Conditional fallback positives | PASS (both runtime paths) |
| For-initializer cleanup | PASS (memory and native resource) |
| Early-return / GC-frame closure | PASS |
| Native/GC separation | PASS |
| Repeated loop stress (2,000 iterations) | PASS |
| Full existing cleanup/resource/GC suites | PASS |

The stress case exercises nested scopes, `continue`, and `break` with repeated
shared allocations. No ASan/UBSan run was available in this MSVC configuration;
correctness evidence is the full suites, raw-IR cleanup counts, runtime runs, and
the stress case.

## Benchmark

Three trials, seven compiler samples and three runtime samples per trial, Windows
11 x64 / MSVC 14.51.36231 / LLVM 23.1.4, `RelWithDebInfo`.

| Metric | Baseline 0.457-beta | Phase 3 | Result |
|---|---:|---:|---|
| Compile cleanup medians | 44.632 / 47.542 / 45.144 ms | 43.937 / 43.781 / 45.793 ms | 45.144 vs 43.937 ms median-of-medians |
| Compile cleanup mean medians | 44.993 / 46.800 / 45.551 ms | 44.575 / 44.218 / 45.331 ms | 45.551 vs 44.575 ms |
| Runtime cleanup medians | 465.018 / 467.828 / 521.086 ms | 481.992 / 453.638 / 471.902 ms | 467.828 vs 471.902 ms |
| Runtime cleanup means | 488.073 / 547.513 / 541.021 ms | 475.270 / 479.543 / 485.951 ms | 541.021 vs 479.543 ms |

The compile benchmark improved by about 2.7% at the median-of-medians. Runtime
medians overlap within run-to-run noise (Phase 3 0.9% higher at the median while
the mean is 11.4% lower), so no runtime speedup is claimed. The stable lowering
result is one fewer `nrq_memory_shared_release` call site per benchmark loop
iteration: four in the baseline, three in Phase 3, with identical output
(`2999997`).

## Limitations

- The checker rejects transfers after a pending defer rather than providing a
  defer-cancellation or ownership-transfer syntax.
- Conditional transfers and conditional defers deliberately retain runtime
  fallback cleanup on a null slot; eliminating those requires path-sensitive
  cleanup guards or richer liveness at labels.
- Cleanup of partially initialized aggregates still calls typed destructors with
  null for uninitialized/moved fields. The runtime destructors define null as a
  no-op; no generic dispatch is introduced.
- Raw `&` pointers remain outside deterministic ownership cleanup.
- Error/unwind cleanup is not implemented; only its integration point is
  documented.
- The benchmark is cleanup-focused, not a whole-compiler or whole-program
  performance suite.

## Commits

- `792d3fd` `sema: reject transfers after deferred cleanup`
- `2d3165f` `tir: unify cleanup liveness and loop scope`
- `62ec090` `test: reject transfers after deferred cleanup`
- `06ac283` `test: unit-test cleanup liveness and loop scope`
- `4fe756a` `test: cover transfer cleanup elision`
- `2182c07` `test: cover defer ownership destruction`
- `4c4e70e` `test: preserve conditional cleanup fallbacks`
- `06d25ea` `test: cover for-initializer destruction`
- `ff0d0ed` `test: cover GC frame closure and native cleanup`
- `cc954e6` `test: stress repeated cleanup paths`
- `b31ba35` `test: track inlined escaping allocation`
- `4a3e47c` `benchmark: measure cleanup compilation and runtime`
- `5d7638c` `docs: document deterministic cleanup ordering`
