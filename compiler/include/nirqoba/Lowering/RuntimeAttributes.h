#pragma once

#include <cstdint>
#include <optional>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"

namespace nirq::runtime {

enum class MemoryEffectKind {
  Unspecified,
  None,     // memory(none)
  ErrnoMem, // errnoMem: readwrite, other: none
};

struct RuntimeFunctionPolicy {
  bool nounwind = false;
  bool willreturn = false;
  bool noreturn = false;
  bool cold = false;
  MemoryEffectKind memory = MemoryEffectKind::Unspecified;
  bool return_noalias = false;
  uint32_t nocapture_args_mask = 0;
};

inline std::optional<RuntimeFunctionPolicy> get_runtime_function_policy(llvm::StringRef symbol) {
  // 1. Pure math
  if (symbol == "nrq_pow_i64" || symbol == "nrq_sqrt" || symbol == "nrq_abs" ||
      symbol == "nrq_floor" || symbol == "nrq_ceil") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    policy.memory = MemoryEffectKind::None;
    return policy;
  }
  if (symbol == "nrq_sin" || symbol == "nrq_cos" || symbol == "nrq_pow_f64") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    policy.memory = MemoryEffectKind::ErrnoMem;
    return policy;
  }

  // 2. Fatal, bounds fail, and exit
  if (symbol == "nrq_memory_array_bounds_fail" || symbol == "nrq_fatal_error") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.noreturn = true;
    policy.cold = true;
    return policy;
  }
  if (symbol == "nrq_runtime_fatal") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.noreturn = true;
    policy.cold = true;
    policy.nocapture_args_mask = (1u << 0);
    return policy;
  }
  if (symbol == "nrq_system_exit") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.noreturn = true;
    policy.cold = true;
    return policy;
  }

  // 3. Print functions
  if (symbol == "nrq_print_int" || symbol == "nrq_print_uint" ||
      symbol == "nrq_print_f64" || symbol == "nrq_print_bool" ||
      symbol == "nrq_print_char") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    return policy;
  }
  if (symbol == "nrq_print_string" || symbol == "nrq_print_cstr") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    policy.nocapture_args_mask = (1u << 0);
    return policy;
  }

  // 4. Memory Allocators
  // Note: NO allocsize attribute attached to any allocator (avoids MallocLike uninit
  // miscompiles on zero-initialized GC storage, and avoids dead-allocation elimination
  // bypassing observable custom allocator callbacks / profiler counters).
  // Return noalias is used instead for alias analysis.
  if (symbol == "nrq_memory_allocate" || symbol == "nrq_memory_allocate_array" ||
      symbol == "nrq_memory_shared_allocate" || symbol == "nrq_memory_secure_allocate" ||
      symbol == "nrq_gc_allocate" || symbol == "nrq_gc_allocate_pinned") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.return_noalias = true;
    return policy;
  }
  if (symbol == "nrq_memory_allocate_from" || symbol == "nrq_memory_allocate_array_from" ||
      symbol == "nrq_memory_reallocate") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.return_noalias = true;
    policy.nocapture_args_mask = (1u << 0);
    return policy;
  }

  // 5. System queries & memory introspection
  if (symbol == "nrq_runtime_version") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    policy.memory = MemoryEffectKind::None;
    return policy;
  }
  if (symbol == "nrq_system_timestamp_ms" || symbol == "nrq_time_now_ms" ||
      symbol == "nrq_system_cpu_count" || symbol == "nrq_system_thread_count" ||
      symbol == "nrq_system_cpu_features" || symbol == "nrq_system_total_bytes" ||
      symbol == "nrq_system_used_bytes" ||
      symbol == "nrq_system_device_memory_total_bytes" ||
      symbol == "nrq_system_device_memory_used_bytes" ||
      symbol == "nrq_memory_available_bytes") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    return policy;
  }

  // 6. Bounds check functions
  // Note: NOT willreturn because they may terminate execution on failure.
  if (symbol == "nrq_memory_array_bounds_check" ||
      symbol == "nrq_memory_array_range_check" ||
      symbol == "nrq_memory_bounds_check") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.memory = MemoryEffectKind::None;
    return policy;
  }

  return std::nullopt;
}

inline void apply_runtime_function_attributes(
    mlir::LLVM::LLVMFuncOp func_op, mlir::OpBuilder& builder) {
  if (!func_op) return;
  auto maybe_policy = get_runtime_function_policy(func_op.getSymName());
  if (!maybe_policy) return;

  const auto& policy = *maybe_policy;
  llvm::SmallVector<mlir::Attribute, 4> passthrough_attrs;

  if (policy.nounwind) {
    func_op.setNoUnwind(true);
    passthrough_attrs.push_back(builder.getStringAttr("nounwind"));
  }
  if (policy.willreturn) {
    func_op.setWillReturn(true);
    passthrough_attrs.push_back(builder.getStringAttr("willreturn"));
  }
  if (policy.noreturn) {
    func_op.setNoreturn(true);
    passthrough_attrs.push_back(builder.getStringAttr("noreturn"));
  }
  if (policy.cold) {
    func_op.setCold(true);
    passthrough_attrs.push_back(builder.getStringAttr("cold"));
  }

  if (!passthrough_attrs.empty()) {
    func_op->setAttr("passthrough", builder.getArrayAttr(passthrough_attrs));
  }

  if (policy.memory == MemoryEffectKind::None) {
    auto mem_attr = mlir::LLVM::MemoryEffectsAttr::get(
        builder.getContext(),
        /*other=*/mlir::LLVM::ModRefInfo::NoModRef,
        /*argMem=*/mlir::LLVM::ModRefInfo::NoModRef,
        /*inaccessibleMem=*/mlir::LLVM::ModRefInfo::NoModRef,
        /*errnoMem=*/mlir::LLVM::ModRefInfo::NoModRef,
        /*targetMem0=*/mlir::LLVM::ModRefInfo::NoModRef,
        /*targetMem1=*/mlir::LLVM::ModRefInfo::NoModRef);
    func_op.setMemoryEffectsAttr(mem_attr);
  } else if (policy.memory == MemoryEffectKind::ErrnoMem) {
    auto mem_attr = mlir::LLVM::MemoryEffectsAttr::get(
        builder.getContext(),
        /*other=*/mlir::LLVM::ModRefInfo::NoModRef,
        /*argMem=*/mlir::LLVM::ModRefInfo::NoModRef,
        /*inaccessibleMem=*/mlir::LLVM::ModRefInfo::NoModRef,
        /*errnoMem=*/mlir::LLVM::ModRefInfo::ModRef,
        /*targetMem0=*/mlir::LLVM::ModRefInfo::NoModRef,
        /*targetMem1=*/mlir::LLVM::ModRefInfo::NoModRef);
    func_op.setMemoryEffectsAttr(mem_attr);
  }

  if (policy.return_noalias && func_op.getNumResults() > 0) {
    func_op.setResultAttr(
        0,
        mlir::LLVM::LLVMDialect::getNoAliasAttrName(),
        builder.getUnitAttr());
  }

  if (policy.nocapture_args_mask != 0) {
    for (unsigned idx = 0; idx < func_op.getNumArguments(); ++idx) {
      if ((policy.nocapture_args_mask & (1u << idx)) != 0) {
        func_op.setArgAttr(
            idx,
            mlir::LLVM::LLVMDialect::getNoCaptureAttrName(),
            builder.getUnitAttr());
      }
    }
  }
}

} // namespace nirq::runtime
