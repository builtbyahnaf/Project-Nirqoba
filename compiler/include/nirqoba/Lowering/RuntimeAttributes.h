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
  // Reads and writes global state (the standard streams, the heap, allocator
  // statistics) but touches neither argument memory nor inaccessible memory.
  OtherReadWrite,
  // As OtherReadWrite, and also reads through a pointer argument.
  OtherReadWriteArgRead,
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
  if (symbol == "nrq_memory_array_bounds_fail") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.noreturn = true;
    policy.cold = true;
    // Formats and writes a diagnostic to the error stream. It takes no
    // arguments, so argument memory is untouched even though globals are not.
    policy.memory = MemoryEffectKind::OtherReadWrite;
    return policy;
  }
  if (symbol == "nrq_fatal_error") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.noreturn = true;
    policy.cold = true;
    // Reads the caller's message and writes the error stream.
    policy.memory = MemoryEffectKind::OtherReadWriteArgRead;
    return policy;
  }
  if (symbol == "nrq_runtime_fatal") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.noreturn = true;
    policy.cold = true;
    policy.memory = MemoryEffectKind::OtherReadWriteArgRead;
    policy.nocapture_args_mask = (1u << 0);
    return policy;
  }
  if (symbol == "nrq_system_exit") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.noreturn = true;
    policy.cold = true;
    // Flushes the standard streams before terminating.
    policy.memory = MemoryEffectKind::OtherReadWrite;
    return policy;
  }

  // 3. Print functions
  if (symbol == "nrq_print_int" || symbol == "nrq_print_uint" ||
      symbol == "nrq_print_f64" || symbol == "nrq_print_bool" ||
      symbol == "nrq_print_char") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    policy.memory = MemoryEffectKind::OtherReadWrite;
    return policy;
  }
  if (symbol == "nrq_print_string" || symbol == "nrq_print_cstr") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    policy.memory = MemoryEffectKind::OtherReadWriteArgRead;
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

  // 7. GC mutator protocol and root management
  //
  // These routines report failures through nrq_runtime_fatal (which terminates
  // the process) and never propagate a C++ exception, so every one of them is
  // nounwind. None of them is marked memory(none): they read and write the
  // shadow stack, the global root table, the remembered set and allocator
  // statistics. The ones that only publish or remove bookkeeping for the
  // current activation cannot wait on another thread, so they also return.
  if (symbol == "nrq_gc_mutator_enter" || symbol == "nrq_gc_mutator_leave" ||
      symbol == "nrq_gc_safepoint") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    return policy;
  }
  if (symbol == "nrq_gc_root_frame_begin" || symbol == "nrq_gc_root_frame_end") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    return policy;
  }
  // The root slot itself is retained by the shadow stack (no nocapture on
  // argument 0), while the reference-offset array is copied into root metadata
  // and never kept.
  if (symbol == "nrq_gc_root_register_typed") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.nocapture_args_mask = (1u << 2);
    return policy;
  }
  if (symbol == "nrq_gc_root_register_array" ||
      symbol == "nrq_gc_root_clear_array_references") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.nocapture_args_mask = (1u << 3);
    return policy;
  }
  // The write barriers publish a new reference into the shadow stack and the
  // remembered set. They are emitted on the assignment path, so they must not
  // be treated as throwing call sites either.
  if (symbol == "nrq_gc_root_store" || symbol == "nrq_gc_root_copy") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    return policy;
  }
  if (symbol == "nrq_gc_root_unregister" || symbol == "nrq_gc_unpin") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    // The slot address is only compared, never stored.
    policy.nocapture_args_mask = (1u << 0);
    return policy;
  }
  if (symbol == "nrq_gc_pin" || symbol == "nrq_gc_pin_transfer") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    return policy;
  }
  if (symbol == "nrq_gc_allocate_array_typed_rooted") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.return_noalias = true;
    return policy;
  }
  // The configuration marker the entry wrapper calls. It does nothing, but it
  // must survive optimization: as a side-effecting call it is what makes the
  // linker require the library built from the matching configuration.
  if (symbol == "nrq_runtime_abi_v1_production" ||
      symbol == "nrq_runtime_abi_v1_development") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    policy.memory = MemoryEffectKind::OtherReadWrite;
    return policy;
  }
  // Development-only instrumentation. Both write the runtime's trace rings and
  // nothing else, so neither allocates, collects or blocks a mutator: they can
  // be called between an allocation and the statement that consumes it. The
  // site description and the site id are read-only arguments the runtime copies.
  if (symbol == "nrq_gc_debug_declare_site") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    policy.memory = MemoryEffectKind::OtherReadWriteArgRead;
    policy.nocapture_args_mask = (1u << 1);
    return policy;
  }
  if (symbol == "nrq_gc_debug_tag_allocation") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    policy.memory = MemoryEffectKind::OtherReadWrite;
    return policy;
  }
  // A collection waits for every other mutator to park, so it is not marked
  // willreturn; it still cannot unwind.
  if (symbol == "nrq_gc_collect") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    return policy;
  }
  // Initialization and shutdown run once around the program.
  if (symbol == "nrq_gc_init" || symbol == "nrq_gc_shutdown") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    return policy;
  }
  if (symbol == "nrq_gc_enable" || symbol == "nrq_gc_disable" ||
      symbol == "nrq_gc_set_threshold" || symbol == "nrq_gc_is_enabled" ||
      symbol == "nrq_gc_get_allocated_bytes" ||
      symbol == "nrq_gc_get_collected_bytes" ||
      symbol == "nrq_gc_get_collection_count" ||
      symbol == "nrq_gc_get_live_objects" ||
      symbol == "nrq_gc_pin_count" || symbol == "nrq_gc_pin_violations" ||
      symbol == "nrq_gc_ref_count_start" || symbol == "nrq_gc_ref_count_stop" ||
      symbol == "nrq_gc_get_ref_count") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
    return policy;
  }

  // 8. Synchronization primitives
  // The mutex family reports failure through nrq_runtime_fatal. Locking may
  // wait for another thread, so only create/unlock/destroy return immediately.
  if (symbol == "nrq_mutex_lock") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    return policy;
  }
  if (symbol == "nrq_mutex_create" || symbol == "nrq_mutex_unlock" ||
      symbol == "nrq_mutex_destroy") {
    RuntimeFunctionPolicy policy;
    policy.nounwind = true;
    policy.willreturn = true;
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
  } else if (policy.memory == MemoryEffectKind::OtherReadWrite ||
             policy.memory == MemoryEffectKind::OtherReadWriteArgRead) {
    auto mem_attr = mlir::LLVM::MemoryEffectsAttr::get(
        builder.getContext(),
        /*other=*/mlir::LLVM::ModRefInfo::ModRef,
        /*argMem=*/policy.memory == MemoryEffectKind::OtherReadWriteArgRead
            ? mlir::LLVM::ModRefInfo::Ref
            : mlir::LLVM::ModRefInfo::NoModRef,
        /*inaccessibleMem=*/mlir::LLVM::ModRefInfo::NoModRef,
        /*errnoMem=*/mlir::LLVM::ModRefInfo::NoModRef,
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
