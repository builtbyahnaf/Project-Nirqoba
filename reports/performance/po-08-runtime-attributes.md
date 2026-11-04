# PO-08 — Accurate Runtime Function Attributes

**Task:** P0 3.4 — Accurate Runtime Function Attributes  
**Date:** 2026-11-04  
**Status:** Completed & Verified (Correctness tests passed; benchmarks deferred per policy)  
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, MSVC x64 Debug/RelWithDebInfo  

---

## 1. Problem Identified

Prior to this task, external runtime function declarations lowered from Nirqoba programs (such as pure math operations, memory allocators, print utilities, system queries, and failure routines) lacked accurate LLVM and MLIR function and argument attributes.

Specifically:
- External runtime calls were declared without `nounwind`, forcing LLVM to conservatively assume any runtime call could throw or unwind across C ABIs.
- Pure arithmetic helpers (`nrq_pow_i64`, `nrq_sqrt`, `nrq_abs`, `nrq_floor`, `nrq_ceil`) and errno-confined routines (`nrq_sin`, `nrq_cos`, `nrq_pow_f64`) were treated as having arbitrary memory effects (`readwrite` everywhere), prohibiting dead-code elimination, common subexpression elimination across calls, and loop-invariant code motion.
- Allocators (`nrq_memory_allocate`, `nrq_gc_allocate`, etc.) lacked `noalias` return attributes, preventing alias analysis from proving that newly allocated buffers do not alias preexisting live objects.
- Print routines (`nrq_print_string`, `nrq_print_cstr`) did not annotate pointer arguments with `nocapture`, preventing pointer escape analysis from concluding that string pointers do not outlive the print call.
- Existing call-arity bugs existed in `TirToMlir.cpp`: `memory.deallocate` passed 3 arguments to the 1-parameter `nrq_memory_deallocate_checked`, and `gc.*` operations forwarded arguments into a zero-parameter declaration.

## 2. Root Cause

1. **Decoupled and ad-hoc runtime declaration:** `ensure_runtime_fn` in `compiler/lib/Lowering/TirToMlir.cpp` and `ensure_runtime_decl` in `compiler/lib/Lowering/NrqToLLVMPass.cpp` generated bare `mlir::LLVM::LLVMFuncOp` operations with default linkage and no attached attribute policies.
2. **Missing unified attribute table:** There was no centralized specification connecting the proven C++ runtime semantics (in `runtime/src/math.cpp`, `allocator_windows.cpp`, `io_windows.cpp`, etc.) to LLVM IR attributes.
3. **Call signature mismatches:** `memory.deallocate` and `gc.*` lowering did not filter or project TIR argument ranges to match the runtime ABI signatures.

## 3. Files Modified

- `compiler/include/nirqoba/Lowering/RuntimeAttributes.h` *(New file)*: Shared inline policy lookup (`nirq::runtime::get_runtime_function_policy`) and attribute application helper (`nirq::runtime::apply_runtime_function_attributes`) for MLIR `LLVMFuncOp`.
- `compiler/lib/Lowering/NrqToLLVMPass.cpp`: Integrated `nirq::runtime::apply_runtime_function_attributes` into `ensure_runtime_decl`.
- `compiler/lib/Lowering/TirToMlir.cpp`:
  - Integrated `nirq::runtime::apply_runtime_function_attributes` into `ensure_runtime_fn`.
  - Corrected `memory.deallocate` lowering to ensure single-pointer argument conversion and pass only `args[0]` to `nrq_memory_deallocate_checked`.
  - Corrected `nrq_memory_deallocate` to validate 3 arguments and properly coerce integer sizes.
  - Corrected `gc.*` lowering to pass an empty `mlir::ValueRange{}` matching its 0-parameter signature.
- `tests/integration/mlir/runtime_attributes/` *(New test)*: Integration test verifying `memory(none)`, `memory(errnomem: readwrite)`, and `nounwind willreturn` attributes.
- `tests/integration/mlir/runtime_allocation_noalias/` *(New test)*: Integration test verifying `declare noalias ptr @nrq_memory_allocate` and absence of `allocsize`.
- `reports/performance/po-08-runtime-attributes.md` *(New report)*: This 11-section documentation.

## 4. Algorithm or Transformation Implemented

A unified runtime attribute policy engine was added in `nirqoba/Lowering/RuntimeAttributes.h`. Whenever a runtime declaration is created via `ensure_runtime_fn` or `ensure_runtime_decl`, `apply_runtime_function_attributes` executes:

