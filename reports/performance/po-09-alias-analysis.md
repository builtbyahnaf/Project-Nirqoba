# PO-09 — Ownership-Based Alias Analysis and Parameter Memory-Effects

**Task:** P0 3.1 — Alias Analysis and Memory-Effect Inference  
**Date:** 2026-11-04  
**Status:** Completed & Verified (Correctness tests passed; benchmarks deferred per policy)  
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, MSVC x64 Debug/RelWithDebInfo  

---

## 1. Problem Identified

Prior to this task, user-defined functions lowered from Nirqoba TIR into MLIR and LLVM IR did not propagate the compiler's rich semantic knowledge of ownership and borrowing into function signatures:
- Pointer arguments lacked aliasing and mutability attributes (`llvm.noalias`, `llvm.nocapture`, `llvm.readonly`).
- Pointers returned from functions with `Owned` ownership lacked `llvm.noalias`, preventing callers from knowing that returned pointers are unaliased.
- User-defined functions were emitted without `nounwind` passthrough attributes, forcing LLVM to treat every user call as a potential exception edge that inhibits interprocedural optimizations.
- Consequently, LLVM's MemorySSA, GVN, Dead Store Elimination, and alias-analysis passes were forced to make conservative assumptions:
  - Stores through mutable references were assumed to potentially clobber immutable references or other function inputs.
  - Redundant memory loads could not be hoisted out of loops or eliminated across calls.
  - LLVM could not infer `memory(argmem: readwrite)`, `writeonly`, `initializes`, or emit `!alias.scope` metadata.

## 2. Root Cause

In `compiler/lib/Lowering/TirToMlir.cpp`, when lowering TIR functions to MLIR `func.func` ops:
1. Argument attributes were constructed without querying `tir_fn->parameters()` ownership semantics (`param.ownership`).
2. Only raw ownership names (`nirqoba.ownership = "owned"`, etc.) were attached as dialect strings, which are uninterpreted by LLVM's alias infrastructure.
3. No check was performed to identify `MutableBorrowed` vs `Borrowed` pointers or to check if a borrowed parameter escapes via `return_borrow_parameter`.
4. Return values were not checked for `return_ownership() == ValueOwnership::Owned`.

## 3. Files Modified