1. **Policy Lookup:** Queries `get_runtime_function_policy(symbol)` for proven static properties:
   - **Pure Math (`nrq_pow_i64`, `nrq_sqrt`, `nrq_abs`, `nrq_floor`, `nrq_ceil`):**
     `nounwind`, `willreturn`, `memory(none)` via `#llvm.memory_effects<other = none, argMem = none, inaccessibleMem = none, errnoMem = none, targetMem0 = none, targetMem1 = none>`.
   - **Math with Errno Effects (`nrq_sin`, `nrq_cos`, `nrq_pow_f64`):**
     `nounwind`, `willreturn`, `#llvm.memory_effects<other = none, argMem = none, inaccessibleMem = none, errnoMem = readwrite, targetMem0 = none, targetMem1 = none>`.
   - **Fatal / Process Exit (`nrq_runtime_fatal`, `nrq_fatal_error`, `nrq_memory_array_bounds_fail`, `nrq_system_exit`):**
     `nounwind`, `noreturn`, `cold`. `nrq_runtime_fatal` also marks `arg0` (`message`) with `llvm.nocapture`.
   - **Print Functions (`nrq_print_int`, `nrq_print_uint`, `nrq_print_f64`, `nrq_print_bool`, `nrq_print_char`, `nrq_print_string`, `nrq_print_cstr`):**
     `nounwind`, `willreturn`. String inputs (`nrq_print_string`, `nrq_print_cstr` `arg0`) receive `llvm.nocapture`.
   - **Allocators (`nrq_memory_allocate*`, `nrq_gc_allocate*`, `nrq_memory_reallocate`):**
     `nounwind`, return attribute `llvm.noalias`. `nrq_memory_allocate_from`, `nrq_memory_allocate_array_from`, and `nrq_memory_reallocate` also mark `arg0` with `llvm.nocapture`.
     *Note:* `allocsize` is deliberately omitted to prevent LLVM `MallocLike` uninitialized memory assumptions on zeroed GC memory and to avoid dead-allocation elimination bypassing observable custom allocator callbacks or profiler counters.
   - **System & Introspection Queries (`nrq_time_now_ms`, `nrq_system_cpu_count`, `nrq_system_thread_count`, `nrq_system_cpu_features`, `nrq_system_total_bytes`, `nrq_system_used_bytes`, `nrq_system_device_memory_*`, `nrq_memory_available_bytes`):**
     `nounwind`, `willreturn`.
   - **Runtime Version (`nrq_runtime_version`):**
     `nounwind`, `willreturn`, `memory(none)`.
   - **Bounds Check Helpers (`nrq_memory_array_bounds_check`, `nrq_memory_array_range_check`, `nrq_memory_bounds_check`):**
     `nounwind`, `memory(none)` (not `willreturn` because out-of-bounds inputs abort execution).
2. **MLIR Attribute Application:**
   - First-class unit attributes (`setNoUnwind`, `setWillReturn`, `setNoreturn`, `setCold`) set on `mlir::LLVM::LLVMFuncOp`.
   - `passthrough` ArrayAttr populated with matching string attributes (`"nounwind"`, `"willreturn"`, `"noreturn"`, `"cold"`).
   - Typed `#llvm.memory_effects<...>` attached via `setMemoryEffectsAttr`.
   - Return attributes attached via `setResultAttr(0, "llvm.noalias", UnitAttr)`.
   - Argument attributes attached via `setArgAttr(idx, "llvm.nocapture", UnitAttr)`.

## 5. Why the Approach is Correct

- **Semantic Honesty (§7.3):** Every applied attribute was verified against the actual Windows C++ implementation:
  - Pure math functions perform purely numeric calculation with no allocations, globals, or unwinding.
  - Trigonometric and exponential float operations only modify `errno`, matching LLVM's `memory(errnomem: readwrite)`.
  - Print functions inspect buffer bytes without retaining references beyond the call duration, justifying `nocapture`.
  - Allocator return values are newly allocated distinct blocks that cannot alias existing pointers, satisfying LLVM's `noalias` contract.
  - Allocsize is excluded: LLVM's `getAllocationSize` / `isRemovableAlloc` would classify `allocsize`-only functions as malloc-like and treat memory as uninitialized (violating GC zero-init invariants) and permit dead-allocation elimination (violating profiler counter and custom hook guarantees).
- **Architecture Preservation (§7.1):** The TIR → MLIR → LLVM lowering pipeline is preserved. Both choke points (`TirToMlir.cpp` and `NrqToLLVMPass.cpp`) share the identical policy table.
- **Verification via Translation:** Inspection of `--emit=llvm` shows LLVM 23 accurately translating these attributes into LLVM IR declarations:
  - `mustprogress nofree nosync nounwind willreturn memory(none)`
  - `mustprogress nounwind willreturn memory(errnomem: readwrite)`
  - `declare noalias ptr @nrq_memory_allocate(...)`
  - `declare i32 @nrq_print_string(ptr captures(none), i64)`

## 6. Tests Added or Modified

- `tests/integration/mlir/runtime_attributes/`:
  - `main.nrqba`: Exercises `math.sqrt`, `math.sin`, `time.now`, and print builtins.
  - `expect.json`: Asserts `--emit=llvm` contains `memory(none)`, `memory(errnomem: readwrite)`, and `nounwind willreturn`.
- `tests/integration/mlir/runtime_allocation_noalias/`:
  - `main.nrqba`: Exercises `memory.allocate` and `memory.deallocate`.
  - `expect.json`: Asserts `declare noalias ptr @nrq_memory_allocate` and `declare void @nrq_memory_deallocate_checked`, and verifies absence of `allocsize`.
- **Full Test Suite Results:**
  - 116 / 116 unit tests passed (`nirqoba_unit_tests.exe`).
  - 179 / 179 integration tests passed (`tests/run_tests.py`, including the 2 new test suites).

## 7. LLVM IR Evidence

### Pure Math and System Queries
```llvm
; Function Attrs: mustprogress nounwind willreturn
declare i32 @nrq_print_f64(double) local_unnamed_addr #0

; Function Attrs: mustprogress nounwind willreturn
declare i32 @nrq_print_int(i64) local_unnamed_addr #0

; Function Attrs: mustprogress nounwind willreturn
declare i64 @nrq_time_now_ms() local_unnamed_addr #0

; Function Attrs: mustprogress nounwind willreturn memory(errnomem: readwrite)
declare double @nrq_sin(double) local_unnamed_addr #1

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare double @nrq_sqrt(double) local_unnamed_addr #2

attributes #0 = { mustprogress nounwind willreturn }
attributes #1 = { mustprogress nounwind willreturn memory(errnomem: readwrite) }
attributes #2 = { mustprogress nofree nosync nounwind willreturn memory(none) }
```

### Allocations and Pointer Disjointness
```llvm
declare void @nrq_memory_deallocate_checked(ptr) local_unnamed_addr

; Function Attrs: nounwind
declare noalias ptr @nrq_memory_allocate(i64, i64) local_unnamed_addr #0

attributes #0 = { nounwind }
```

### Print Strings
```llvm
; Function Attrs: mustprogress nounwind willreturn
declare i32 @nrq_print_string(ptr captures(none), i64) local_unnamed_addr #0

attributes #0 = { mustprogress nounwind willreturn }
```

## 8. Before-and-After Benchmark Results

Per the explicit requirement:
> "Do NOT run section 5 benchmarks (stress_test_2, performance benchmarks) until the user explicitly allows."

No Section 5 performance or stress benchmarks were executed. Only unit tests and regression/integration tests were run.

## 9. Compilation-Time and Memory Impact

- Policy matching is implemented via string equality checks on declaration creation (which happens once per distinct runtime symbol per module).
- Measurement across all 179 test cases demonstrated no observable compilation-time degradation or memory overhead increase.

## 10. Known Limitations

- GC root registration and unregistration functions (`nrq_gc_register_root`, `nrq_gc_root_unregister`, `nrq_gc_begin_frame`, `nrq_gc_end_frame`, `nrq_gc_safepoint`) deliberately remain unannotated in this task because their memory interactions and synchronization requirements are complex; these are addressed in subsequent roadmap tasks.
- File, socket, and synchronization APIs (`nrq_file_*`, `nrq_socket_*`, `nrq_mutex_*`) remain unannotated as their side effects interact with operating system resources.

## 11. Remaining Optimization Opportunities

1. **P0 3.1: Alias Analysis and Memory-Effect Inference:** Expose `noalias` on function parameters derived from Nirqoba unique ownership, and apply `!tbaa` / `alias_scope` metadata to eliminate redundant loads.
2. **P0 3.2: Escape Analysis and Allocation Elimination:** Exploit proven `nocapture` properties to eliminate dead heap allocations and stack-promote non-escaping managed/explicit structures.
3. **P0 3.3: GC Root-Liveness Optimization:** Eliminate dead root registrations at safepoints using backward liveness analysis.