- `compiler/lib/Lowering/TirToMlir.cpp`:
  - Attached `passthrough = ["nounwind"]` to all user-defined `func.func` declarations and definitions.
  - On `func.func` construction, iterated through `tir_fn->parameters()` for pointer types (`param.type.is_pointer()`):
    - `MutableBorrowed` parameters: mapped to `llvm.noalias` and `llvm.nocapture` (unless the parameter is the function's designated `return_borrow_parameter`).
    - `Borrowed` parameters: mapped to `llvm.readonly` and `llvm.nocapture` (unless the parameter is the function's designated `return_borrow_parameter`).
  - Attached `llvm.noalias` to the return value (`setResultAttr(0, ...)`) when `tir_fn->return_type().is_pointer()` and `tir_fn->return_ownership() == ValueOwnership::Owned`.
- `tests/integration/mlir/pointer_alias_attrs/`:
  - `main.nrqba`: New test exercising `copy_data(borrow mut int* dst, borrow int* src)` with memory allocation and pointer reads/writes.
  - `expect.json`: Verifies presence of `noalias`, `readonly`, and `captures(none)` in LLVM IR.
- `reports/performance/po-09-alias-analysis.md`: This 11-section documentation.

## 4. Algorithm or Transformation Implemented

During TIR-to-MLIR translation in `TirToMlir::lower_function`:

```cpp
// 1. Mark user-defined functions as nounwind (Nirqoba runtime handles errors via aborts/fatal)
func_op->setAttr("passthrough", builder.getArrayAttr({builder.getStringAttr("nounwind")}));

// 2. Map parameter ownership to LLVM alias and capture attributes
for (size_t i = 0; i < tir_fn->parameters().size(); ++i) {
    const auto& param = tir_fn->parameters()[i];
    if (param.type.is_pointer()) {
        bool escapes_via_return = (tir_fn->return_borrow_parameter() == param.name);
        if (param.ownership == tir::ValueOwnership::MutableBorrowed) {
            arg_attrs[i].push_back(builder.getNamedAttr(LLVM::LLVMDialect::getNoAliasAttrName(), builder.getUnitAttr()));
            if (!escapes_via_return) {
                arg_attrs[i].push_back(builder.getNamedAttr(LLVM::LLVMDialect::getNoCaptureAttrName(), builder.getUnitAttr()));
            }
        } else if (param.ownership == tir::ValueOwnership::Borrowed) {
            arg_attrs[i].push_back(builder.getNamedAttr(LLVM::LLVMDialect::getReadonlyAttrName(), builder.getUnitAttr()));
            if (!escapes_via_return) {
                arg_attrs[i].push_back(builder.getNamedAttr(LLVM::LLVMDialect::getNoCaptureAttrName(), builder.getUnitAttr()));
            }
        }
    }
}

// 3. Mark newly allocated / owned return pointers as noalias
if (tir_fn->return_type().is_pointer() && tir_fn->return_ownership() == tir::ValueOwnership::Owned) {
    func_op.setResultAttr(0, LLVM::LLVMDialect::getNoAliasAttrName(), builder.getUnitAttr());
}
```

## 5. Why the Approach is Correct

1. **Borrow Checker Semantics Guarantees (§7.3):**
   - In Nirqoba's type system and semantic analysis (Sema), `borrow mut` enforces exclusive, non-aliasing access for the lifetime of the borrow. No other active reference to the same memory location can coexist. This strictly satisfies LLVM's `noalias` contract.
   - Immutable `borrow` guarantees that the callee will never mutate the referenced memory through this pointer, strictly satisfying LLVM's `readonly` contract.
2. **Nocapture Correctness:**
   - A borrowed reference is scoped to the call. If the parameter is designated as the source of a borrowed return value (`return_borrow_parameter() == param.name`), it escapes back to the caller; therefore, `nocapture` is omitted for that parameter. For all other borrowed parameters, the language guarantees they do not outlive the function invocation, justifying `nocapture` (`captures(none)`).
3. **Owned Pointer Return:**
   - A function returning an `Owned` pointer transfers sole, unaliased ownership of a freshly allocated or distinct resource to the caller, satisfying `noalias` on the return value.
4. **Exclusion of Unproven Domains (§7.3):**
   - `Shared`, `Raw`, and `GcManaged` pointer parameters are deliberately excluded from `noalias`, as shared and GC objects may have concurrent or multiple aliased references.

## 6. Tests Added or Modified

- `tests/integration/mlir/pointer_alias_attrs/`:
  - `main.nrqba`: Contains `void function copy_data(borrow mut int* dst, borrow int* src)` and verifies that `b` and `a` are passed with proper attributes.
  - `expect.json`: Verifies `--emit=llvm` matches `define void @copy_data(ptr noalias ...` and `ptr readonly captures(none)`.
- Existing integration tests `tests/integration/mlir/ownership_attrs` and `borrowed_return_attrs` verified to pass without regression.
- **Suite Results:**
  - 116 / 116 unit tests passed.
  - 180 / 180 integration tests passed.

## 7. LLVM IR Evidence

From `nirqoba build tests/integration/mlir/pointer_alias_attrs/main.nrqba --emit=llvm`:

```llvm
; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: readwrite)
define void @copy_data(ptr noalias nofree writeonly captures(none) initializes((0, 8)) %0, ptr readonly captures(none) %1) local_unnamed_addr #2 {
  %3 = load i64, ptr %1, align 4
  store i64 %3, ptr %0, align 4
  ret void
}

define noundef i64 @main() local_unnamed_addr {
  %1 = tail call ptr @nrq_memory_allocate(i64 1, i64 8)
  %2 = tail call ptr @nrq_memory_allocate(i64 1, i64 8)
  store i64 42, ptr %1, align 4
  store i64 42, ptr %2, align 4, !alias.scope !1
  %3 = tail call i32 @nrq_print_int(i64 42)
  tail call void @nrq_memory_deallocate_checked(ptr nonnull %1)
  tail call void @nrq_memory_deallocate_checked(ptr nonnull %2)
  tail call void @nrq_memory_deallocate_checked(ptr null)
  tail call void @nrq_memory_deallocate_checked(ptr null)
  ret i64 0
}

!1 = !{!2}
!2 = distinct !{!2, !3, !"copy_data: argument 0"}
!3 = distinct !{!3, !"copy_data"}
```

Notice:
- `%0` receives `noalias nofree writeonly captures(none) initializes((0, 8))`.
- `%1` receives `readonly captures(none)`.
- Function memory effects were automatically inferred by LLVM as `memory(argmem: readwrite)`.
- LLVM generated fine-grained `!alias.scope` metadata (`!alias.scope !1`) for downstream optimization.

## 8. Before-and-After Benchmark Results

Deferred per instructions (§5: benchmark executions forbidden without explicit user command).

## 9. Compilation-Time and Memory Impact

Negligible. Adding parameter attribute dictionaries during `func.func` lowering is a single linear pass over the function's parameter vector at AST/TIR lowering time ($O(P)$ where $P \le 16$ typically).

## 10. Known Limitations

- Array view parameters (which are lowered as aggregate structs `{ptr, i64, i64}` rather than bare pointer parameters) do not yet receive field-level alias annotations in the function signature.
- Scoped TBAA metadata is not yet emitted on individual loads and stores.

## 11. Remaining Optimization Opportunities

- **P0 3.2 Escape Analysis and Allocation Elimination:** Exploit `nocapture` and lifetime bounds to promote non-escaping heap allocations to the stack and eliminate redundant allocation/deallocation pairs.
- **TBAA / Scoped NoAlias Metadata:** Attach LLVM alias scope and noalias domain metadata to distinct local alloca and dereference sites in TIR lowering.
