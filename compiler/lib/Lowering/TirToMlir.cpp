// TirToMlir.cpp — translates a TIR module into MLIR.

#include "nirqoba/Lowering/TirToMlir.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nirqoba/runtime/gc_inline.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Location.h"

#include "nirqoba/Diagnostics/Diagnostic.h"
#include "nirqoba/Lowering/DataLayout.h"
#include "nirqoba/Lowering/NrqDialect.h"
#include "nirqoba/Lowering/NrqOps.h"
#include "nirqoba/Lowering/RuntimeAttributes.h"
#include "nirqoba/TIR/LifetimeAnalysis.h"

namespace nirq {
using namespace std;
namespace {

static mlir::Type tir_type_to_mlir(const Type& type, mlir::MLIRContext* ctx,
                                   mlir::Builder& builder) {
  // A container is one block of columns, so its value is a pointer to that block;
  // the same representation an owned array's data pointer uses.
  if (type.is_soa()) return mlir::LLVM::LLVMPointerType::get(ctx);
  if (type.is_bool()) return builder.getI1Type();
  if (type.is_char()) return builder.getI32Type();
  if (type.is_f32()) return builder.getF32Type();
  if (type.is_f64()) return builder.getF64Type();
  if (type.is_integer()) {
    switch (type.bit_width()) {
      case 8: return builder.getI8Type();
      case 16: return builder.getI16Type();
      case 32: return builder.getI32Type();
      case 64: return builder.getI64Type();
      default: return builder.getI64Type();
    }
  }
  if (type.is_string()) return mlir::LLVM::LLVMPointerType::get(ctx);
  if (type.is_pointer() || type.is_managed_pointer())
    return mlir::LLVM::LLVMPointerType::get(ctx);
  if (type.is_owned_array()) {
    return mlir::LLVM::LLVMStructType::getLiteral(
        ctx, {mlir::LLVM::LLVMPointerType::get(ctx), builder.getI64Type()});
  }
  if (type.is_array_view()) {
    return mlir::LLVM::LLVMStructType::getLiteral(
        ctx, {mlir::LLVM::LLVMPointerType::get(ctx), builder.getI64Type()});
  }
  // A dynamic array is an inline three-word value: {buffer, length, capacity}.
  // The buffer belongs to the runtime's array allocator, but the container does
  // not: length and capacity are ordinary fields, which is what lets a property
  // read be a field extraction instead of a runtime query.
  if (type.is_dynamic_array()) {
    return mlir::LLVM::LLVMStructType::getLiteral(
        ctx, {mlir::LLVM::LLVMPointerType::get(ctx), builder.getI64Type(),
              builder.getI64Type()});
  }
  if (type.is_void()) return {};
  if (type.is_array()) {
    mlir::Type elem = tir_type_to_mlir(type.element_type(), ctx, builder);
    if (!elem) return {};
    return mlir::LLVM::LLVMArrayType::get(elem, static_cast<unsigned>(type.array_length()));
  }
  if (type.is_tuple() || type.is_struct()) {
    const bool is_struct = type.is_struct();
    if (!is_struct && type.tuple_elements().empty()) return {};
    if (is_struct && type.struct_fields().empty()) {
      // Opaque struct type by name — fields will be resolved during a full lowering pass.
      // We map it to an identified struct with the struct name.
      return mlir::LLVM::LLVMStructType::getIdentified(ctx, type.struct_name());
    }
    std::vector<mlir::Type> fields;
    fields.reserve(is_struct ? type.struct_fields().size() : type.tuple_elements().size());
    for_each_field_in_layout_order(type, [&](const Type& field) {
      mlir::Type field_type = tir_type_to_mlir(field, ctx, builder);
      if (field_type) fields.push_back(field_type);
    });
    if (fields.size() != (is_struct ? type.struct_fields().size() : type.tuple_elements().size())) {
      return {};
    }
    return mlir::LLVM::LLVMStructType::getLiteral(ctx, fields);
  }
  return {};
}

static mlir::Type llvm_ptr_type(mlir::MLIRContext* ctx) {
  return mlir::LLVM::LLVMPointerType::get(ctx);
}

// A fresh allocation is the strongest thing alias analysis can be told about a
// pointer: nothing the caller could already name overlaps it, and its base is a
// multiple of the layout's alignment. Both are stated on the allocator's
// declaration, which is where LLVM reads them from: a call inherits its callee's
// return attributes, so every call site gets `noalias` and the alignment without
// a second place to keep in sync.
static void mark_fresh_allocation(mlir::LLVM::LLVMFuncOp function,
                                  std::size_t alignment_bytes) {
  if (!function || function.getNumResults() == 0 || alignment_bytes == 0) return;
  mlir::MLIRContext* ctx = function.getContext();
  function.setResultAttr(0, mlir::LLVM::LLVMDialect::getNoAliasAttrName(),
                         mlir::UnitAttr::get(ctx));
  function.setResultAttr(
      0, mlir::LLVM::LLVMDialect::getAlignAttrName(),
      mlir::IntegerAttr::get(mlir::IntegerType::get(ctx, 64), alignment_bytes));
}

// Promote small, fixed-size owned arrays and raw allocations whose descriptors
// and data pointers are proven not to leave the current function. This intentionally
// rejects unknown uses instead of guessing at alias behavior.
struct StackAllocationLayout {
  enum class Kind {
    OwnedArray,
    RawPointer
  };
  Kind kind = Kind::OwnedArray;
  std::size_t count = 0;
  std::size_t alignment = 0;
};

// Alignment of a promoted stack buffer's slot.
//
// E3 measured what a 16-byte floor buys a vectorized scan, cache resident and
// hot (benchmarks/bottleneck_discovery/alignment/vector_alignment.cpp): aligned
// and unaligned 16-byte loads both measured 0.157 ns/element, and an
// 8-byte-misaligned base measured 0.158. The floor bought nothing, so the slot
// now takes the element's natural alignment, which is all a correct load needs.
// An explicit `memory.owned_array_aligned` request still raises it, because that
// is the programmer asking for a guarantee rather than the compiler guessing at
// one.
static std::size_t stack_slot_alignment(std::size_t natural_alignment,
                                        std::size_t bytes) {
  (void)bytes;
  return natural_alignment;
}

using BorrowedEscapeSummary =
    std::unordered_map<std::string, std::vector<bool>>;

static bool borrowed_view_parameter_does_not_escape(
    const tir::Function& function, std::size_t parameter_index,
    const BorrowedEscapeSummary& summary) {
  if (parameter_index >= function.parameters().size()) return false;
  const tir::Function::Parameter& parameter =
      function.parameters()[parameter_index];
  if (!parameter.type.is_array_view() ||
      (parameter.ownership != ValueOwnership::Borrowed &&
       parameter.ownership != ValueOwnership::MutableBorrowed))
    return false;
  if (function.return_borrow_parameter() == parameter.name)
    return false;
  const Type& element_type = parameter.type.element_type();
  if (!(element_type.is_integer() || element_type.is_float() ||
        element_type.is_bool() || element_type.is_char()))
    return false;

  std::unordered_set<int> view_slots;
  for (const tir::Op& op : function.operations()) {
    if (op.kind != tir::OpKind::Store || !op.operand.is_valid() ||
        !op.operand.type.is_array_view())
      continue;
    const bool local_slot = std::any_of(
        function.operations().begin(), function.operations().end(),
        [&op](const tir::Op& candidate) {
          return candidate.kind == tir::OpKind::Alloca &&
                 candidate.result.id == op.lhs.id;
        });
    if (local_slot) view_slots.insert(op.lhs.id);
  }

  std::unordered_set<int> descriptors{parameter.value.id};
  std::unordered_set<int> data_pointers;
  for (const tir::Op& op : function.operations()) {
    if (op.kind == tir::OpKind::Load && view_slots.count(op.lhs.id) &&
        op.result.is_valid())
      descriptors.insert(op.result.id);

    auto uses = [&](int id) {
      if (id < 0) return false;
      if (op.operand.id == id || op.lhs.id == id || op.rhs.id == id)
        return true;
      for (const tir::Value argument : op.arguments)
        if (argument.id == id) return true;
      for (const tir::Value value : op.aggregate_elements)
        if (value.id == id) return true;
      return false;
    };
    bool uses_descriptor = false;
    for (int id : descriptors) uses_descriptor |= uses(id);
    bool uses_data = false;
    for (int id : data_pointers) uses_data |= uses(id);
    if (!uses_descriptor && !uses_data) continue;

    if (op.kind == tir::OpKind::Store) {
      const bool local_view_store = view_slots.count(op.lhs.id) &&
                                    descriptors.count(op.operand.id);
      if (uses_descriptor && !local_view_store) return false;
      if (uses_data &&
          !(data_pointers.count(op.lhs.id) && !uses_descriptor))
        return false;
      continue;
    }
    if (op.kind == tir::OpKind::Call && uses_descriptor &&
        (op.callee == "memory.array_view" ||
         op.callee == "memory.array_view_mut") &&
        !op.arguments.empty() && descriptors.count(op.arguments[0].id) &&
        op.result.is_valid()) {
      descriptors.insert(op.result.id);
      continue;
    }
    if (op.kind == tir::OpKind::Call && uses_descriptor) {
      const auto callee = summary.find(op.callee);
      bool passed_view = false;
      bool all_borrows_are_safe = callee != summary.end();
      for (std::size_t index = 0; index < op.arguments.size(); ++index) {
        if (!descriptors.count(op.arguments[index].id)) continue;
        passed_view = true;
        if (callee == summary.end() || index >= callee->second.size() ||
            !callee->second[index])
          all_borrows_are_safe = false;
      }
      if (passed_view && all_borrows_are_safe) continue;
    }
    if (op.kind == tir::OpKind::AggregateExtract && uses_descriptor &&
        descriptors.count(op.operand.id) &&
        (op.field_index == 0 || op.field_index == 1)) {
      if (op.field_index == 0 && op.result.is_valid())
        data_pointers.insert(op.result.id);
      continue;
    }
    if (op.kind == tir::OpKind::GetElementPtr && uses_data &&
        data_pointers.count(op.lhs.id) && op.result.is_valid()) {
      data_pointers.insert(op.result.id);
      continue;
    }
    if (op.kind == tir::OpKind::Load && uses_data &&
        data_pointers.count(op.lhs.id) && op.result.is_valid() &&
        (op.result.type.is_integer() || op.result.type.is_float() ||
         op.result.type.is_bool() || op.result.type.is_char()))
      continue;
    if (op.kind == tir::OpKind::Length && uses_descriptor &&
        descriptors.count(op.operand.id))
      continue;
    if (op.kind == tir::OpKind::DynamicIndex && uses_descriptor &&
        descriptors.count(op.lhs.id) && op.result.is_valid() &&
        (op.result.type.is_integer() || op.result.type.is_float() ||
         op.result.type.is_bool() || op.result.type.is_char()))
      continue;
    return false;
  }
  return true;
}

// Runtime operations that use a borrowed pointer for the duration of the call
// and never retain it: the standard streams and the resource handles. A pointer
// passed to one of them does not escape the caller, so it does not disqualify
// the argument from `nocapture`.
//
// The list is an assertion about the runtime ABI, so anything missing from it is
// treated conservatively as an escape. That is the safe direction: the attribute
// is withheld, never wrongly claimed.
static bool runtime_call_consumes_borrow_without_retaining(
    const std::string& callee) {
  static const std::unordered_set<std::string> consumers = {
      "print",           "print_string",     "print_cstr",
      "nrq_print_int",   "nrq_print_uint",   "nrq_print_f64",
      "nrq_print_bool",  "nrq_print_char",   "nrq_print_string",
      "nrq_print_cstr",  "file.read",        "file.write",
      "file.read_text",  "file.write_text",  "file.append_text",
      "file.exists",     "file.close",       "file.read_text_owned",
      "file.free_text",  "file.text_live_count",
      "socket.send",
      "socket.receive",  "socket.close",     "mutex.lock",
      "mutex.unlock",    "mutex.destroy",
  };
  return consumers.count(callee) != 0;
}

// True when a store moves an ordinary scalar: a number, a boolean or a
// character. Only those are data. A pointer, a string, a managed reference or
// an aggregate can carry a pointer, so storing one into memory the caller can
// still observe publishes that pointer, wherever the value came from.
static bool store_moves_pointer_free_data(const tir::Op& op) {
  if (!op.operand.is_valid() || !op.aggregate_elements.empty()) return false;
  const Type& type = op.operand.type;
  return type.is_numeric() || type.is_bool() || type.is_char();
}

static bool contains_shared_pointer(const Type& type) {
  if (type.is_shared_pointer()) return true;
  if (type.is_array()) return contains_shared_pointer(type.element_type());
  if (type.is_tuple()) {
    for (const Type& element : type.tuple_elements())
      if (contains_shared_pointer(element)) return true;
  }
  if (type.is_struct()) {
    for (const auto& field : type.struct_fields())
      if (contains_shared_pointer(field.second)) return true;
  }
  return false;
}

static bool borrowed_pointer_parameter_does_not_escape(
    const tir::Function& function, std::size_t parameter_index,
    const BorrowedEscapeSummary& summary) {
  if (parameter_index >= function.parameters().size()) return false;
  const tir::Function::Parameter& parameter =
      function.parameters()[parameter_index];
  if (!parameter.type.is_pointer() ||
      (parameter.ownership != ValueOwnership::Borrowed &&
       parameter.ownership != ValueOwnership::MutableBorrowed))
    return false;
  if (function.return_borrow_parameter() == parameter.name)
    return false;

  std::unordered_set<int> local_slots;
  for (const tir::Op& op : function.operations()) {
    if (op.kind != tir::OpKind::Store || !op.operand.is_valid() ||
        op.operand.id != parameter.value.id)
      continue;
    const bool local_slot = std::any_of(
        function.operations().begin(), function.operations().end(),
        [&op](const tir::Op& candidate) {
          return candidate.kind == tir::OpKind::Alloca &&
                 candidate.result.id == op.lhs.id;
        });
    if (local_slot) local_slots.insert(op.lhs.id);
  }

  std::unordered_set<int> data_pointers{parameter.value.id};
  for (const tir::Op& op : function.operations()) {
    if (op.kind == tir::OpKind::Load && local_slots.count(op.lhs.id) &&
        op.result.is_valid())
      data_pointers.insert(op.result.id);

    auto uses = [&](int id) {
      if (id < 0) return false;
      if (op.operand.id == id || op.lhs.id == id || op.rhs.id == id)
        return true;
      for (const tir::Value argument : op.arguments)
        if (argument.id == id) return true;
      for (const tir::Value value : op.aggregate_elements)
        if (value.id == id) return true;
      return false;
    };
    bool uses_data = false;
    for (int id : data_pointers) uses_data |= uses(id);
    if (!uses_data) continue;

    if (op.kind == tir::OpKind::Store) {
      const bool local_store = local_slots.count(op.lhs.id) &&
                               data_pointers.count(op.operand.id);
      if (local_store) continue;
      // Storing through the pointer: the pointer is not captured by that, but
      // only a plain scalar is data. A pointer, string, managed reference or
      // aggregate written here would be published into memory the caller can
      // still reach, which is an escape regardless of where the value came
      // from.
      if (data_pointers.count(op.lhs.id) && !data_pointers.count(op.operand.id) &&
          store_moves_pointer_free_data(op))
        continue;
      return false;
    }
    if (op.kind == tir::OpKind::GetElementPtr && data_pointers.count(op.lhs.id) &&
        op.result.is_valid()) {
      data_pointers.insert(op.result.id);
      continue;
    }
    if (op.kind == tir::OpKind::Load && data_pointers.count(op.lhs.id)) {
      continue;
    }
    if (op.kind == tir::OpKind::Call) {
      if (runtime_call_consumes_borrow_without_retaining(op.callee)) continue;
      const auto callee = summary.find(op.callee);
      bool passed_pointer = false;
      bool all_borrows_are_safe = callee != summary.end();
      for (std::size_t index = 0; index < op.arguments.size(); ++index) {
        if (!data_pointers.count(op.arguments[index].id)) continue;
        passed_pointer = true;
        if (callee == summary.end() || index >= callee->second.size() ||
            !callee->second[index])
          all_borrows_are_safe = false;
      }
      if (passed_pointer && all_borrows_are_safe) continue;
    }
    return false;
  }
  return true;
}

static bool borrowed_parameter_does_not_escape(
    const tir::Function& function, std::size_t parameter_index,
    const BorrowedEscapeSummary& summary) {
  if (parameter_index >= function.parameters().size()) return false;
  const tir::Function::Parameter& parameter =
      function.parameters()[parameter_index];
  if (parameter.ownership != ValueOwnership::Borrowed &&
      parameter.ownership != ValueOwnership::MutableBorrowed)
    return false;
  if (function.return_borrow_parameter() == parameter.name)
    return false;
  if (parameter.type.is_array_view()) {
    return borrowed_view_parameter_does_not_escape(function, parameter_index, summary);
  }
  if (parameter.type.is_pointer()) {
    return borrowed_pointer_parameter_does_not_escape(function, parameter_index, summary);
  }
  return false;
}

// True when the function never writes through `parameter_index`: no store uses
// the parameter, a pointer derived from it, or the pointer reloaded from the
// slot it was spilled into as its destination.
//
// This is the proof behind `llvm.readonly`. Ownership alone only makes the
// promise plausible: the attribute says the body does not write through this
// pointer, which is a statement about the instructions, not about the type. A
// callee cannot write through it either, because handing an immutable borrow to
// a mutable or owning parameter is rejected before lowering, so the local scan
// is sufficient.
static bool parameter_is_never_stored_through(const tir::Function& function,
                                             std::size_t parameter_index) {
  if (parameter_index >= function.parameters().size()) return false;
  const tir::Function::Parameter& parameter =
      function.parameters()[parameter_index];
  if (parameter.ownership != ValueOwnership::Borrowed) return false;

  std::unordered_set<int> local_slots;
  for (const tir::Op& op : function.operations()) {
    if (op.kind != tir::OpKind::Store || !op.operand.is_valid() ||
        op.operand.id != parameter.value.id)
      continue;
    const bool local_slot = std::any_of(
        function.operations().begin(), function.operations().end(),
        [&op](const tir::Op& candidate) {
          return candidate.kind == tir::OpKind::Alloca &&
                 candidate.result.id == op.lhs.id;
        });
    if (local_slot) local_slots.insert(op.lhs.id);
  }

  std::unordered_set<int> data_pointers{parameter.value.id};
  for (const tir::Op& op : function.operations()) {
    if (op.kind == tir::OpKind::Load && local_slots.count(op.lhs.id) &&
        op.result.is_valid())
      data_pointers.insert(op.result.id);

    auto uses = [&](int id) {
      if (id < 0) return false;
      if (op.operand.id == id || op.lhs.id == id || op.rhs.id == id)
        return true;
      for (const tir::Value argument : op.arguments)
        if (argument.id == id) return true;
      for (const tir::Value value : op.aggregate_elements)
        if (value.id == id) return true;
      return false;
    };
    bool uses_data = false;
    for (int id : data_pointers) uses_data |= uses(id);
    if (!uses_data) continue;

    if (op.kind == tir::OpKind::Store) {
      if (data_pointers.count(op.lhs.id)) return false;
      continue;
    }
    if (op.kind == tir::OpKind::GetElementPtr &&
        data_pointers.count(op.lhs.id) && op.result.is_valid()) {
      data_pointers.insert(op.result.id);
      continue;
    }
    if (op.kind == tir::OpKind::Load || op.kind == tir::OpKind::Call) continue;
    // Ownership markers observe a pointer without writing through it.
    if (op.kind == tir::OpKind::Move || op.kind == tir::OpKind::Borrow ||
        op.kind == tir::OpKind::EndBorrow || op.kind == tir::OpKind::Drop)
      continue;
    if (op.kind == tir::OpKind::Return) continue;
    // Any other use of the pointer could write through it.
    return false;
  }
  return true;
}

static BorrowedEscapeSummary summarize_borrowed_parameters(
    const tir::Module& module) {
  BorrowedEscapeSummary summary;
  for (const auto& function : module.functions()) {
    std::vector<bool> parameters(function->parameters().size(), true);
    summary.emplace(function->name(), std::move(parameters));
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& function : module.functions()) {
      auto entry = summary.find(function->name());
      if (entry == summary.end()) continue;
      for (std::size_t index = 0; index < entry->second.size(); ++index) {
        if (entry->second[index] &&
            !borrowed_parameter_does_not_escape(*function, index, summary)) {
          entry->second[index] = false;
          changed = true;
        }
      }
    }
  }
  return summary;
}

static std::unordered_map<int, StackAllocationLayout> analyze_stack_allocations(
    const tir::Function& function,
    const BorrowedEscapeSummary& borrowed_summary) {
  for (const tir::Op& op : function.operations())
    if (op.kind == tir::OpKind::Call && op.callee == "memory.use_allocator")
      return {};
  std::unordered_map<int, int64_t> integer_constants;
  for (const tir::Op& op : function.operations())
    if (op.kind == tir::OpKind::ConstantInt && op.result.is_valid())
      integer_constants.emplace(op.result.id, op.integer_value);

  // Indexes that do not depend on the candidate allocation. They are built in
  // two linear passes instead of being recomputed per candidate: the previous
  // shape rescanned the body for every candidate, and the alloca lookups nested
  // one scan inside another, which made the analysis quadratic in the operation
  // count for every allocation in the function.
  std::unordered_set<int> alloca_slots;
  for (const tir::Op& op : function.operations())
    if (op.kind == tir::OpKind::Alloca && op.result.is_valid())
      alloca_slots.insert(op.result.id);

  std::unordered_map<int, std::vector<const tir::Op*>> stores_by_value;
  std::unordered_map<int, std::vector<const tir::Op*>> loads_by_slot;
  // Slots that receive an array view; the escape scan below has to know them.
  std::unordered_set<int> view_slots;
  for (const tir::Op& op : function.operations()) {
    if (op.kind == tir::OpKind::Store && op.operand.is_valid()) {
      stores_by_value[op.operand.id].push_back(&op);
      if (op.operand.type.is_array_view() && alloca_slots.count(op.lhs.id) != 0)
        view_slots.insert(op.lhs.id);
      continue;
    }
    if (op.kind == tir::OpKind::Load && op.result.is_valid())
      loads_by_slot[op.lhs.id].push_back(&op);
  }

  // A candidate's descriptor set grows while the escape scan walks the body, so
  // the scan can only ask "does this operation reference a member of the set".
  // Testing every descriptor against every operation made the analysis cubic in
  // practice, because each array view read adds another descriptor: a function
  // with 600 views spent most of its compile time here. Membership is a hash
  // lookup per referenced value instead.
  auto references_any = [](const tir::Op& op,
                           const std::unordered_set<int>& values) {
    if (values.empty()) return false;
    auto contains = [&values](int id) {
      return id >= 0 && values.count(id) != 0;
    };
    if (contains(op.operand.id) || contains(op.lhs.id) ||
        contains(op.rhs.id))
      return true;
    for (const tir::Value& argument : op.arguments)
      if (contains(argument.id)) return true;
    for (const tir::Value& element : op.aggregate_elements)
      if (contains(element.id)) return true;
    return false;
  };

  std::unordered_map<int, StackAllocationLayout> candidates;
  std::size_t promoted_bytes = 0;
  constexpr std::size_t kMaxArrayBytes = 4096;
  constexpr std::size_t kMaxFunctionFrameBytes = 16384;
  for (const tir::Op& allocation : function.operations()) {
    const bool custom_alignment =
        allocation.callee == "memory.owned_array_aligned";
    const bool is_owned_array =
        allocation.kind == tir::OpKind::Call &&
        (allocation.callee == "memory.owned_array" || custom_alignment) &&
        allocation.result.is_valid() && allocation.result.type.is_owned_array() &&
        allocation.arguments.size() == (custom_alignment ? 2u : 1u);

    const bool is_raw_pointer =
        allocation.kind == tir::OpKind::Call &&
        allocation.callee == "memory.allocate" &&
        allocation.result.is_valid() && allocation.result.type.is_pointer() &&
        allocation.arguments.size() == 1u;

    if (!is_owned_array && !is_raw_pointer)
      continue;

    if (is_owned_array) {
      const auto count_value = integer_constants.find(allocation.arguments[0].id);
      const uint64_t element_bytes = type_size(allocation.result.type.element_type());
      if (count_value == integer_constants.end() || count_value->second <= 0 ||
          element_bytes == 0 ||
          static_cast<uint64_t>(count_value->second) > kMaxArrayBytes / element_bytes)
        continue;
      const std::size_t count = static_cast<std::size_t>(count_value->second);
      const std::size_t bytes = count * static_cast<std::size_t>(element_bytes);
      std::size_t requested_alignment =
          type_alignment(allocation.result.type.element_type());
      if (custom_alignment) {
        const auto alignment_value = integer_constants.find(allocation.arguments[1].id);
        if (alignment_value == integer_constants.end() || alignment_value->second <= 0)
          continue;
        const uint64_t requested = static_cast<uint64_t>(alignment_value->second);
        if ((requested & (requested - 1)) != 0 ||
            requested > std::numeric_limits<std::size_t>::max())
          continue;
        requested_alignment = std::max(
            requested_alignment, static_cast<std::size_t>(requested));
      }
      requested_alignment = stack_slot_alignment(requested_alignment, bytes);
      if (requested_alignment > kMaxFunctionFrameBytes) continue;
      const std::size_t frame_cost = bytes + requested_alignment - 1;
      if (frame_cost > kMaxFunctionFrameBytes - promoted_bytes) continue;

      // The owned array must be stored exactly once, into a local slot, so the
      // pointer is either this frame's storage or not promoted at all.
      int owner_slot = -1;
      std::size_t initial_stores = 0;
      const auto owners = stores_by_value.find(allocation.result.id);
      if (owners != stores_by_value.end())
        for (const tir::Op* store : owners->second) {
          ++initial_stores;
          owner_slot = store->lhs.id;
        }
      if (initial_stores != 1 || owner_slot < 0) continue;
      if (alloca_slots.count(owner_slot) == 0) continue;

      std::unordered_set<int> descriptors{allocation.result.id};
      const auto slot_loads = loads_by_slot.find(owner_slot);
      if (slot_loads != loads_by_slot.end())
        for (const tir::Op* load : slot_loads->second)
          if (load->result.is_valid()) descriptors.insert(load->result.id);
      std::unordered_set<int> data_pointers;
      bool escapes = false;
      for (const tir::Op& op : function.operations()) {
        if (op.kind == tir::OpKind::Load && view_slots.count(op.lhs.id) &&
            op.result.is_valid())
          descriptors.insert(op.result.id);
        const bool uses_descriptor = references_any(op, descriptors);
        const bool uses_data = references_any(op, data_pointers);
        if (!uses_descriptor && !uses_data) continue;

        if (op.kind == tir::OpKind::Store) {
          const bool initialization = op.lhs.id == owner_slot &&
                                     op.operand.id == allocation.result.id;
          const bool local_view_store = uses_descriptor &&
              view_slots.count(op.lhs.id) && descriptors.count(op.operand.id);
          if ((uses_descriptor && !initialization && !local_view_store) ||
              (uses_data && op.operand.id >= 0 && data_pointers.count(op.operand.id)))
            escapes = true;
          continue;
        }
        if (op.kind == tir::OpKind::Call && uses_descriptor &&
            (op.callee == "memory.array_view" ||
             op.callee == "memory.array_view_mut") &&
            !op.arguments.empty() &&
            descriptors.count(op.arguments[0].id) && op.result.is_valid()) {
          descriptors.insert(op.result.id);
          continue;
        }
        if (op.kind == tir::OpKind::Call && (uses_descriptor || uses_data)) {
          const auto callee = borrowed_summary.find(op.callee);
          bool all_borrows_are_safe = callee != borrowed_summary.end();
          bool passed_view = false;
          for (std::size_t index = 0; index < op.arguments.size(); ++index) {
            if (!descriptors.count(op.arguments[index].id) &&
                !data_pointers.count(op.arguments[index].id))
              continue;
            passed_view = true;
            if (callee == borrowed_summary.end() ||
                index >= callee->second.size() || !callee->second[index])
              all_borrows_are_safe = false;
          }
          if (passed_view && all_borrows_are_safe) continue;
        }
        if (op.kind == tir::OpKind::AggregateExtract && uses_descriptor &&
            descriptors.count(op.operand.id) &&
            (op.field_index == 0 || op.field_index == 1)) {
          if (op.field_index == 0 && op.result.is_valid())
            data_pointers.insert(op.result.id);
          continue;
        }
        if (op.kind == tir::OpKind::Length && uses_descriptor &&
            descriptors.count(op.operand.id))
          continue;
        if (op.kind == tir::OpKind::DynamicIndex && uses_descriptor &&
            descriptors.count(op.lhs.id))
          continue;
        if (op.kind == tir::OpKind::GetElementPtr && uses_data &&
            data_pointers.count(op.lhs.id) && op.result.is_valid()) {
          data_pointers.insert(op.result.id);
          continue;
        }
        if (op.kind == tir::OpKind::Load && uses_data &&
            data_pointers.count(op.lhs.id))
          continue;
        if (op.kind == tir::OpKind::Call && uses_descriptor &&
            op.callee == "array.owned_release" && op.arguments.size() == 1 &&
            descriptors.count(op.arguments[0].id))
          continue;
        if (op.kind == tir::OpKind::Store && uses_data &&
            data_pointers.count(op.lhs.id) && !uses_descriptor)
          continue;
        // A move is an alias: the result inherits the descriptor identity of
        // its source, so later uses are still seen by the escape scan.
        if (op.kind == tir::OpKind::Move) {
          if (op.result.is_valid()) {
            if (descriptors.count(op.operand.id)) descriptors.insert(op.result.id);
            if (data_pointers.count(op.operand.id))
              data_pointers.insert(op.result.id);
          }
          continue;
        }
        // A field walk destroys what the storage holds; it neither keeps the
        // storage nor lets it out of the frame.
        if (op.kind == tir::OpKind::Borrow || op.kind == tir::OpKind::EndBorrow ||
            op.kind == tir::OpKind::Drop ||
            op.kind == tir::OpKind::AggregateFieldDestroy)
          continue;
        escapes = true;
      }
      if (escapes) continue;
      const StackAllocationLayout layout{
          StackAllocationLayout::Kind::OwnedArray, count, requested_alignment};
      candidates.emplace(allocation.result.id, layout);
      for (int id : descriptors) candidates.emplace(id, layout);
      promoted_bytes += frame_cost;
    } else if (is_raw_pointer) {
      const auto count_value = integer_constants.find(allocation.arguments[0].id);
      if (count_value == integer_constants.end() || count_value->second <= 0 ||
          static_cast<uint64_t>(count_value->second) > kMaxArrayBytes)
        continue;
      const std::size_t bytes = static_cast<std::size_t>(count_value->second);
      std::size_t requested_alignment =
          type_alignment(allocation.result.type.element_type());
      if (requested_alignment == 0) requested_alignment = 1;
      requested_alignment = stack_slot_alignment(requested_alignment, bytes);
      if (requested_alignment > kMaxFunctionFrameBytes) continue;
      const std::size_t frame_cost = bytes + requested_alignment - 1;
      if (frame_cost > kMaxFunctionFrameBytes - promoted_bytes) continue;

      int owner_slot = -1;
      std::size_t initial_stores = 0;
      const auto owners = stores_by_value.find(allocation.result.id);
      if (owners != stores_by_value.end())
        for (const tir::Op* store : owners->second) {
          ++initial_stores;
          owner_slot = store->lhs.id;
        }
      if (initial_stores != 1 || owner_slot < 0) continue;
      if (alloca_slots.count(owner_slot) == 0) continue;

      std::unordered_set<int> descriptors{allocation.result.id};
      std::unordered_set<int> data_pointers{allocation.result.id};
      const auto slot_loads = loads_by_slot.find(owner_slot);
      if (slot_loads != loads_by_slot.end())
        for (const tir::Op* load : slot_loads->second) {
          descriptors.insert(load->result.id);
          data_pointers.insert(load->result.id);
        }

      bool escapes = false;
      for (const tir::Op& op : function.operations()) {
        if (op.kind == tir::OpKind::Load && op.lhs.id == owner_slot &&
            op.result.is_valid()) {
          descriptors.insert(op.result.id);
          data_pointers.insert(op.result.id);
        }
        const bool uses_descriptor = references_any(op, descriptors);
        const bool uses_data = references_any(op, data_pointers);
        if (!uses_descriptor && !uses_data) continue;

        if (op.kind == tir::OpKind::Store) {
          const bool initialization = op.lhs.id == owner_slot &&
                                     op.operand.id == allocation.result.id;
          if (initialization) continue;
          if (data_pointers.count(op.lhs.id) && !data_pointers.count(op.operand.id))
            continue;
          escapes = true;
          continue;
        }
        if (op.kind == tir::OpKind::Load && uses_data &&
            data_pointers.count(op.lhs.id))
          continue;
        if (op.kind == tir::OpKind::GetElementPtr && uses_data &&
            data_pointers.count(op.lhs.id) && op.result.is_valid()) {
          data_pointers.insert(op.result.id);
          continue;
        }
        if (op.kind == tir::OpKind::Call) {
          if (op.callee == "memory.deallocate" &&
              !op.arguments.empty() && descriptors.count(op.arguments[0].id))
            continue;
          if (op.callee == "print" || op.callee == "nrq_print_int" ||
              op.callee == "nrq_print_uint" || op.callee == "nrq_print_f64" ||
              op.callee == "nrq_print_bool" || op.callee == "nrq_print_char")
            continue;
          const auto callee = borrowed_summary.find(op.callee);
          bool all_borrows_are_safe = callee != borrowed_summary.end();
          bool passed_arg = false;
          for (std::size_t index = 0; index < op.arguments.size(); ++index) {
            if (!descriptors.count(op.arguments[index].id) &&
                !data_pointers.count(op.arguments[index].id))
              continue;
            passed_arg = true;
            if (callee == borrowed_summary.end() ||
                index >= callee->second.size() || !callee->second[index])
              all_borrows_are_safe = false;
          }
          if (passed_arg && all_borrows_are_safe) continue;
          escapes = true;
          continue;
        }
        // A move aliases its source, so the result keeps the pointer identity
        // the escape scan is tracking.
        if (op.kind == tir::OpKind::Move) {
          if (op.result.is_valid()) {
            if (descriptors.count(op.operand.id)) descriptors.insert(op.result.id);
            if (data_pointers.count(op.operand.id))
              data_pointers.insert(op.result.id);
          }
          continue;
        }
        // A field walk destroys what the storage holds; it neither keeps the
        // storage nor lets it out of the frame.
        if (op.kind == tir::OpKind::Borrow || op.kind == tir::OpKind::EndBorrow ||
            op.kind == tir::OpKind::Drop ||
            op.kind == tir::OpKind::AggregateFieldDestroy)
          continue;
        escapes = true;
      }
      if (escapes) continue;

      const StackAllocationLayout layout{
          StackAllocationLayout::Kind::RawPointer, bytes, requested_alignment};
      candidates.emplace(allocation.result.id, layout);
      for (int id : descriptors) candidates.emplace(id, layout);
      for (int id : data_pointers) candidates.emplace(id, layout);
      promoted_bytes += frame_cost;
    }
  }
  return candidates;
}

static void collect_managed_reference_offsets(const Type& type, uint64_t base,
                                              std::vector<uint64_t>& offsets) {
  // A container value is one pointer to a traced object, so a slot holding it is a
  // pointer root. Its columns are traced through the object layout, not the slot.
  if (type.is_soa()) {
    offsets.push_back(base);
    return;
  }
  if (type.is_managed_pointer()) {
    offsets.push_back(base);
    return;
  }
  if (type.is_array()) {
    const uint64_t stride = type_size(type.element_type());
    for (std::size_t index = 0; index < type.array_length(); ++index)
      collect_managed_reference_offsets(type.element_type(),
                                        base + index * stride, offsets);
    return;
  }
  if (type.is_struct() || type.is_tuple()) {
    uint64_t field_offset = 0;
    for_each_field_in_layout_order(type, [&](const Type& field_type) {
      const uint64_t alignment = type_alignment(field_type);
      field_offset = (field_offset + alignment - 1) & ~(alignment - 1);
      collect_managed_reference_offsets(field_type, base + field_offset, offsets);
      field_offset += type_size(field_type);
    });
  }
}

static bool collect_has_managed_reference(const Type& type) {
  std::vector<uint64_t> offsets;
  collect_managed_reference_offsets(type, 0, offsets);
  return !offsets.empty();
}

// A release only has a collector root to remove when the released storage can
// contain a managed reference. The payload is the pointee of an owned pointer or
// the element type of an owned array; the container/pointer token itself is not a
// GC root. SoA storage is collector-owned and always has a root.
static bool release_payload_has_managed_reference(const Type& type) {
  if (type.is_soa() || type.is_managed_pointer()) return true;
  if (type.is_owned_pointer() || type.is_owned_array())
    return collect_has_managed_reference(type.element_type());
  return false;
}

static bool function_uses_gc(const tir::Function& function) {
  auto type_uses_gc = [](const Type& type) {
    if (collect_has_managed_reference(type)) return true;
    return type.is_owned_array() &&
           collect_has_managed_reference(type.element_type());
  };
  if (type_uses_gc(function.return_type())) return true;
  for (const tir::Function::Parameter& parameter : function.parameters())
    if (type_uses_gc(parameter.type)) return true;
  for (const tir::Op& op : function.operations()) {
    // Explicit storage can sit outside the GC heap while containing managed
    // references. Such a function still needs the mutator region: a callee can
    // collect while the owned block is its only root.
    if (op.kind == tir::OpKind::Call && op.result.is_valid() &&
        op.result.type.is_pointer() &&
        (op.callee == "memory.allocate" ||
         op.callee == "memory.manual_allocate" ||
         op.callee == "memory.reallocate" ||
         op.callee == "memory.shared_allocate") &&
        collect_has_managed_reference(op.result.type.element_type()))
      return true;
    // Only managed values require mutator/root instrumentation. A function that
    // merely queries GC state (or calls gc.collect) without holding a managed
    // reference does not need a mutator frame or loop safepoints.
    if ((op.result.is_valid() && type_uses_gc(op.result.type)) ||
        (op.operand.is_valid() && type_uses_gc(op.operand.type)) ||
        (op.lhs.is_valid() && type_uses_gc(op.lhs.type)) ||
        (op.rhs.is_valid() && type_uses_gc(op.rhs.type)))
      return true;
    for (const tir::Value argument : op.arguments)
      if (type_uses_gc(argument.type)) return true;
    for (const tir::Value element : op.aggregate_elements)
      if (type_uses_gc(element.type)) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// GC root liveness
//
// Root registration used to be activation-wide and unconditional: every stack
// slot able to hold a managed reference was registered at function entry, every
// managed store paid a write barrier, and every activation opened a root frame.
// Registration is not cheap - it allocates root metadata, takes the shadow
// stack lock and pushes a root that marking must then trace - and most slots
// die long before the next allocation can collect. The plan below answers one
// question per slot: can a collection observe this slot's contents while some
// later instruction still reads them? Only those slots are registered. Every
// other slot stays an ordinary stack slot, which additionally lets LLVM promote
// it and its contents to SSA values.
//
// A collection can run at two kinds of program point, and both are treated as
// collection points here:
//   * a call that may allocate or explicitly collect (transitively, so calls to
//     user functions inherit the property of their callees), and
//   * a loop back edge, where the generated code polls nrq_gc_safepoint and a
//     collection requested by another thread snapshots this thread's roots.
//
// A slot must stay registered across such a point when it is live there: the
// collector takes its snapshot at exactly that point, so a slot whose contents
// are still needed afterwards must be visible to it. When the analysis proves
// that a slot is never live across any collection point, it is registered
// nowhere and its stores skip the write barrier.

struct GcRootPlan {
  // Alloca results that must be registered as typed roots.
  std::unordered_set<int> rooted_slots;
  // Parameter values that need an activation-wide root slot.
  std::unordered_set<int> rooted_values;
  // Managed call results whose handoff needs a real root slot in the caller.
  std::unordered_set<int> rooted_call_results;
  // Stack addresses the collector can never observe; stores through them need
  // no write barrier and no root copy.
  std::unordered_set<int> unobserved_addresses;
  // The function contains an allocation that must publish its result through an
  // already registered root slot.
  bool has_rooted_allocation = false;
  // True when allocation temp root slots are the only roots needed and can be
  // registered solely inside the cold allocation slow path.
  bool only_cold_alloc_roots = false;
  // Whether a root frame (and therefore any root registration) is required.
  bool needs_root_frame = false;
};

// Runtime routines that never allocate a managed object and never run a
// collection. Calls to anything else - including every user function not proven
// collection-free - are treated as collection points. The list is deliberately
// narrow: a missing entry only costs precision, while a wrong entry would let
// the collector reclaim a value that is still live.
static bool callee_is_collection_free(llvm::StringRef callee) {
  static const std::unordered_set<std::string> collection_free = {
      "print", "nrq_print_int", "nrq_print_uint", "nrq_print_f64",
      "nrq_print_bool", "nrq_print_char", "nrq_print_string",
      "nrq_print_cstr", "nrq_strlen", "nrq_args_length", "nrq_args_get",
      "math.sqrt", "sqrt", "math.sin", "sin", "math.cos", "cos",
      "math.abs", "abs", "math.floor", "floor", "math.ceil", "ceil",
      "time.now", "time.cycles", "memory.used_bytes", "memory.total_bytes",
      "memory.device_memory_total_bytes", "memory.device_memory_used_bytes",
      "memory.available_bytes", "memory.cpu_count", "memory.cpu_features",
      "memory.thread_count", "memory.allocator_kind", "memory.runtime_version",
      "memory.pin_count", "memory.pin_violations", "memory.get_ref_count",
      "memory.array_bounds_check", "gc.is_enabled", "gc.set_threshold",
      "gc.set_generational", "gc.disable", "mutex.create", "mutex.lock",
      "mutex.unlock", "mutex.destroy", "memory.get_allocator",
      "memory.get_default_allocator", "memory.profiler.start",
      "memory.profiler.stop", "memory.profiler.capture",
      // The general allocator, the ref-counted allocator and the mapping, file
      // and socket APIs never enter the collector: only nrq_gc_allocate* runs
      // maybe_collect_automatically, and only gc.collect* runs a cycle.
      "memory.owned_array", "memory.owned_array_aligned", "memory.allocate",
      "memory.manual_allocate", "memory.deallocate", "memory.manual_release",
      "memory.reallocate", "memory.shared_allocate",
      "memory.shared_retain",
      "memory.shared_release", "memory.shared_count", "memory.secure_allocate",
      "memory.secure_release", "memory.secure_zero", "memory.start_ref_count",
      "memory.stop_ref_count", "memory.pin", "memory.unpin", "memory.map_file",
      "memory.unmap_file", "memory.mapping_size", "memory.mapped_read",
      "memory.mapped_write", "memory.general_allocator", "memory.pool_allocator",
      "memory.arena_allocator", "memory.region", "memory.stack_allocator",
      "memory.slab_allocator", "memory.buddy_allocator",
      "memory.free_list_allocator", "memory.tlsf_allocator",
      "memory.use_allocator", "memory.reset_allocator",
      "memory.destroy_allocator", "memory.set_default_allocator",
      "memory.array_view", "memory.array_view_mut", "array.owned_release",
      "nrq_ffi_free",
      "memory.dynamic_array", "memory.array_live_count",
      "memory.array_append",
      "aggregate.owned_drop", "file.open", "file.close", "file.write",
      "file.read", "file.read_text", "file.read_text_owned",
      "file.free_text", "file.text_live_count", "file.write_text",
      "file.append_text",
      "file.exists", "socket.connect", "socket.close", "socket.send",
      "socket.receive", "system.exit",
  };
  return collection_free.count(callee.str()) != 0;
}

// Module-wide answer to "can this call run a collection?". A call to a module
// function inherits that function's answer; a call to anything else is a
// collection point unless the runtime routine is on the proven list above.
struct CollectionSummary {
  std::unordered_set<std::string> defined_functions;
  std::unordered_set<std::string> collecting_functions;

  bool call_may_collect(const std::string& callee) const {
    if (collecting_functions.count(callee) != 0) return true;
    if (defined_functions.count(callee) != 0) return false;
    return !callee_is_collection_free(callee);
  }
};

// Greatest fixpoint over the call graph: a function may trigger a collection
// when any operation in it can, where a call inherits its callee's answer.
static CollectionSummary summarize_collections(const tir::Module& module) {
  CollectionSummary summary;
  for (const auto& function : module.functions())
    summary.defined_functions.insert(function->name());

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& function : module.functions()) {
      if (summary.collecting_functions.count(function->name())) continue;
      for (const tir::Op& op : function->operations()) {
        if (op.kind != tir::OpKind::Call) continue;
        if (!summary.call_may_collect(op.callee)) continue;
        summary.collecting_functions.insert(function->name());
        changed = true;
        break;
      }
    }
  }
  return summary;
}

// Which functions have to run inside a mutator region. Holding a managed value is
// one reason; calling a function that may allocate or collect is another, and it is
// the one that matters for batching: a loop in a non-managed helper that calls an
// allocating function five hundred thousand times would otherwise enter and leave
// the protocol on every iteration, and each leave publishes whatever the TLAB holds.
//
// This is deliberately not the root-frame question. A function can need a region
// while needing no root frame, and a function that only queries GC state still
// needs neither.
static std::unordered_set<std::string> summarize_mutator_regions(
    const tir::Module& module) {
  std::unordered_set<std::string> regions;
  for (const auto& function : module.functions())
    if (function_uses_gc(*function)) regions.insert(function->name());

  // Least fixpoint over direct calls. A function that calls a region-needing
  // function joins, so a caller that reaches an allocation only through a helper
  // keeps one region open across the whole call, and a recursive or mutually
  // recursive group converges because membership only grows.
  //
  // Calls to runtime entry points are not propagated. Querying GC state, toggling
  // collection or collecting explicitly needs no region of its own; the function
  // that allocates the value does, and it holds a managed value by definition.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& function : module.functions()) {
      if (regions.count(function->name()) != 0) continue;
      for (const tir::Op& op : function->operations()) {
        if (op.kind != tir::OpKind::Call) continue;
        if (regions.count(op.callee) == 0) continue;
        regions.insert(function->name());
        changed = true;
        break;
      }
    }
  }
  return regions;
}

// Backward liveness over TIR values. The CFG is implicit in the operation
// stream: labels split blocks, branches carry the edges, and an operation
// without a terminator falls through to its successor.
class ValueLiveness {
 public:
  explicit ValueLiveness(const tir::Function& function) {
    const std::vector<tir::Op>& operations = function.operations();
    const std::size_t op_count = operations.size();
    value_count_ = function.value_count();
    words_ = (value_count_ + 63) / 64;
    if (op_count == 0 || words_ == 0) return;

    live_in_.assign(op_count * words_, 0);
    live_out_.assign(op_count * words_, 0);
    successors_.resize(op_count);
    predecessors_.resize(op_count);

    std::unordered_map<int, std::size_t> label_positions;
    for (std::size_t index = 0; index < op_count; ++index)
      if (operations[index].kind == tir::OpKind::Label)
        label_positions.emplace(operations[index].label_id, index);
    label_positions_ = label_positions;

    auto label_position = [&](int label) -> long long {
      const auto found = label_positions.find(label);
      return found == label_positions.end()
                 ? -1
                 : static_cast<long long>(found->second);
    };

    for (std::size_t index = 0; index < op_count; ++index) {
      const tir::Op& op = operations[index];
      auto fall_through = [&]() {
        if (index + 1 < op_count) successors_[index].push_back(index + 1);
      };
      switch (op.kind) {
        case tir::OpKind::Branch: {
          const long long target = label_position(op.label_id);
          if (target >= 0)
            successors_[index].push_back(static_cast<std::size_t>(target));
          else
            fall_through();
          break;
        }
        case tir::OpKind::BranchCond: {
          const long long yes = label_position(op.true_label);
          const long long no = label_position(op.false_label);
          if (yes >= 0) successors_[index].push_back(static_cast<std::size_t>(yes));
          if (no >= 0) successors_[index].push_back(static_cast<std::size_t>(no));
          if (yes < 0 || no < 0) fall_through();
          break;
        }
        case tir::OpKind::Return:
        case tir::OpKind::Unreachable:
          break;
        default:
          fall_through();
          break;
      }
    }
    for (std::size_t index = 0; index < op_count; ++index)
      for (std::size_t successor : successors_[index])
        predecessors_[successor].push_back(index);

    std::vector<uint64_t> uses(words_, 0);
    std::vector<uint64_t> defs(words_, 0);
    std::vector<uint64_t> new_out(words_, 0);
    std::vector<std::size_t> worklist;
    std::vector<char> queued(op_count, 1);
    worklist.reserve(op_count);
    for (std::size_t index = 0; index < op_count; ++index)
      worklist.push_back(index);

    while (!worklist.empty()) {
      const std::size_t index = worklist.back();
      worklist.pop_back();
      queued[index] = 0;

      std::fill(uses.begin(), uses.end(), uint64_t{0});
      std::fill(defs.begin(), defs.end(), uint64_t{0});
      const tir::Op& op = operations[index];
      add_bit(uses, op.operand.id);
      add_bit(uses, op.lhs.id);
      add_bit(uses, op.rhs.id);
      for (const tir::Value argument : op.arguments)
        add_bit(uses, argument.id);
      for (const tir::Value element : op.aggregate_elements)
        add_bit(uses, element.id);
      add_bit(defs, op.result.id);

      std::fill(new_out.begin(), new_out.end(), uint64_t{0});
      for (std::size_t successor : successors_[index]) {
        const uint64_t* successor_in = &live_in_[successor * words_];
        for (std::size_t word = 0; word < words_; ++word)
          new_out[word] |= successor_in[word];
      }

      uint64_t* out = &live_out_[index * words_];
      uint64_t* in = &live_in_[index * words_];
      bool changed = false;
      for (std::size_t word = 0; word < words_; ++word) {
        if (out[word] != new_out[word]) {
          out[word] = new_out[word];
          changed = true;
        }
        const uint64_t updated = uses[word] | (new_out[word] & ~defs[word]);
        if (in[word] != updated) {
          in[word] = updated;
          changed = true;
        }
      }
      if (!changed) continue;
      for (std::size_t predecessor : predecessors_[index]) {
        if (queued[predecessor]) continue;
        queued[predecessor] = 1;
        worklist.push_back(predecessor);
      }
    }
  }

  bool live_in(std::size_t op_index, int value) const {
    return test(live_in_, op_index, value);
  }
  // Visits every value live at `op_index` in ascending id order. Root attribution
  // walks this instead of asking `live_in` once per candidate, which is what made it
  // quadratic in the number of roots.
  template <typename Callback>
  void for_each_live_in(std::size_t op_index, Callback&& callback) const {
    if (words_ == 0) return;
    const uint64_t* bits = &live_in_[op_index * words_];
    for (std::size_t word = 0; word < words_; ++word) {
      uint64_t chunk = bits[word];
      while (chunk != 0) {
        const unsigned bit = static_cast<unsigned>(llvm::countr_zero(chunk));
        callback(static_cast<int>(word * 64 + bit));
        chunk &= chunk - 1;
      }
    }
  }
  bool live_out(std::size_t op_index, int value) const {
    return test(live_out_, op_index, value);
  }
  bool is_backedge_target(int label_id, std::size_t op_index) const {
    const auto found = label_positions_.find(label_id);
    return found != label_positions_.end() && found->second < op_index;
  }

 private:
  bool test(const std::vector<uint64_t>& bits, std::size_t op_index,
            int value) const {
    if (words_ == 0 || value < 0) return false;
    const std::size_t id = static_cast<std::size_t>(value);
    if (id >= value_count_) return false;
    return (bits[op_index * words_ + id / 64] & (uint64_t{1} << (id % 64))) != 0;
  }

  void add_bit(std::vector<uint64_t>& bits, int value) const {
    if (words_ == 0 || value < 0) return;
    const std::size_t id = static_cast<std::size_t>(value);
    if (id >= value_count_) return;
    bits[id / 64] |= uint64_t{1} << (id % 64);
  }

  std::size_t value_count_ = 0;
  std::size_t words_ = 0;
  std::vector<uint64_t> live_in_;
  std::vector<uint64_t> live_out_;
  std::vector<std::vector<std::size_t>> successors_;
  std::vector<std::vector<std::size_t>> predecessors_;
  std::unordered_map<int, std::size_t> label_positions_;
};

namespace {
GcRootAnalysisStats& analysis_stats() {
  static GcRootAnalysisStats stats;
  return stats;
}
}  // namespace

static GcRootPlan analyze_gc_root_plan(const tir::Function& function,
                                       const CollectionSummary& summary,
                                       bool has_collector_allocation = false) {
  const std::vector<tir::Op>& operations = function.operations();

  bool has_managed_slots = false;
  bool has_managed_parameters = false;
  bool has_managed_call_results = false;
  bool has_owned_storage_roots = false;
  GcRootPlan plan;
  for (const tir::Op& op : operations) {
    if (op.kind == tir::OpKind::Alloca && op.result.is_valid() &&
        collect_has_managed_reference(op.result.type))
      has_managed_slots = true;
    if (op.kind == tir::OpKind::Call && op.result.is_valid() &&
        collect_has_managed_reference(op.result.type) &&
        op.callee != "memory.array_view" &&
        op.callee != "memory.array_view_mut")
      has_managed_call_results = true;
    // Both heap containers publish their result through an entry root slot. The
    // collector entry reads that slot while the allocation is still in flight, so
    // the frame must exist before the call even when the result is consumed
    // immediately (for example as an aggregate field initializer).
    if (op.kind == tir::OpKind::Call && op.result.is_valid() &&
        ((op.callee == "array.allocate" && op.result.type.is_managed_pointer()) ||
         (op.callee == "memory.owned_soa" && op.result.type.is_soa())))
      plan.has_rooted_allocation = true;
    // Stack-placed owned storage is registered inside the activation frame,
    // even when the function has no managed local of its own.
    if (op.kind == tir::OpKind::Call && op.result.is_valid() &&
        op.result.type.is_pointer() &&
        (op.callee == "memory.allocate" ||
         op.callee == "memory.manual_allocate" ||
         op.callee == "memory.reallocate") &&
        collect_has_managed_reference(op.result.type.element_type()))
      has_owned_storage_roots = true;
  }
  // A collected record publishes its result through an entry root slot for the
  // same reason the heap containers do: the collector reads that slot while the
  // allocation is in flight, so the frame has to exist before the call.
  plan.has_rooted_allocation |= has_collector_allocation;
  for (const tir::Function::Parameter& parameter : function.parameters())
    if (collect_has_managed_reference(parameter.type)) has_managed_parameters = true;

  if (!has_managed_slots && !has_managed_parameters && !has_managed_call_results) {
    plan.only_cold_alloc_roots =
        plan.has_rooted_allocation && !has_owned_storage_roots;
    plan.needs_root_frame = plan.has_rooted_allocation || has_owned_storage_roots;
    return plan;
  }

  GcRootAnalysisStats& stats = analysis_stats();
  ++stats.functions;
  stats.operations += operations.size();

  const ValueLiveness liveness(function);

  // Collection points: calls that may collect, plus loop back edges where the
  // thread parks and publishes its roots.
  std::vector<std::size_t> collection_points;
  for (std::size_t index = 0; index < operations.size(); ++index) {
    const tir::Op& op = operations[index];
    if (op.kind == tir::OpKind::Call) {
      if (summary.call_may_collect(op.callee)) collection_points.push_back(index);
      continue;
    }
    if (op.kind == tir::OpKind::Branch) {
      if (liveness.is_backedge_target(op.label_id, index))
        collection_points.push_back(index);
      continue;
    }
    if (op.kind == tir::OpKind::BranchCond &&
        (liveness.is_backedge_target(op.true_label, index) ||
         liveness.is_backedge_target(op.false_label, index)))
      collection_points.push_back(index);
  }

  auto live_at_any_collection_point = [&](int value) {
    ++stats.liveness_probes;
    for (std::size_t point : collection_points)
      if (liveness.live_in(point, value)) return true;
    return false;
  };

  // Stack slots whose type contains managed references, plus every address derived
  // from them, so field and element stores can be attributed. TIR values are SSA and
  // a derivation can only name a value defined before it, so one pass in program
  // order attributes every address: no fixpoint, and no rescanning the function once
  // per slot to find the derivations.
  std::unordered_map<int, int> slot_of_address;
  for (const tir::Op& op : operations) {
    if (!op.result.is_valid()) continue;
    if (op.kind == tir::OpKind::Alloca && collect_has_managed_reference(op.result.type)) {
      slot_of_address.emplace(op.result.id, op.result.id);
      continue;
    }
    if (op.kind != tir::OpKind::GetElementPtr) continue;
    ++stats.address_scans;
    const auto source = slot_of_address.find(op.lhs.id);
    if (source == slot_of_address.end()) continue;
    slot_of_address.emplace(op.result.id, source->second);
    ++stats.address_derivations;
  }

  // Slot -> every address that names part of it, kept in program order so the
  // candidates below are visited predictably.
  std::unordered_map<int, std::unordered_set<int>> slot_addresses;
  for (const auto& entry : slot_of_address)
    slot_addresses[entry.second].insert(entry.first);

  for (const auto& entry : slot_addresses) stats.slot_addresses += entry.second.size();

  // A value loaded out of a slot keeps the object alive only while its source slot
  // still holds the reference, so a slot has to stay registered as long as any
  // address of it, or any value loaded from it, is live at a collection point.
  // Asking liveness once per candidate made this quadratic; instead every address
  // and load is mapped to its slot once, and the live sets at the collection points
  // decide which slots are needed.
  std::unordered_map<int, int> slot_of_candidate;
  for (const auto& entry : slot_of_address) slot_of_candidate.emplace(entry.first, entry.second);
  for (const tir::Op& op : operations) {
    if (op.kind != tir::OpKind::Load || !op.result.is_valid()) continue;
    const auto source = slot_of_address.find(op.lhs.id);
    if (source == slot_of_address.end()) continue;
    slot_of_candidate.emplace(op.result.id, source->second);
    ++stats.loads;
  }

  std::unordered_set<int> needed_slots;
  for (std::size_t point : collection_points) {
    liveness.for_each_live_in(point, [&](int value) {
      ++stats.liveness_probes;
      const auto candidate = slot_of_candidate.find(value);
      if (candidate != slot_of_candidate.end()) needed_slots.insert(candidate->second);
    });
  }

  for (auto& entry : slot_addresses) {
    if (needed_slots.count(entry.first) != 0) {
      plan.rooted_slots.insert(entry.first);
    } else {
      plan.unobserved_addresses.insert(entry.second.begin(), entry.second.end());
    }
  }

  for (const tir::Function::Parameter& parameter : function.parameters()) {
    if (!collect_has_managed_reference(parameter.type)) continue;
    if (live_at_any_collection_point(parameter.value.id))
      plan.rooted_values.insert(parameter.value.id);
  }

  for (const tir::Op& op : operations) {
    if (op.kind != tir::OpKind::Call || !op.result.is_valid()) continue;
    if (!collect_has_managed_reference(op.result.type)) continue;
    if (live_at_any_collection_point(op.result.id))
      plan.rooted_call_results.insert(op.result.id);
  }

  plan.only_cold_alloc_roots =
      plan.has_rooted_allocation && !has_owned_storage_roots &&
      plan.rooted_slots.empty() && plan.rooted_values.empty() &&
      plan.rooted_call_results.empty();
  plan.needs_root_frame = !plan.rooted_slots.empty() ||
                          !plan.rooted_values.empty() ||
                          !plan.rooted_call_results.empty() ||
                          plan.has_rooted_allocation || has_owned_storage_roots;
  return plan;
}

static mlir::Location to_mlir_loc(const SourceLocation& loc, mlir::MLIRContext* ctx) {
  if (!loc.is_valid()) return mlir::UnknownLoc::get(ctx);
  return mlir::FileLineColLoc::get(ctx,
    llvm::StringRef(loc.file),
    static_cast<unsigned>(loc.line),
    static_cast<unsigned>(loc.column));
}

static llvm::StringRef ownership_spelling(ValueOwnership ownership) {
  // One spelling source for TIR printing, MLIR attributes and diagnostics.
  return ownership_name(ownership);
  return "trivial";
}

static mlir::LLVM::LLVMFuncOp ensure_runtime_fn(
    mlir::ModuleOp module, mlir::OpBuilder& builder,
    llvm::StringRef symbol, mlir::Type retTy, llvm::ArrayRef<mlir::Type> argTys) {
  if (auto fn = module.lookupSymbol<mlir::LLVM::LLVMFuncOp>(symbol)) {
    return fn;
  }
  mlir::OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(module.getBody());
  auto fnTy = mlir::LLVM::LLVMFunctionType::get(retTy, argTys);
  auto fn = builder.create<mlir::LLVM::LLVMFuncOp>(
      module.getLoc(), symbol, fnTy, mlir::LLVM::Linkage::External);
  nirq::runtime::apply_runtime_function_attributes(fn, builder);
  return fn;
}

// The frontend owns every local slot: `alloca`, `store` of the initial value,
// then a `load` per use. When that is the whole story, the slot is pure spill and
// the stored value can be used directly: no slot, no store, no loads. A slot is
// only eligible when the store that initializes it immediately follows the
// alloca, nothing else ever writes it, and every other use is a load. Those
// conditions describe exactly the single-assignment locals, and the language's
// definite assignment means the initializing store has already run wherever the
// slot is read.
//
// Slots that hold a managed reference are left alone: such a slot may also be a
// registered GC root, and a root must keep the address it was registered with.
struct SlotPromotion {
  // Slot value id -> the value stored into it, which every load now returns.
  std::unordered_map<int, int> slots;
};

static SlotPromotion analyze_promoted_slots(const tir::Function& function) {
  SlotPromotion promotion;
  const std::vector<tir::Op>& operations = function.operations();

  auto holds_no_managed_reference = [](const Type& type) {
    std::vector<uint64_t> offsets;
    collect_managed_reference_offsets(type, 0, offsets);
    return offsets.empty();
  };

  for (std::size_t index = 0; index + 1 < operations.size(); ++index) {
    const tir::Op& slot = operations[index];
    const tir::Op& init = operations[index + 1];
    if (slot.kind != tir::OpKind::Alloca || !slot.result.is_valid()) continue;
    if (!holds_no_managed_reference(slot.result.type)) continue;
    if (init.kind != tir::OpKind::Store || init.lhs.id != slot.result.id) continue;
    if (!init.operand.is_valid() || init.lhs.id == init.operand.id) continue;

    // The slot is spill only if nothing else ever writes it or takes its
    // address; a single unexpected use disqualifies it.
    bool read_only = true;
    for (std::size_t use = index + 2; use < operations.size() && read_only; ++use) {
      const tir::Op& op = operations[use];
      if (op.kind == tir::OpKind::Load && op.lhs.id == slot.result.id) continue;
      auto touches_slot = [&](const tir::Value& value) {
        return value.is_valid() && value.id == slot.result.id;
      };
      if (touches_slot(op.operand) || touches_slot(op.lhs) || touches_slot(op.rhs))
        read_only = false;
      for (const tir::Value& argument : op.arguments)
        if (touches_slot(argument)) read_only = false;
      for (const tir::Value& element : op.aggregate_elements)
        if (touches_slot(element)) read_only = false;
    }
    if (!read_only) continue;
    promotion.slots.emplace(slot.result.id, init.operand.id);
  }
  return promotion;
}

// Where a region's handle lives. A frame region is created once in the entry
// block and destroyed on every return; a loop region is created on the edge into
// the loop's header and destroyed on every edge out of it, so the storage is
// reclaimed when the loop finishes rather than when the activation does.
struct RegionScope {
  bool loop = false;
  // Positions in the operation stream: the loop's header label and the back edge
  // that closes it. The analysis measures them on the stream it walked; the
  // lowering re-derives them from `header_label` on the stream it emits, because
  // the teardown expansion changes that stream's length.
  std::size_t begin = 0;
  std::size_t end = 0;
  // The header label the span belongs to.
  int header_label = -1;
};

// One shared region: the scratch arena for the bounded sites of a scope, or one
// pool per layout group of the repeated ones. The scope is what decides where the
// create and the destroy go; the layout is what decides whether a group is worth
// a pool at all.
// The managed fields a payload struct was packed with, and the layout the fields
// belong to. Kept per struct name because one literal's field list describes
// every allocation of that payload.
struct PayloadTeardowns {
  Type layout;
  std::vector<std::pair<int, std::string>> fields;
};

struct RegionSelection {
  bool pooled = false;
  std::string layout_key;
  std::uint64_t slot_bytes = 0;
  std::size_t alignment = 1;
  RegionScope scope;
  std::vector<int> sites;
  // Every value naming one of the region's allocations: the Drop that would
  // release one binding must not expand into a call, because the region owns the
  // storage. A pooled region turns that Drop into a slot release instead.
  std::unordered_set<int> aliases;
};

struct PendingGcRoot {
  mlir::Value slot;
  Type type;
  mlir::Location loc;
  mlir::Value initial_value;
};

// String literals are module-scope globals, but lowering creates one translator
// per function. A table owned by the caller and handed to every translator keeps
// literal names unique across the module and materialises an identical literal
// once, whichever function mentions it first.
struct StringLiteralTable {
  std::unordered_map<std::string, std::string> symbols;
  int counter = 0;
};

class FunctionTranslator {
 public:
  FunctionTranslator(mlir::ModuleOp module,
                     const tir::Function& tir_fn,
                     const BorrowedEscapeSummary& borrowed_summary,
                     const CollectionSummary& collections,
                     mlir::func::FuncOp func_op,
                     mlir::OpBuilder& builder,
                     mlir::MLIRContext* ctx,
                     DiagnosticEngine& diag,
                     bool fast_math,
                     RuntimeConfiguration configuration,
                     StringLiteralTable& string_literals,
                     bool emit_abi_reference,
                     bool needs_mutator_region,
                     std::unordered_map<int, StackAllocationLayout>
                         stack_allocations,
                     std::vector<RegionSelection> regions,
                     int caller_arena_arg_index,
                     std::unordered_set<int> arena_param_sites,
                     std::unordered_set<int> arena_param_aliases,
                     std::unordered_map<int, std::size_t> arena_call_regions,
                     std::unordered_set<std::string> promoted_callees,
                     std::unordered_set<int> forwarded_call_results,
                     std::unordered_set<int> activation_sites,
                     std::unordered_set<int> collector_sites,
                     bool activation_handle_guaranteed,
                     std::unordered_map<std::string, PayloadTeardowns>
                         payload_field_releases,
                     const StructFieldTable* struct_types)
      : module_(module), tir_fn_(tir_fn), func_op_(func_op),
        builder_(builder), ctx_(ctx), diag_(diag), fast_math_(fast_math),
        configuration_(configuration),
        string_literals_(string_literals),
        emit_abi_reference_(emit_abi_reference),
        stack_allocations_(std::move(stack_allocations)),
        regions_(std::move(regions)),
        caller_arena_arg_index_(caller_arena_arg_index),
        arena_param_sites_(std::move(arena_param_sites)),
        arena_param_aliases_(std::move(arena_param_aliases)),
        arena_call_regions_(std::move(arena_call_regions)),
        promoted_callees_(std::move(promoted_callees)),
        forwarded_call_results_(std::move(forwarded_call_results)),
        activation_sites_(std::move(activation_sites)),
        collector_sites_(std::move(collector_sites)),
        activation_handle_guaranteed_(activation_handle_guaranteed),
        payload_field_releases_(std::move(payload_field_releases)),
        struct_types_(struct_types),
        gc_roots_(analyze_gc_root_plan(tir_fn, collections,
                                       !collector_sites_.empty())),
        uses_gc_mutator_(needs_mutator_region),
        needs_gc_root_frame_(uses_gc_mutator_ && gc_roots_.needs_root_frame),
        promoted_slots_(analyze_promoted_slots(tir_fn)) {
    const std::vector<tir::Op>& operations = tir_fn_.operations();
    for (std::size_t index = 0; index < operations.size(); ++index)
      if (operations[index].kind == tir::OpKind::Label)
        label_positions_.emplace(operations[index].label_id, index);
    // The span the analysis measured is on the stream it walked; a create, a
    // membership test and a destroy all act on the stream this lowering emits, and
    // that one is longer or shorter wherever a teardown was expanded. The header
    // label names the loop in both streams, so the span is re-derived from it: the
    // header's own position, then the last branch back to it, which is the back
    // edge.
    for (RegionSelection& region : regions_) {
      if (!region.scope.loop || region.scope.header_label < 0) continue;
      const auto header = label_positions_.find(region.scope.header_label);
      if (header == label_positions_.end()) continue;
      std::size_t end = header->second;
      for (std::size_t index = header->second + 1; index < operations.size();
           ++index) {
        const tir::Op& op = operations[index];
        if (op.kind != tir::OpKind::Branch &&
            op.kind != tir::OpKind::BranchCond)
          continue;
        if (op.label_id == region.scope.header_label ||
            op.true_label == region.scope.header_label ||
            op.false_label == region.scope.header_label)
          end = index;
      }
      // A loop with no back edge cannot host a region: keep the analysis answer,
      // which is what the guard upstream agreed to.
      if (end <= header->second) continue;
      region.scope.begin = header->second;
      region.scope.end = end;
    }
    // Region membership by TIR value: a site needs its region's handle, and a
    // Drop must hand its slot back to the pool that handed it out. A loop-scoped
    // region is also indexed by its header's position, which is where its create
    // runs.
    region_slots_.resize(regions_.size());
    for (std::size_t index = 0; index < regions_.size(); ++index) {
      for (int site : regions_[index].sites)
        region_of_site_.emplace(site, index);
      for (int value : regions_[index].aliases)
        region_of_value_.emplace(value, index);
      if (regions_[index].scope.loop)
        loop_regions_at_header_[regions_[index].scope.begin].push_back(index);
    }
    // A stack-owned root is observable only at a collection point. Calls that
    // may allocate or collect are points; a loop back edge is one too, because
    // the runtime safepoint there can publish this activation's roots.
    // Addresses inside stack-placed owned storage need no write barrier when
    // this activation has no collection point. Track the allocation result and
    // every address derived from it so the Store lowering can emit a plain store.
    for (const tir::Op& op : operations) {
      if (!op.result.is_valid()) continue;
      const auto allocation = stack_allocations_.find(op.result.id);
      if (allocation != stack_allocations_.end() &&
          allocation->second.kind == StackAllocationLayout::Kind::RawPointer) {
        unobserved_stack_owned_addresses_.insert(op.result.id);
        continue;
      }
      if (op.kind == tir::OpKind::GetElementPtr &&
          unobserved_stack_owned_addresses_.count(op.lhs.id) != 0)
        unobserved_stack_owned_addresses_.insert(op.result.id);
    }
    for (std::size_t index = 0; index < operations.size(); ++index) {
      const tir::Op& op = operations[index];
      if (op.kind == tir::OpKind::Call && collections.call_may_collect(op.callee)) {
        has_collection_point_ = true;
        break;
      }
      if (op.kind == tir::OpKind::Branch) {
        const auto target = label_positions_.find(op.label_id);
        if (target != label_positions_.end() && target->second < index) {
          has_collection_point_ = true;
          break;
        }
      }
      if (op.kind == tir::OpKind::BranchCond) {
        const auto true_target = label_positions_.find(op.true_label);
        const auto false_target = label_positions_.find(op.false_label);
        if ((true_target != label_positions_.end() && true_target->second < index) ||
            (false_target != label_positions_.end() && false_target->second < index)) {
          has_collection_point_ = true;
          break;
        }
      }
    }
    entry_block_ = func_op.addEntryBlock();
    builder_.setInsertionPointToStart(entry_block_);
    values_.resize(tir_fn.value_count());

    for (std::size_t i = 0; i < tir_fn.parameters().size(); ++i) {
      set(tir_fn.parameters()[i].value, entry_block_->getArgument(i));
    }

    // The entry point names the runtime configuration this object was compiled
    // for. Each runtime library defines exactly one of the two symbols, so
    // linking against the other one leaves this call unresolved instead of
    // producing a program that observes the heap differently from the way it was
    // compiled to. The call is never reached from user code and does nothing.
    if (emit_abi_reference_) {
      auto abi_check = ensure_runtime_fn(
          module_, builder_, runtime_abi_symbol(configuration_),
          mlir::LLVM::LLVMVoidType::get(ctx_), {});
      builder_.create<mlir::LLVM::CallOp>(mlir::UnknownLoc::get(ctx_), abi_check,
                                         mlir::ValueRange{});
    }

    if (uses_gc_mutator_) {
      auto mutator_enter = ensure_runtime_fn(
          module_, builder_, "nrq_gc_mutator_enter",
          mlir::LLVM::LLVMVoidType::get(ctx_), {});
      builder_.create<mlir::LLVM::CallOp>(
          mlir::UnknownLoc::get(ctx_), mutator_enter, mlir::ValueRange{});
    }

    // Functions that cannot touch a GC-managed value do not need a root frame.
    // Recursive numeric helpers previously paid an atomic begin and a mutex
    // lock on every call even though they had no roots to track.
    if (needs_gc_root_frame_) {
      auto begin_frame = ensure_runtime_fn(
          module_, builder_, "nrq_gc_root_frame_begin",
          builder_.getI64Type(), {});
      gc_root_frame_marker_ = builder_.create<mlir::LLVM::CallOp>(
          mlir::UnknownLoc::get(ctx_), begin_frame,
          mlir::ValueRange{}).getResult();
    }
    for (std::size_t i = 0; i < tir_fn.parameters().size(); ++i) {
      if (!gc_roots_.rooted_values.count(tir_fn.parameters()[i].value.id)) continue;
      std::vector<uint64_t> offsets;
      collect_managed_reference_offsets(tir_fn.parameters()[i].type, 0, offsets);
      if (offsets.empty()) continue;
      mlir::Type value_type = tir_type_to_mlir(tir_fn.parameters()[i].type, ctx_, builder_);
      if (!value_type) continue;
      mlir::Value slot = alloca_at_entry(value_type,
                                         mlir::UnknownLoc::get(ctx_));
      register_gc_root(slot, tir_fn.parameters()[i].type,
                       mlir::UnknownLoc::get(ctx_),
                       entry_block_->getArgument(i));
    }
  }

  bool translate() {
    prepare_frame_regions();
    const std::vector<tir::Op>& operations = tir_fn_.operations();
    for (std::size_t index = 0; index < operations.size(); ++index) {
      current_operation_index_ = index;
      const tir::Op& op = operations[index];
      // Forwarded values are only valid until the next call (which may publish,
      // move or collect what the slot holds) or the next control-flow join (whose
      // incoming edges this single-block map does not describe).
      const bool inline_cfg_call =
          op.kind == tir::OpKind::Call && op.result.is_valid() &&
          ((op.callee == "array.allocate" &&
            op.result.type.is_managed_pointer()) ||
           (op.callee == "memory.owned_soa" && op.result.type.is_soa()));
      if (op.kind == tir::OpKind::Call || op.kind == tir::OpKind::Label ||
          op.kind == tir::OpKind::Branch ||
          op.kind == tir::OpKind::BranchCond) {
        forwarded_root_values_.clear();
        if (!inline_cfg_call) invalidate_inline_cfg_cache();
      }
      // A write to any address ends value forwarding, because the address
      // arithmetic behind a derived pointer is not tracked here.
      if (op.kind == tir::OpKind::Store) forwarded_root_values_.clear();
      if (!translate_op(op)) return false;
    }
    attach_cold_blocks();
    flush_gc_root_registrations();
    return true;
  }

  // There is deliberately no handoff-fusion pass. The lowering always publishes
  // a pinned result before it releases the pin (`root_store` then `unpin`), while
  // the fused runtime entry `nrq_gc_root_store_release` performs that same order
  // in one call. A pass that rewrote publish-then-release into the fused call
  // would have to be reordered and re-proven first, and the previous pass matched
  // the opposite order and therefore fired on no emitted code (see
  // `reports/performance/bo-03-handoff-fusion-audit.md`). The runtime entry stays
  // available for callers that hand a pending release over explicitly.
 private:
  mlir::Value get(tir::Value v) const {
    if (!v.is_valid()) return {};
    return values_[static_cast<std::size_t>(v.id)];
  }

  // Alignment of every pointer whose origin this function knows: frame slots,
  // stack-placed array storage and the element pointers derived from them. A
  // pointer with no entry has unknown provenance and keeps the conservative
  // default, because a value loaded from memory or received as a parameter can
  // point anywhere.
  void record_pointer_alignment(const tir::Value& value, uint64_t alignment) {
    if (!value.is_valid() || alignment <= 1) return;
    pointer_alignment_[value.id] = alignment;
  }

  uint64_t known_pointer_alignment(const tir::Value& value) const {
    if (!value.is_valid()) return 1;
    const auto found = pointer_alignment_.find(value.id);
    return found == pointer_alignment_.end() ? 1 : found->second;
  }

  // States the alignment of a memory access. A `T`-typed location is always
  // naturally aligned: the language has no pointer casts, type punning, unions
  // or FFI, so the compiler and the runtime are the only creators of typed
  // storage, and both lay aggregates out with natural alignment. That makes the
  // accessed type's alignment a valid floor, which the default the LLVM dialect
  // would otherwise assume (4 bytes) is not. Provenance can only raise it: a
  // slot is aligned more generously than its type needs.
  template <typename AccessOp>
  void apply_access_alignment(AccessOp access, const tir::Value& pointer,
                              const Type& accessed_type) {
    const uint64_t natural = std::max<uint64_t>(1, type_alignment(accessed_type));
    const uint64_t known = known_pointer_alignment(pointer);
    const uint64_t alignment = std::max(natural, known);
    if (alignment > 1) access.setAlignment(alignment);
  }

  // `--fast-math` opts into reassociation and FMA contraction for floating-point
  // arithmetic. Strict IEEE ordering stays the default, because reassociation
  // changes the result of a sum. With the flag LLVM may reassociate an
  // accumulation, which is the difference between a scalar and a vectorized
  // floating-point reduction.
  //
  // The flags are passed to the op's builder rather than set as a raw attribute:
  // MLIR keeps them in the op's properties, and a plain "fastmath" attribute is
  // printed but ignored by the LLVM translation.
  template <typename OpTy>
  mlir::Value create_fp_binary(mlir::Location loc, mlir::Value lhs,
                               mlir::Value rhs) {
    if (!fast_math_) return builder_.create<OpTy>(loc, lhs, rhs).getResult();
    const mlir::LLVM::FastmathFlags flags =
        mlir::LLVM::FastmathFlags::reassoc | mlir::LLVM::FastmathFlags::contract;
    return builder_.create<OpTy>(loc, lhs, rhs, flags).getResult();
  }

  void set(tir::Value v, mlir::Value mlir_val) {
    if (!v.is_valid()) return;
    if (v.ownership != ValueOwnership::Trivial && mlir_val.getDefiningOp())
      mlir_val.getDefiningOp()->setAttr(
          "nirqoba.ownership",
          builder_.getStringAttr(ownership_spelling(v.ownership)));
    values_[static_cast<std::size_t>(v.id)] = mlir_val;
  }

  bool is_backedge(int label_id) const {
    const auto target = label_positions_.find(label_id);
    return target != label_positions_.end() &&
           target->second < current_operation_index_;
  }

  // Bounds checks used to be ordinary runtime calls on every array access.
  // That prevented the loop vectorizer from seeing the loop body and forced a
  // call/return pair per element. Emit the comparison inline and branch to a
  // cold noreturn failure block instead; LLVM can then fold a check that is
  // implied by the loop condition, or version/vectorize the loop around it.
  void emit_bounds_guard(mlir::Value index, mlir::Value length,
                         mlir::Location loc) {
    const mlir::Type i64 = builder_.getI64Type();
    if (index.getType() != i64) {
      const unsigned bits = index.getType().getIntOrFloatBitWidth();
      index = bits < 64
          ? builder_.create<mlir::LLVM::ZExtOp>(loc, i64, index).getResult()
          : builder_.create<mlir::LLVM::TruncOp>(loc, i64, index).getResult();
    }
    mlir::Value in_bounds = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::ult, index, length).getResult();

    mlir::Block* continue_block = func_op_.addBlock();
    builder_.create<mlir::LLVM::CondBrOp>(loc, in_bounds, continue_block,
                                           bounds_fail_block());
    builder_.setInsertionPointToStart(continue_block);
  }

  // Fresh storage a promoted callee returns has two possible homes: the arena the
  // caller created when it bounds the result, or the general allocator when the
  // caller passes null because the result leaves its frame. The handle is checked
  // here at runtime, so one callee body serves both kinds of call site and a
  // single escaping caller no longer denies the arena to every other caller. Both
  // branches produce the pointer the function returns, so nothing downstream has
  // to know which one ran.
  mlir::Value allocate_for_returned_storage(mlir::Value handle, mlir::Value bytes,
                                            mlir::Value alignment,
                                            std::size_t alignment_bytes,
                                            mlir::Location loc) {
    auto arena_fn = ensure_runtime_fn(
        module_, builder_, "nrq_cdmm_arena_allocate", llvm_ptr_type(ctx_),
        {llvm_ptr_type(ctx_), builder_.getI64Type(), builder_.getI64Type()});
    auto general_fn = ensure_runtime_fn(
        module_, builder_, "nrq_memory_allocate", llvm_ptr_type(ctx_),
        {builder_.getI64Type(), builder_.getI64Type()});
    mark_fresh_allocation(arena_fn, alignment_bytes);
    mark_fresh_allocation(general_fn, alignment_bytes);

    if (activation_handle_guaranteed_) {
      // Every caller passes a handle, so the null arm is unreachable: this
      // activation fills the chunk its caller created and never reaches for the
      // general allocator.
      auto arena_only = builder_.create<mlir::LLVM::CallOp>(
          loc, arena_fn, mlir::ValueRange{handle, bytes, alignment});
      return arena_only.getResult();
    }

    const mlir::Type ptr = llvm_ptr_type(ctx_);
    mlir::Value missing =
        builder_.create<mlir::LLVM::ZeroOp>(loc, ptr).getResult();
    mlir::Value absent = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::eq, handle, missing).getResult();

    mlir::Block* arena_block = func_op_.addBlock();
    mlir::Block* general_block = func_op_.addBlock();
    mlir::Block* merge_block = func_op_.addBlock();
    mlir::Value merged = merge_block->addArgument(ptr, loc);
    builder_.create<mlir::LLVM::CondBrOp>(loc, absent, general_block,
                                          arena_block);

    builder_.setInsertionPointToStart(arena_block);
    auto arena_call = builder_.create<mlir::LLVM::CallOp>(
        loc, arena_fn, mlir::ValueRange{handle, bytes, alignment});
    builder_.create<mlir::LLVM::BrOp>(loc,
                                      mlir::ValueRange{arena_call.getResult()},
                                      merge_block);

    builder_.setInsertionPointToStart(general_block);
    auto general_call = builder_.create<mlir::LLVM::CallOp>(
        loc, general_fn, mlir::ValueRange{bytes, alignment});
    builder_.create<mlir::LLVM::BrOp>(loc,
                                      mlir::ValueRange{general_call.getResult()},
                                      merge_block);

    builder_.setInsertionPointToStart(merge_block);
    return merged;
  }

  // A promoted callee can reach a release of the storage it returns: a path that
  // returns something else still drops the binding. The arena owns that storage
  // only when the caller supplied a handle, so the release is emitted on the null
  // path alone; the arena's own destroy releases the rest.
  void emit_release_unless_arena(mlir::Value pointer, const Type& payload,
                                 mlir::Location loc) {
    if (caller_arena_arg_index_ < 0) return;
    // No caller passes null, so there is no general-allocator arm to release:
    // every path that reaches here leaves the storage to the caller's arena.
    if (activation_handle_guaranteed_) return;
    const mlir::Type ptr = llvm_ptr_type(ctx_);
    mlir::Value coerced = pointer;
    if (!mlir::isa<mlir::LLVM::LLVMPointerType>(coerced.getType()))
      coerced = builder_.create<mlir::LLVM::IntToPtrOp>(loc, ptr, coerced)
                    .getResult();
    mlir::Value handle = func_op_.getArgument(
        static_cast<unsigned>(caller_arena_arg_index_));
    mlir::Value missing =
        builder_.create<mlir::LLVM::ZeroOp>(loc, ptr).getResult();
    mlir::Value absent = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::eq, handle, missing).getResult();

    mlir::Block* release_block = func_op_.addBlock();
    mlir::Block* continue_block = func_op_.addBlock();
    builder_.create<mlir::LLVM::CondBrOp>(loc, absent, release_block,
                                          continue_block);

    builder_.setInsertionPointToStart(release_block);
    emit_gc_root_unregistration(coerced, payload, loc);
    auto release_fn = ensure_runtime_fn(
        module_, builder_, "nrq_memory_deallocate_checked",
        mlir::LLVM::LLVMVoidType::get(ctx_), {ptr});
    builder_.create<mlir::LLVM::CallOp>(loc, release_fn,
                                        mlir::ValueRange{coerced});
    builder_.create<mlir::LLVM::BrOp>(loc, continue_block);

    builder_.setInsertionPointToStart(continue_block);
  }

  // One cold failure block serves every bounds check in the function. The
  // message, call and unreachable are identical for all of them, and giving each
  // check its own block tripled the block count of bounds-checked code, which
  // then had to be laid out, verified and merged by every later pass. The block
  // is built on first use and attached at the end of the body, so the hot blocks
  // stay contiguous.
  mlir::Block* bounds_fail_block() {
    if (bounds_fail_block_) return bounds_fail_block_;
    mlir::Block* block = new mlir::Block();
    bounds_fail_block_ = block;
    mlir::OpBuilder::InsertionGuard guard(builder_);
    builder_.setInsertionPointToStart(block);
    const mlir::Location loc = mlir::UnknownLoc::get(ctx_);
    auto fail_fn = ensure_runtime_fn(
        module_, builder_, "nrq_memory_array_bounds_fail",
        mlir::LLVM::LLVMVoidType::get(ctx_), {});
    fail_fn.setNoreturn(true);
    fail_fn.setCold(true);
    fail_fn.setNoUnwind(true);
    builder_.create<mlir::LLVM::CallOp>(loc, fail_fn, mlir::ValueRange{});
    builder_.create<mlir::LLVM::UnreachableOp>(loc);
    return block;
  }

  // Cold blocks are built detached so they can be appended once the whole body
  // exists; a block has to live in the function's region to be a branch target.
  void attach_cold_blocks() {
    if (bounds_fail_block_) func_op_.getBody().push_back(bounds_fail_block_);
    for (mlir::Block* block : cold_alloc_blocks_) {
      func_op_.getBody().push_back(block);
    }
    cold_alloc_blocks_.clear();
  }

  // Back edges poll the safepoint so another thread can snapshot this thread's
  // roots. Polling on every crossing made an allocation-free loop pay a runtime
  // call per iteration, so the poll is throttled by a scalar counter kept in an
  // entry-block slot that counts down: the loop carries a subtract, a compare
  // and a store, and the call runs once every kSafepointInterval crossings, where
  // the countdown is reloaded on the polled path only. A collection requested by
  // another thread is therefore observed within that many iterations, which
  // bounds the pause latency without paying for a call per iteration.
  static constexpr int64_t kSafepointInterval = 64;

  // A back edge with one target: poll in a block of its own on the way there, so
  // nothing is added between the branch and the loop header. Returns false when
  // the function needs no poll at all, and then the caller emits the plain
  // branch itself.
  bool emit_gc_safepoint(mlir::Location loc, mlir::Block* target) {
    if (!uses_gc_mutator_) return false;
    mlir::Value due = safepoint_due(loc);
    mlir::Block* poll_block = func_op_.addBlock();
    builder_.create<mlir::LLVM::CondBrOp>(loc, due, poll_block, target);
    builder_.setInsertionPointToStart(poll_block);
    emit_safepoint_call(loc);
    reload_safepoint_counter(loc);
    builder_.create<mlir::LLVM::BrOp>(loc, target);
    return true;
  }

  // A back edge that may take either of two targets: both have to poll, so the
  // guard runs first and the caller branches from the returned block.
  void emit_gc_safepoint(mlir::Location loc) {
    if (!uses_gc_mutator_) return;
    mlir::Value due = safepoint_due(loc);
    mlir::Block* poll_block = func_op_.addBlock();
    mlir::Block* continue_block = func_op_.addBlock();
    builder_.create<mlir::LLVM::CondBrOp>(loc, due, poll_block, continue_block);
    builder_.setInsertionPointToStart(poll_block);
    emit_safepoint_call(loc);
    reload_safepoint_counter(loc);
    builder_.create<mlir::LLVM::BrOp>(loc, continue_block);
    builder_.setInsertionPointToStart(continue_block);
  }

  // Advances the counter and reports whether this crossing is the one that
  // polls. The counter is an entry-block slot, so LLVM promotes it to a register
  // and the loop carries no memory traffic that mem2reg cannot remove.
  mlir::Value safepoint_due(mlir::Location loc) {
    mlir::Value counter = safepoint_counter_slot(loc);
    const mlir::Type i64 = builder_.getI64Type();
    auto constant = [&](int64_t value) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, i64, builder_.getI64IntegerAttr(value)).getResult();
    };
    mlir::Value current = builder_.create<mlir::LLVM::LoadOp>(
        loc, i64, counter).getResult();
    mlir::Value next = builder_.create<mlir::LLVM::SubOp>(
        loc, current, constant(1)).getResult();
    builder_.create<mlir::LLVM::StoreOp>(loc, next, counter);
    // Signed less-or-equal, not equality: a counter that reached zero without
    // taking the polled path, or wrapped past it, would never compare equal again
    // and the loop would stop observing collection requests for good. A bound
    // has to hold for every counter value, not just the ones this emission
    // intended.
    return builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::sle, next, constant(0)).getResult();
  }

  // Reloads the countdown so the next poll is again kSafepointInterval crossings
  // away. Only the polled path runs this, which is one crossing in the interval.
  void reload_safepoint_counter(mlir::Location loc) {
    mlir::Value counter = safepoint_counter_slot(loc);
    mlir::Value interval = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI64Type(),
        builder_.getI64IntegerAttr(kSafepointInterval)).getResult();
    builder_.create<mlir::LLVM::StoreOp>(loc, interval, counter);
  }

  void emit_safepoint_call(mlir::Location loc) {
    auto safepoint = ensure_runtime_fn(
        module_, builder_, "nrq_gc_safepoint",
        mlir::LLVM::LLVMVoidType::get(ctx_), {});
    builder_.create<mlir::LLVM::CallOp>(
        loc, safepoint, mlir::ValueRange{});
  }

  mlir::Value safepoint_counter_slot(mlir::Location loc) {
    if (safepoint_counter_) return safepoint_counter_;
    const mlir::Type i64 = builder_.getI64Type();
    mlir::Value slot = alloca_at_entry(i64, loc);
    mlir::OpBuilder::InsertionGuard guard(builder_);
    builder_.setInsertionPointAfter(slot.getDefiningOp());
    // The counter counts down, so a fresh activation starts one interval away
    // from its first poll.
    mlir::Value initial = builder_.create<mlir::LLVM::ConstantOp>(
        loc, i64, builder_.getI64IntegerAttr(kSafepointInterval)).getResult();
    builder_.create<mlir::LLVM::StoreOp>(loc, initial, slot);
    safepoint_counter_ = slot;
    return slot;
  }

  // Every stack slot is allocated in the function's entry block, never at the
  // point of the `tir.alloca`. A loop body re-executes its instructions, and an
  // alloca executed per iteration grows the stack without bound until the
  // process dies; hoisting also lets the backend fold the frame into one
  // prologue adjustment.
  mlir::Value alloca_at_entry(mlir::Type element_type, mlir::Location loc) {
    if (entry_block_ == nullptr) return {};
    mlir::OpBuilder::InsertionGuard guard(builder_);
    builder_.setInsertionPointToStart(entry_block_);
    mlir::Value one = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI64Type(), builder_.getI64IntegerAttr(1)).getResult();
    return builder_.create<mlir::LLVM::AllocaOp>(
        loc, llvm_ptr_type(ctx_), element_type, one).getResult();
  }

  // Automated strategy selection (Phase Psi) shares storage between the sites
  // that can share it: the scratch arena of a scope for bounded sites, and one
  // pool per layout group of the repeated ones. Each region is created where its
  // scope begins and destroyed on the edges out of that scope, so a loop that
  // allocates per iteration reclaims at its own exit instead of holding its
  // storage until the frame ends.
  //
  // Only the create moves with the scope. The handle always lives in a frame
  // slot, because an alloca inside a loop body would grow the stack once per
  // iteration, and a load per site keeps that slot the only place it is published.
  static constexpr int64_t kScratchArenaBytes = 4096;

  // Slots per chunk for a bulk pool. The runtime defaults to a chunk of at most
  // 64 KiB, and eight slots is where a chunk stops being grown for a pool whose
  // payloads are ordinary working-set sizes.
  static constexpr int64_t kPoolSlotsPerChunk = 8;

  // The frame's regions are created once, in the entry block, beside the slot
  // that publishes their handles.
  void prepare_frame_regions() {
    if (entry_block_ == nullptr) return;
    for (std::size_t index = 0; index < regions_.size(); ++index)
      if (!regions_[index].scope.loop) emit_region_create(index);
  }

  mlir::Value region_handle_slot(std::size_t index, mlir::Location loc) {
    if (index >= region_slots_.size()) return {};
    if (region_slots_[index].handle_slot) return region_slots_[index].handle_slot;
    region_slots_[index].handle_slot = alloca_at_entry(llvm_ptr_type(ctx_), loc);
    return region_slots_[index].handle_slot;
  }

  // Creates one region and publishes its handle in its slot. The caller's
  // insertion point is the scope's entry: the entry block for a frame region, or
  // the edge into the loop header for a loop region.
  void emit_region_create(std::size_t index) {
    if (index >= regions_.size() || entry_block_ == nullptr) return;
    const RegionSelection& region = regions_[index];
    const mlir::Location loc = func_op_.getLoc();
    mlir::Type pointer = llvm_ptr_type(ctx_);
    mlir::Value slot = region_handle_slot(index, loc);
    if (!slot) return;
    mlir::OpBuilder::InsertionGuard guard(builder_);
    if (!region.scope.loop) {
      if (mlir::Operation* slot_op = slot.getDefiningOp())
        builder_.setInsertionPointAfter(slot_op);
    }
    auto constant = [&](mlir::Type type, int64_t value) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, type, builder_.getIntegerAttr(type, value)).getResult();
    };
    mlir::LLVM::CallOp create;
    if (region.pooled) {
      auto create_fn = ensure_runtime_fn(
          module_, builder_, "nrq_cdmm_pool_create", pointer,
          {builder_.getI64Type(), builder_.getI64Type(), builder_.getI64Type(),
           builder_.getI32Type(), pointer, pointer});
      mlir::Value null = builder_.create<mlir::LLVM::ZeroOp>(loc, pointer).getResult();
      create = builder_.create<mlir::LLVM::CallOp>(
          loc, create_fn,
          mlir::ValueRange{
              constant(builder_.getI64Type(),
                       static_cast<int64_t>(region.slot_bytes)),
              constant(builder_.getI64Type(),
                       static_cast<int64_t>(region.alignment)),
              constant(builder_.getI64Type(), kPoolSlotsPerChunk),
              // No generation words: a pooled slot never outlives the pool, so a
              // stale pointer cannot be validated by a generation either.
              constant(builder_.getI32Type(), 0), null, null});
    } else {
      auto create_fn = ensure_runtime_fn(
          module_, builder_, "nrq_cdmm_arena_create", pointer,
          {builder_.getI64Type(), builder_.getI64Type()});
      create = builder_.create<mlir::LLVM::CallOp>(
          loc, create_fn,
          mlir::ValueRange{
              constant(builder_.getI64Type(), kScratchArenaBytes),
              // No cap: a bounded site whose data outgrows the first chunk grows
              // the arena instead of failing, exactly as the general allocator
              // would.
              constant(builder_.getI64Type(), 0)});
    }
    // Names the decision for the pass that audits it and for anyone reading the
    // emitted IR: a `loop` create sits in the loop's preheader, a `frame` one in
    // the entry block.
    create->setAttr("nirqoba.region_scope",
                    builder_.getStringAttr(region.scope.loop ? "loop" : "frame"));
    builder_.create<mlir::LLVM::StoreOp>(loc, create.getResult(), slot)
        .setAlignment(8);
  }

  // Reads one region's handle out of its slot at the point of use.
  mlir::Value region_handle(std::size_t index, mlir::Location loc) {
    if (index >= region_slots_.size() || !region_slots_[index].handle_slot)
      return {};
    auto load = builder_.create<mlir::LLVM::LoadOp>(
        loc, llvm_ptr_type(ctx_), region_slots_[index].handle_slot);
    load.setAlignment(8);
    return load.getResult();
  }

  // Destroys one region where the caller has reached: a loop's exit edge, or the
  // return that ends its frame.
  void emit_region_destroy(std::size_t index, mlir::Location loc) {
    if (index >= regions_.size()) return;
    mlir::Value handle = region_handle(index, loc);
    if (!handle) return;
    auto destroy_fn = ensure_runtime_fn(
        module_, builder_,
        regions_[index].pooled ? "nrq_cdmm_pool_destroy"
                               : "nrq_cdmm_arena_destroy",
        builder_.getI32Type(), {llvm_ptr_type(ctx_)});
    builder_.create<mlir::LLVM::CallOp>(loc, destroy_fn,
                                        mlir::ValueRange{handle});
  }

  // Registers one teardown per managed field of storage the arena owns. The
  // address is the compiler's to compute: it knows where the field's pointer word
  // sits, and the runtime only has to load that word and hand it to the release.
  // Registration follows the allocation because the object's address is the
  // allocation's result.
  void emit_arena_field_teardowns(mlir::Value handle, mlir::Value object,
                                  const Type& payload, mlir::Location loc,
                                  bool pool_slot = false) {
    if (!handle || !object) return;
    // An allocation's result is a pointer to the payload, and it is the payload's
    // fields the teardown table describes.
    const Type& storage_type =
        payload.is_pointer() ? payload.element_type() : payload;
    if (!storage_type.is_struct() || storage_type.struct_name().empty()) return;
    const auto found =
        payload_field_releases_.find(storage_type.struct_name());
    if (found == payload_field_releases_.end() ||
        found->second.fields.empty())
      return;
    mlir::Type payload_type = tir_type_to_mlir(storage_type, ctx_, builder_);
    if (!payload_type) return;
    const mlir::Type ptr = llvm_ptr_type(ctx_);
    auto constant32 = [&](int64_t value) {
      return builder_
          .create<mlir::LLVM::ConstantOp>(loc, builder_.getI32Type(),
                                          builder_.getI32IntegerAttr(value))
          .getResult();
    };
    for (const std::pair<int, std::string>& field : found->second.fields) {
      // The registry names a release by a small kind, not by a function pointer:
      // the set of releases the language defines is closed, so one destructor
      // reads the kind and calls the right one.
      int64_t kind = 0;
      if (field.second == "nrq_ffi_free")
        kind = 1;
      else if (field.second == "file.free_text")
        kind = 2;
      else if (field.second == "nrq_array_free")
        kind = 3;
      else if (field.second == "array.owned_release")
        kind = 4;
      else if (field.second == "file.close")
        kind = 5;
      if (kind == 0) continue;
      // The pack position is the field index, exactly as the pack itself was
      // lowered, so both halves of the layout agree.
      const int64_t slot = field.first;
      if (slot < 0) continue;
      mlir::Value address = builder_
          .create<mlir::LLVM::GEPOp>(loc, ptr, payload_type, object,
                                     mlir::ValueRange{constant32(0),
                                                      constant32(slot)})
          .getResult();
      // The slot is at least eight-byte aligned, so the kind fits in the three
      // low bits of the encoded context without displacing part of the address.
      mlir::Value encoded = builder_
          .create<mlir::LLVM::PtrToIntOp>(loc, builder_.getI64Type(), address)
          .getResult();
      encoded = builder_
          .create<mlir::LLVM::OrOp>(
              loc, encoded,
              builder_
                  .create<mlir::LLVM::ConstantOp>(
                      loc, builder_.getI64Type(),
                      builder_.getI64IntegerAttr(kind))
                  .getResult())
          .getResult();
      mlir::Value context = builder_
          .create<mlir::LLVM::IntToPtrOp>(loc, ptr, encoded)
          .getResult();
      // A pool slot is reused, so its registry runs the teardown when the slot is
      // released rather than when the pool dies; the record itself is the same.
      auto register_fn = ensure_runtime_fn(
          module_, builder_,
          pool_slot ? "nrq_cdmm_pool_register_teardown"
                    : "nrq_cdmm_arena_register_teardown",
          builder_.getI32Type(), {ptr, ptr, ptr});
      builder_.create<mlir::LLVM::CallOp>(
          loc, register_fn, mlir::ValueRange{handle, object, context});
    }
  }

  // Emits the create for every loop-scoped region whose header this edge enters
  // from outside the loop. The classifier picked the header as the single entry
  // edge, so this is the one place the create belongs.
  void emit_loop_header_creates(int header_label) {
    const auto position = label_positions_.find(header_label);
    if (position == label_positions_.end()) return;
    const auto seated = loop_regions_at_header_.find(position->second);
    if (seated == loop_regions_at_header_.end()) return;
    for (std::size_t region : seated->second) emit_region_create(region);
  }

  // True when an edge from this operation to that label leaves a loop that hosts
  // a region containing this operation. An unknown label counts as an exit:
  // destroying early is safe, destroying late is not.
  bool label_leaves_loop(std::size_t index, int label) const {
    const auto found = label_positions_.find(label);
    if (found == label_positions_.end()) return true;
    const std::size_t target = found->second;
    for (const RegionSelection& region : regions_) {
      if (!region.scope.loop) continue;
      if (index < region.scope.begin || index > region.scope.end) continue;
      if (target < region.scope.begin || target > region.scope.end) return true;
    }
    return false;
  }

  // Destroys every loop-scoped region this operation leaves. `targets` names the
  // edges; an empty list means the operation itself ends the scope (a return).
  void emit_loop_region_destroys(std::size_t index, mlir::Location loc,
                                 std::vector<int> targets) {
    std::vector<std::size_t> dying;
    for (std::size_t region = 0; region < regions_.size(); ++region) {
      const RegionScope& scope = regions_[region].scope;
      if (!scope.loop || index < scope.begin || index > scope.end) continue;
      bool leaves = targets.empty();
      for (int label : targets) {
        const auto found = label_positions_.find(label);
        const std::size_t target =
            found == label_positions_.end() ? index + 1 : found->second;
        if (target < scope.begin || target > scope.end) {
          leaves = true;
          break;
        }
      }
      if (leaves) dying.push_back(region);
    }
    if (dying.empty()) return;
    // Innermost first: a region created inside another must die before it.
    std::sort(dying.begin(), dying.end(),
              [&](std::size_t left, std::size_t right) {
                return regions_[left].scope.begin > regions_[right].scope.begin;
              });
    for (std::size_t region : dying) emit_region_destroy(region, loc);
  }

  // A conditional edge cannot carry the destroy inline, because the other edge
  // may stay in the loop. This builds the block the leaving edge jumps to: the
  // destroys, then the branch to the edge's real target.
  mlir::Block* destroy_edge_block(mlir::Location loc, mlir::Block* target,
                                  int label) {
    mlir::Block* block = func_op_.addBlock();
    mlir::OpBuilder::InsertionGuard guard(builder_);
    builder_.setInsertionPointToStart(block);
    emit_loop_region_destroys(current_operation_index_, loc, {label});
    builder_.create<mlir::LLVM::BrOp>(loc, target);
    return block;
  }

  // Root registration is likewise a per-activation action: the same slot must
  // The container's half of the element contract: a kind whose values own
  // storage maps to the runtime call that releases one element, and an empty
  // result means the element is trivial and the buffer alone is the allocation.
  // Sema admits exactly the kinds with an entry here.
  static std::string element_release_symbol(const Type& element_type) {
    if (element_type.is_owned_string()) return "nrq_file_text_release";
    return {};
  }

  static bool element_owns_storage(const Type& type) {
    return type.is_owned_string() || type.is_owned_pointer() ||
           type.is_owned_array() || type.is_soa() ||
           type.is_dynamic_array() || type.is_shared_pointer() ||
           type.is_managed_pointer();
  }

  // Destroys the owned elements of a container before its buffer is released.
  //
  // The loop runs backwards from `length`, not from `capacity`: the slots past
  // the length were never written, so reading them would release garbage, and
  // reverse order tears a partially filled buffer down from the most recently
  // stored element. The counter lives in a slot because it is carried across the
  // back edge, and the loop is emitted where the function already has a frame.
  void emit_element_teardown_loop(mlir::Value buffer, mlir::Value length,
                                  const Type& element_type,
                                  mlir::Type element_mlir,
                                  const std::string& release_symbol,
                                  SourceLocation source_location,
                                  mlir::Location loc) {
    const mlir::Type i64 = builder_.getI64Type();
    const mlir::Type ptr = llvm_ptr_type(ctx_);
    auto constant = [&](int64_t value) {
      return builder_
          .create<mlir::LLVM::ConstantOp>(
              loc, i64, builder_.getI64IntegerAttr(value))
          .getResult();
    };
    mlir::Value counter = alloca_at_entry(i64, loc);
    if (!counter) {
      diag_.error("NRQ4005", source_location,
                  "internal: element teardown needs a function entry block");
      return;
    }
    builder_.create<mlir::LLVM::StoreOp>(loc, length, counter);

    mlir::Block* head = func_op_.addBlock();
    mlir::Block* body = func_op_.addBlock();
    mlir::Block* done = func_op_.addBlock();
    builder_.create<mlir::LLVM::BrOp>(loc, head);

    builder_.setInsertionPointToStart(head);
    mlir::Value stored = builder_.create<mlir::LLVM::LoadOp>(
        loc, i64, counter).getResult();
    mlir::Value remaining = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::ugt, stored, constant(0)).getResult();
    builder_.create<mlir::LLVM::CondBrOp>(loc, remaining, body, done);

    builder_.setInsertionPointToStart(body);
    mlir::Value index = builder_.create<mlir::LLVM::SubOp>(
        loc, stored, constant(1)).getResult();
    builder_.create<mlir::LLVM::StoreOp>(loc, index, counter);
    mlir::Value slot = builder_
        .create<mlir::LLVM::GEPOp>(loc, ptr, element_mlir, buffer,
                                   mlir::ValueRange{index})
        .getResult();
    auto element = builder_.create<mlir::LLVM::LoadOp>(loc, element_mlir, slot);
    // The element sits at `index * stride`, so it is aligned whenever the buffer
    // is, which the allocator guarantees.
    const uint64_t element_alignment =
        std::max<uint64_t>(1, element_type.alignment());
    if (element_alignment > 1) element.setAlignment(element_alignment);
    auto release = ensure_runtime_fn(
        module_, builder_, release_symbol,
        mlir::LLVM::LLVMVoidType::get(ctx_), {element_mlir});
    builder_.create<mlir::LLVM::CallOp>(loc, release,
                                        mlir::ValueRange{element.getResult()});
    builder_.create<mlir::LLVM::BrOp>(loc, head);

    builder_.setInsertionPointToStart(done);
  }

  // Destroying an aggregate walks its physical fields in reverse declaration
  // order. The recursion carries a path of aggregate indices, so one extract per
  // leaf is enough; no per-traversal callable is built, which keeps destruction
  // allocation-free even for deeply nested aggregates.
  void drop_aggregate_fields(const Type& type, mlir::Value aggregate,
                             llvm::SmallVectorImpl<int64_t>& path,
                             mlir::Location loc) {
    auto extract = [&](mlir::Value value, llvm::ArrayRef<int64_t> indices) {
      return builder_
          .create<mlir::LLVM::ExtractValueOp>(
              loc, value, builder_.getDenseI64ArrayAttr(indices))
          .getResult();
    };
    auto unregister_root = [&](mlir::Value data) {
      auto unregister = ensure_runtime_fn(
          module_, builder_, "nrq_gc_root_unregister",
          mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
      builder_.create<mlir::LLVM::CallOp>(loc, unregister,
                                           mlir::ValueRange{data});
    };

    if (type.is_soa()) {
      // A structure-of-arrays container is one collector object. Releasing the
      // aggregate drops its root; the collector reclaims the storage later.
      unregister_root(extract(aggregate, path));
      return;
    }
    if (type.is_shared_pointer()) {
      mlir::Value data = extract(aggregate, path);
      auto release = ensure_runtime_fn(
          module_, builder_, "nrq_memory_shared_release",
          builder_.getI32Type(), {llvm_ptr_type(ctx_)});
      builder_.create<mlir::LLVM::CallOp>(loc, release,
                                           mlir::ValueRange{data});
      return;
    }
    if (type.is_owned_pointer()) {
      mlir::Value data = extract(aggregate, path);
      if (type.is_resource_pointer()) {
        // A domain handle is not collector storage: release it with its runtime
        // destructor. Every one of these accepts null, which is what an
        // uninitialized or moved-out field holds.
        const std::string& domain = type.resource_domain();
        std::string symbol;
        mlir::Type result = mlir::LLVM::LLVMVoidType::get(ctx_);
        if (domain == "file") {
          symbol = "nrq_file_close_handle";
          result = builder_.getI32Type();
        } else if (domain == "socket") {
          symbol = "nrq_socket_close_handle";
          result = builder_.getI32Type();
        } else if (domain == "mutex") {
          symbol = "nrq_mutex_destroy";
        } else if (domain == "mapping") {
          symbol = "nrq_memory_unmap_file";
          result = builder_.getI32Type();
        } else if (domain == "secure") {
          symbol = "nrq_memory_secure_release";
        }
        if (!symbol.empty()) {
          auto release = ensure_runtime_fn(
              module_, builder_, symbol, result, {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, release,
                                               mlir::ValueRange{data});
          return;
        }
      }
      if (release_payload_has_managed_reference(type)) unregister_root(data);
      auto release = ensure_runtime_fn(
          module_, builder_, "nrq_memory_deallocate_checked",
          mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
      builder_.create<mlir::LLVM::CallOp>(loc, release,
                                           mlir::ValueRange{data});
      return;
    }
    if (type.is_owned_array()) {
      mlir::Value descriptor = extract(aggregate, path);
      mlir::Value data = extract(descriptor, {0});
      if (release_payload_has_managed_reference(type)) unregister_root(data);
      auto release = ensure_runtime_fn(
          module_, builder_, "nrq_memory_deallocate_checked",
          mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
      builder_.create<mlir::LLVM::CallOp>(loc, release,
                                           mlir::ValueRange{data});
      return;
    }

    auto visit = [&](const Type& field_type, int64_t index) {
      path.push_back(index);
      if (field_type.is_soa() || field_type.is_owned_array() ||
          field_type.is_owned_pointer() || field_type.is_shared_pointer() ||
          field_type.is_array() || field_type.is_struct() ||
          field_type.is_tuple())
        drop_aggregate_fields(field_type, aggregate, path, loc);
      path.pop_back();
    };
    // Fields are destroyed in reverse declaration order, matching the LIFO
    // discipline used for scopes and parameters.
    if (type.is_array()) {
      for (std::size_t index = type.array_length(); index-- > 0;)
        visit(type.element_type(), static_cast<int64_t>(index));
    } else if (type.is_struct()) {
      for (std::size_t index = type.struct_fields().size(); index-- > 0;)
        visit(type.struct_fields()[index].second, static_cast<int64_t>(index));
    } else if (type.is_tuple()) {
      for (std::size_t index = type.tuple_elements().size(); index-- > 0;)
        visit(type.tuple_elements()[index], static_cast<int64_t>(index));
    }
  }

  // Copying an aggregate retains every shared pointer field it carries, in
  // declaration order. Like destruction this is a plain recursion, not a
  // heap-allocated callable.
  void retain_aggregate_fields(const Type& type, mlir::Value aggregate,
                               llvm::SmallVectorImpl<int64_t>& path,
                               mlir::Location loc) {
    auto extract = [&](mlir::Value value, llvm::ArrayRef<int64_t> indices) {
      return builder_
          .create<mlir::LLVM::ExtractValueOp>(
              loc, value, builder_.getDenseI64ArrayAttr(indices))
          .getResult();
    };
    if (type.is_shared_pointer()) {
      mlir::Value data = extract(aggregate, path);
      auto retain = ensure_runtime_fn(
          module_, builder_, "nrq_memory_shared_retain",
          llvm_ptr_type(ctx_), {llvm_ptr_type(ctx_)});
      builder_.create<mlir::LLVM::CallOp>(loc, retain,
                                           mlir::ValueRange{data});
      return;
    }
    auto visit = [&](const Type& field_type, int64_t index) {
      path.push_back(index);
      if (field_type.is_shared_pointer() || field_type.is_array() ||
          field_type.is_struct() || field_type.is_tuple())
        retain_aggregate_fields(field_type, aggregate, path, loc);
      path.pop_back();
    };
    if (type.is_array()) {
      for (std::size_t index = 0; index < type.array_length(); ++index)
        visit(type.element_type(), static_cast<int64_t>(index));
    } else if (type.is_struct()) {
      for (std::size_t index = 0; index < type.struct_fields().size(); ++index)
        visit(type.struct_fields()[index].second, static_cast<int64_t>(index));
    } else if (type.is_tuple()) {
      for (std::size_t index = 0; index < type.tuple_elements().size(); ++index)
        visit(type.tuple_elements()[index], static_cast<int64_t>(index));
    }
  }

  // A container allocation publishes its object through a root slot. When the
  // language declares a managed local and immediately initialises it with that
  // object, the local is the better publish target: the activation then carries
  // one root slot for the object instead of two, which is one frame entry, one
  // cleared slot and one publish store fewer for every activation that allocates.
  //
  // The substitution is only sound for a fresh slot: declared in the same
  // straight-line block right after the allocation and initialised immediately,
  // in a block no branch targets. A slot an earlier iteration had already
  // written would hold an older reference, and the allocation publishes with a
  // plain store inside its own barrier region instead of going through the store
  // barrier that would retain that older reference.
  mlir::Value coalesced_allocation_root(const tir::Op& call_op,
                                        mlir::Location loc) {
    const std::vector<tir::Op>& operations = tir_fn_.operations();
    const std::size_t call_index = current_operation_index_;
    if (call_index + 2 >= operations.size()) return {};
    const tir::Op& declaration = operations[call_index + 1];
    const tir::Op& initialisation = operations[call_index + 2];
    if (declaration.kind != tir::OpKind::Alloca) return {};
    if (initialisation.kind != tir::OpKind::Store) return {};
    if (initialisation.lhs.id != declaration.result.id) return {};
    if (initialisation.operand.id != call_op.result.id) return {};
    if (!declaration.result.type.is_managed_pointer()) return {};
    if (gc_roots_.rooted_slots.count(declaration.result.id) == 0) return {};
    if (promoted_slots_.slots.count(declaration.result.id) != 0) return {};
    // The declaration block must be reachable by fallthrough alone: a branch
    // target can be entered again with an earlier reference still in the slot.
    int block_label = -1;
    for (std::size_t index = 0; index <= call_index + 1; ++index)
      if (operations[index].kind == tir::OpKind::Label)
        block_label = operations[index].label_id;
    if (block_label >= 0) {
      for (const tir::Op& other : operations) {
        if ((other.kind == tir::OpKind::Branch &&
             other.label_id == block_label) ||
            (other.kind == tir::OpKind::BranchCond &&
             (other.true_label == block_label ||
              other.false_label == block_label)))
          return {};
      }
    }
    // The local's storage is created here so the allocation can publish into it;
    // the declaration op that follows reuses this slot instead of making another.
    mlir::Value slot = alloca_at_entry(llvm_ptr_type(ctx_), loc);
    if (!slot) return {};
    precreated_slots_[declaration.result.id] = slot;
    return slot;
  }

  // Every root slot in the activation is described once, before the body runs.
  // Registrations are queued until all managed slots are known, then emitted
  // after the frame begins and before any body operation can trigger collection.
  void register_gc_root(mlir::Value slot, const Type& value_type,
                        mlir::Location loc,
                        mlir::Value initial_value = {}) {    std::vector<uint64_t> offsets;
    collect_managed_reference_offsets(value_type, 0, offsets);
    if (offsets.empty()) {
      // The two answers this lowering can give about a type must agree: a type
      // that reports no reference here must also report none to
      // function_uses_gc, which decides whether the activation registers at all.
      // If they ever disagree, registering offset 0 keeps a live object alive
      // instead of letting it be swept; dropping the root would be silent.
      assert(!collect_has_managed_reference(value_type) &&
             "managed type lost its reference offsets");
      if (!collect_has_managed_reference(value_type)) return;
      offsets.push_back(0);
    }
    pending_gc_roots_.push_back(
        PendingGcRoot{slot, value_type, loc, initial_value});
  }

  void flush_gc_root_registrations() {
    if (entry_block_ == nullptr || !gc_root_frame_marker_) return;
    if (pending_gc_roots_.empty()) {
      if (gc_roots_.only_cold_alloc_roots) {
        llvm::SmallVector<mlir::LLVM::CallOp, 4> end_calls;
        for (mlir::OpOperand& use : gc_root_frame_marker_.getUses()) {
          if (auto call = llvm::dyn_cast<mlir::LLVM::CallOp>(use.getOwner())) {
            if (call.getCallee() == "nrq_gc_root_frame_end")
              end_calls.push_back(call);
          }
        }
        for (mlir::LLVM::CallOp end_call : end_calls) end_call->erase();
        if (gc_root_frame_marker_.use_empty()) {
          if (mlir::Operation* begin_op = gc_root_frame_marker_.getDefiningOp())
            begin_op->erase();
          gc_root_frame_marker_ = {};
        }
      }
      return;
    }
    mlir::OpBuilder::InsertionGuard guard(builder_);
    mlir::Operation* frame_begin = gc_root_frame_marker_.getDefiningOp();
    if (!frame_begin) return;
    invalidate_inline_cfg_cache();
    builder_.setInsertionPointAfter(frame_begin);
    mlir::Operation* last_zero_op = frame_begin;
    // A slot must never hold a stale pointer when it becomes visible to a
    // collection, so every slot is cleared before any of them is published.
    for (const PendingGcRoot& root : pending_gc_roots_) {
      mlir::Type value_type = tir_type_to_mlir(root.type, ctx_, builder_);
      if (!value_type) continue;
      mlir::Value zero = builder_.create<mlir::LLVM::ZeroOp>(
          root.loc, value_type).getResult();
      last_zero_op = builder_.create<mlir::LLVM::StoreOp>(
          root.loc, zero, root.slot);
    }

    auto emit_initial_stores = [&]() {
      for (const PendingGcRoot& root : pending_gc_roots_) {
        if (!root.initial_value) continue;
        mlir::Type value_type = tir_type_to_mlir(root.type, ctx_, builder_);
        if (!value_type) continue;
        // A parameter that is a single managed reference is published as that
        // reference: the barrier takes it directly, so the activation needs no
        // staging slot and no byte copy to initialise its root.
        if (root.type.is_managed_pointer()) {
          emit_inline_root_store(root.slot, root.initial_value, root.loc,
                                 /*known_stack_slot=*/true);
          continue;
        }
        mlir::Value initial_slot = alloca_at_entry(value_type, root.loc);
        if (!initial_slot) continue;
        builder_.create<mlir::LLVM::StoreOp>(root.loc, root.initial_value,
                                             initial_slot);
        auto copy_fn = ensure_runtime_fn(
            module_, builder_, "nrq_gc_root_copy",
            mlir::LLVM::LLVMVoidType::get(ctx_),
            {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_), builder_.getI64Type()});
        mlir::Value byte_count = builder_.create<mlir::LLVM::ConstantOp>(
            root.loc, builder_.getI64Type(),
            builder_.getI64IntegerAttr(static_cast<int64_t>(type_size(root.type))))
                                     .getResult();
        builder_.create<mlir::LLVM::CallOp>(
            root.loc, copy_fn,
            mlir::ValueRange{root.slot, initial_slot, byte_count});
      }
    };

    const bool can_inline_static_frame =
        !has_late_root_registrations_ &&
        pending_gc_roots_.size() >= 1 &&
        pending_gc_roots_.size() <= 8 &&
        std::all_of(pending_gc_roots_.begin(), pending_gc_roots_.end(),
                    [](const PendingGcRoot& r) {
                      return r.type.is_managed_pointer();
                    });

    if (can_inline_static_frame &&
        emit_inline_static_root_frame(frame_begin, last_zero_op,
                                      emit_initial_stores)) {
      pending_gc_roots_.clear();
      return;
    }

    // Fallback: runtime frame registration.
    if (!emit_gc_frame_root_registration(pending_gc_roots_)) {
      for (const PendingGcRoot& root : pending_gc_roots_)
        emit_gc_root_registration(root.slot, root.type, root.loc);
    }
    emit_initial_stores();
    pending_gc_roots_.clear();
  }

  template <typename EmitInitialStores>
  bool emit_inline_static_root_frame(mlir::Operation* frame_begin,
                                     mlir::Operation* last_zero_op,
                                     EmitInitialStores&& emit_initial_stores) {
    const std::size_t root_count = pending_gc_roots_.size();
    mlir::Location loc = pending_gc_roots_.front().loc;
    const mlir::Type i32 = builder_.getI32Type();
    const mlir::Type i64 = builder_.getI64Type();
    const mlir::Type ptr = llvm_ptr_type(ctx_);

    // Pre-validate that every root slot is an entry-block single-pointer
    // alloca so all slots can be packed into one stack-resident
    // `NrqGcStackFrame` alloca: `{ ptr prev, i64 slot_count, [N x ptr] slots }`.
    for (const PendingGcRoot& root : pending_gc_roots_) {
      auto old_alloc = root.slot.getDefiningOp<mlir::LLVM::AllocaOp>();
      if (!old_alloc || old_alloc->getBlock() != entry_block_) return false;
    }

    // Pre-validate cold-path descriptor/slots if batched (>= 2 roots).
    FrameRootLayout slow_layout;
    mlir::Value slow_slots;
    mlir::Type slow_slots_type;
    if (root_count >= 2) {
      slow_layout = build_frame_root_layout(pending_gc_roots_);
      if (!slow_layout.valid) return false;
      slow_slots_type = mlir::LLVM::LLVMArrayType::get(
          ptr, static_cast<unsigned>(root_count));
      slow_slots = alloca_at_entry(slow_slots_type, loc);
      if (!slow_slots) return false;
    }

    mlir::Type slots_array_ty = mlir::LLVM::LLVMArrayType::get(
        ptr, static_cast<unsigned>(root_count));
    mlir::Type stack_frame_ty = mlir::LLVM::LLVMStructType::getLiteral(
        ctx_, {ptr, i64, slots_array_ty});
    mlir::Value frame_alloca = alloca_at_entry(stack_frame_ty, loc);
    if (!frame_alloca) return false;

    auto c_i32 = [&](int32_t v) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, i32, builder_.getI32IntegerAttr(v)).getResult();
    };
    auto c_i64 = [&](int64_t v) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, i64, builder_.getI64IntegerAttr(v)).getResult();
    };
    auto byte_gep = [&](mlir::Value base, int64_t offset) {
      if (offset == 0) return base;
      return builder_.create<mlir::LLVM::GEPOp>(
          loc, ptr, i8_type(), base, mlir::ValueRange{c_i64(offset)}).getResult();
    };

    // Populate `frame.slot_count` once in entry_block_ and redirect all slot
    // uses to contiguous `frame.slots[i]`.
    builder_.setInsertionPointAfter(frame_alloca.getDefiningOp());
    mlir::Value frame_count_ptr =
        builder_
            .create<mlir::LLVM::GEPOp>(
                loc, ptr, stack_frame_ty, frame_alloca,
                llvm::ArrayRef<mlir::LLVM::GEPArg>{0, 1})
            .getResult();
    builder_.create<mlir::LLVM::StoreOp>(loc, c_i64(static_cast<int64_t>(root_count)),
                                         frame_count_ptr)
        .setAlignment(8);
    for (std::size_t index = 0; index < root_count; ++index) {
      mlir::Value packed_slot =
          builder_
              .create<mlir::LLVM::GEPOp>(
                  loc, ptr, stack_frame_ty, frame_alloca,
                  llvm::ArrayRef<mlir::LLVM::GEPArg>{
                      0, 2, static_cast<int32_t>(index)})
              .getResult();
      mlir::Operation* old_alloc = pending_gc_roots_[index].slot.getDefiningOp();
      pending_gc_roots_[index].slot.replaceAllUsesWith(packed_slot);
      pending_gc_roots_[index].slot = packed_slot;
      if (old_alloc && old_alloc->use_empty()) old_alloc->erase();
    }

    // Split entry_block_ right after the slot null-stores so the body moves
    // into `body_block` receiving (frame_marker, cfg64, prev_stack_frame).
    mlir::Block* body_block =
        entry_block_->splitBlock(std::next(last_zero_op->getIterator()));
    mlir::Value body_marker = body_block->addArgument(i64, loc);
    mlir::Value body_cfg64 = body_block->addArgument(i64, loc);
    mlir::Value body_prev_frame = body_block->addArgument(ptr, loc);
    gc_root_frame_marker_.replaceAllUsesWith(body_marker);
    gc_root_frame_marker_ = body_marker;
    frame_begin->erase();

    mlir::Block* fast_open_block = new mlir::Block();
    mlir::Block* slow_open_block = new mlir::Block();
    func_op_.getBlocks().insert(body_block->getIterator(), fast_open_block);
    func_op_.getBlocks().push_back(slow_open_block);

    const std::pair<uint32_t, uint32_t> likely_weights{2000u, 1u};

    // Entry check: check `(gate & (PHASE | DIAG_ROOTS)) == 0`.
    builder_.setInsertionPointToEnd(entry_block_);
    mlir::Value tls = ensure_fast_tls_global(loc);
    mlir::Value cfg_addr = ensure_inline_gc_config_global(loc);
    mlir::Value stack_top_ptr = byte_gep(
        tls, static_cast<int64_t>(offsetof(NrqGcFastTls, stack_frame_top)));

    auto cfg64_load = builder_.create<mlir::LLVM::LoadOp>(
        loc, i64,
        byte_gep(cfg_addr,
                 static_cast<int64_t>(offsetof(NrqGcInlineConfig, packed_state))));
    cfg64_load.setAlignment(8);
    cfg64_load.setOrdering(mlir::LLVM::AtomicOrdering::acquire);
    mlir::Value gate =
        builder_.create<mlir::LLVM::TruncOp>(loc, i32, cfg64_load.getResult())
            .getResult();
    const uint32_t frame_gate_mask =
        NRQ_GC_INLINE_GATE_PHASE_BIT | NRQ_GC_INLINE_GATE_DIAG_ROOTS_BIT;
    mlir::Value masked_gate = builder_.create<mlir::LLVM::AndOp>(
        loc, gate, c_i32(static_cast<int32_t>(frame_gate_mask))).getResult();
    mlir::Value fast_ok = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::eq, masked_gate, c_i32(0)).getResult();
    builder_.create<mlir::LLVM::CondBrOp>(
        loc, fast_ok, fast_open_block, mlir::ValueRange{},
        slow_open_block, mlir::ValueRange{}, likely_weights);

    // Fast open block: link intrusive stack frame and publish to `stack_frame_top`.
    builder_.setInsertionPointToStart(fast_open_block);
    auto prev_frame_load =
        builder_.create<mlir::LLVM::LoadOp>(loc, ptr, stack_top_ptr);
    prev_frame_load.setAlignment(8);
    mlir::Value prev_frame = prev_frame_load.getResult();
    builder_.create<mlir::LLVM::StoreOp>(loc, prev_frame, frame_alloca)
        .setAlignment(8);
    auto pub_top =
        builder_.create<mlir::LLVM::StoreOp>(loc, frame_alloca, stack_top_ptr);
    pub_top.setAlignment(8);
    pub_top.setOrdering(mlir::LLVM::AtomicOrdering::release);
    builder_.create<mlir::LLVM::BrOp>(
        loc, mlir::ValueRange{c_i64(0), cfg64_load.getResult(), prev_frame},
        body_block);

    // Cold slow open block: call runtime frame API when phase/diag gate is raised.
    builder_.setInsertionPointToStart(slow_open_block);
    mlir::Value null_ptr =
        builder_.create<mlir::LLVM::ZeroOp>(loc, ptr).getResult();
    mlir::Value slow_marker;
    if (root_count >= 2) {
      mlir::Value slow_descriptor = frame_root_descriptor(slow_layout, loc);
      for (std::size_t index = 0; index < root_count; ++index) {
        mlir::Value element = builder_.create<mlir::LLVM::GEPOp>(
            loc, ptr, slow_slots_type, slow_slots,
            mlir::ValueRange{c_i32(0), c_i64(static_cast<int64_t>(index))})
                                  .getResult();
        builder_.create<mlir::LLVM::StoreOp>(loc, pending_gc_roots_[index].slot,
                                             element);
      }
      auto open_fn = ensure_runtime_fn(
          module_, builder_, "nrq_gc_root_frame_open", i64, {ptr, i64, ptr});
      slow_marker = builder_.create<mlir::LLVM::CallOp>(
          loc, open_fn,
          mlir::ValueRange{slow_slots, c_i64(static_cast<int64_t>(root_count)),
                           slow_descriptor}).getResult();
    } else {
      auto begin_fn = ensure_runtime_fn(
          module_, builder_, "nrq_gc_root_frame_begin", i64, {});
      slow_marker = builder_.create<mlir::LLVM::CallOp>(
          loc, begin_fn, mlir::ValueRange{}).getResult();
      emit_gc_root_registration(pending_gc_roots_[0].slot,
                                pending_gc_roots_[0].type,
                                pending_gc_roots_[0].loc);
    }
    mlir::Value slow_cfg64 = reload_inline_cfg64_uncached(loc);
    builder_.create<mlir::LLVM::BrOp>(
        loc, mlir::ValueRange{slow_marker, slow_cfg64, null_ptr}, body_block);

    // Replace any leading inline-config load at the top of `body_block` (before
    // any call or branch) with `body_cfg64`, eliminating a redundant atomic load.
    auto is_inline_config_load = [&](mlir::Operation* op) -> mlir::LLVM::LoadOp {
      auto load = llvm::dyn_cast<mlir::LLVM::LoadOp>(op);
      if (!load || load.getResult().getType() != i64) return nullptr;
      mlir::Value addr = load.getAddr();
      if (auto gep = addr.getDefiningOp<mlir::LLVM::GEPOp>()) {
        if (gep.getDynamicIndices().empty()) {
          auto raw_consts = gep.getRawConstantIndices();
          if (raw_consts.size() == 1 && raw_consts[0] == 0) {
            addr = gep.getBase();
          }
        }
      }
      if (auto sym = addr.getDefiningOp<mlir::LLVM::AddressOfOp>()) {
        if (sym.getGlobalName() == "nrq_gc_inline_config") return load;
      }
      return nullptr;
    };
    llvm::SmallVector<mlir::LLVM::LoadOp, 2> leading_cfg_loads;
    for (mlir::Operation& op : *body_block) {
      if (llvm::isa<mlir::LLVM::CallOp, mlir::func::CallOp,
                    mlir::LLVM::CondBrOp, mlir::LLVM::BrOp,
                    mlir::func::ReturnOp>(op)) {
        break;
      }
      if (auto cfg_ld = is_inline_config_load(&op)) {
        leading_cfg_loads.push_back(cfg_ld);
      }
    }
    for (mlir::LLVM::LoadOp cfg_ld : leading_cfg_loads) {
      cfg_ld.getResult().replaceAllUsesWith(body_cfg64);
      cfg_ld.erase();
    }

    // Emit initial parameter stores at start of body_block.
    builder_.setInsertionPointToStart(body_block);
    emit_initial_stores();

    // Rewrite every `nrq_gc_root_frame_end(body_marker)` site to pop the
    // intrusive stack frame when marker == 0, or call runtime end when marker != 0.
    llvm::SmallVector<mlir::LLVM::CallOp, 4> end_calls;
    for (mlir::OpOperand& use : body_marker.getUses()) {
      if (auto call = llvm::dyn_cast<mlir::LLVM::CallOp>(use.getOwner())) {
        if (call.getCallee() == "nrq_gc_root_frame_end")
          end_calls.push_back(call);
      }
    }
    for (mlir::LLVM::CallOp end_call : end_calls) {
      mlir::Location end_loc = end_call.getLoc();
      mlir::Block* cur_block = end_call->getBlock();
      mlir::Block* slow_end_block =
          cur_block->splitBlock(end_call->getIterator());
      mlir::Block* post_end_block =
          slow_end_block->splitBlock(std::next(end_call->getIterator()));

      mlir::Block* fast_end_block = new mlir::Block();
      func_op_.getBlocks().insert(post_end_block->getIterator(), fast_end_block);
      func_op_.getBlocks().splice(func_op_.getBlocks().end(),
                                  func_op_.getBlocks(),
                                  slow_end_block->getIterator());

      builder_.setInsertionPointToEnd(cur_block);
      mlir::Value is_inline = builder_.create<mlir::LLVM::ICmpOp>(
          end_loc, mlir::LLVM::ICmpPredicate::eq, body_marker, c_i64(0))
                                  .getResult();
      builder_.create<mlir::LLVM::CondBrOp>(
          end_loc, is_inline, fast_end_block, mlir::ValueRange{},
          slow_end_block, mlir::ValueRange{}, likely_weights);

      builder_.setInsertionPointToStart(fast_end_block);
      auto pop_top = builder_.create<mlir::LLVM::StoreOp>(
          end_loc, body_prev_frame, stack_top_ptr);
      pop_top.setAlignment(8);
      pop_top.setOrdering(mlir::LLVM::AtomicOrdering::release);
      builder_.create<mlir::LLVM::BrOp>(end_loc, post_end_block);

      builder_.setInsertionPointToEnd(slow_end_block);
      builder_.create<mlir::LLVM::BrOp>(end_loc, post_end_block);
    }

    return true;
  }

  void invalidate_inline_cfg_cache() {
    cached_cfg_block_ = nullptr;
    cached_cfg64_ = {};
  }

  void set_inline_cfg_cache(mlir::Block* block, mlir::Value cfg64) {
    cached_cfg_block_ = block;
    cached_cfg64_ = cfg64;
  }

  mlir::Value load_inline_cfg64(mlir::Location loc) {
    mlir::Block* cur = builder_.getInsertionBlock();
    if (cur == cached_cfg_block_ && cached_cfg64_) return cached_cfg64_;
    mlir::Value cfg_addr = ensure_inline_gc_config_global(loc);
    mlir::Value offset0 = cfg_addr;
    auto cfg64 = builder_.create<mlir::LLVM::LoadOp>(
        loc, builder_.getI64Type(), offset0);
    cfg64.setAlignment(8);
    cfg64.setOrdering(mlir::LLVM::AtomicOrdering::acquire);
    set_inline_cfg_cache(cur, cfg64.getResult());
    return cached_cfg64_;
  }

  mlir::Value reload_inline_cfg64_uncached(mlir::Location loc) {
    mlir::Value cfg_addr = ensure_inline_gc_config_global(loc);
    auto cfg64 = builder_.create<mlir::LLVM::LoadOp>(
        loc, builder_.getI64Type(), cfg_addr);
    cfg64.setAlignment(8);
    cfg64.setOrdering(mlir::LLVM::AtomicOrdering::acquire);
    return cfg64.getResult();
  }

  void emit_inline_root_store(mlir::Value slot, mlir::Value value,
                              mlir::Location loc, bool known_stack_slot,
                              const tir::Value* alignment_lhs = nullptr,
                              const Type* alignment_type = nullptr) {
    const mlir::Type i8 = builder_.getI8Type();
    const mlir::Type i32 = builder_.getI32Type();
    const mlir::Type i64 = builder_.getI64Type();
    const mlir::Type ptr = llvm_ptr_type(ctx_);

    mlir::Block* cur_block = builder_.getInsertionBlock();
    mlir::Value cached_cfg =
        (cur_block == cached_cfg_block_) ? cached_cfg64_ : mlir::Value{};

    mlir::Block* cont_block = cur_block->splitBlock(builder_.getInsertionPoint());
    cont_block->addArgument(i64, loc);
    mlir::Block* fast_block = new mlir::Block();
    mlir::Block* slow_block = new mlir::Block();
    func_op_.getBlocks().insert(cont_block->getIterator(), fast_block);
    func_op_.getBlocks().push_back(slow_block);

    const std::pair<uint32_t, uint32_t> likely_weights{2000u, 1u};

    builder_.setInsertionPointToEnd(cur_block);
    auto c_i32 = [&](int32_t v) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, i32, builder_.getI32IntegerAttr(v)).getResult();
    };
    auto c_i64 = [&](int64_t v) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, i64, builder_.getI64IntegerAttr(v)).getResult();
    };
    auto byte_gep = [&](mlir::Value base, int64_t offset) {
      if (offset == 0) return base;
      return builder_.create<mlir::LLVM::GEPOp>(
          loc, ptr, i8, base, mlir::ValueRange{c_i64(offset)}).getResult();
    };

    mlir::Value cfg_addr = ensure_inline_gc_config_global(loc);
    mlir::Value cfg64_val = cached_cfg;
    if (!cfg64_val) {
      auto cfg64 = builder_.create<mlir::LLVM::LoadOp>(
          loc, i64,
          byte_gep(cfg_addr,
                   static_cast<int64_t>(offsetof(NrqGcInlineConfig, packed_state))));
      cfg64.setAlignment(8);
      cfg64.setOrdering(mlir::LLVM::AtomicOrdering::acquire);
      cfg64_val = cfg64.getResult();
    }
    mlir::Value gate =
        builder_.create<mlir::LLVM::TruncOp>(loc, i32, cfg64_val).getResult();
    const uint32_t barrier_mask = known_stack_slot
        ? static_cast<uint32_t>(NRQ_GC_INLINE_GATE_PHASE_BIT)
        : static_cast<uint32_t>(NRQ_GC_INLINE_GATE_PHASE_BIT |
                                NRQ_GC_INLINE_GATE_STORE_BARRIER_BIT);
    mlir::Value masked = builder_.create<mlir::LLVM::AndOp>(
        loc, gate, c_i32(static_cast<int32_t>(barrier_mask))).getResult();
    mlir::Value cond = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::eq, masked, c_i32(0)).getResult();
    builder_.create<mlir::LLVM::CondBrOp>(
        loc, cond, fast_block, mlir::ValueRange{},
        slow_block, mlir::ValueRange{}, likely_weights);

    builder_.setInsertionPointToStart(fast_block);
    auto store = builder_.create<mlir::LLVM::StoreOp>(loc, value, slot);
    if (alignment_lhs && alignment_type) {
      apply_access_alignment(store, *alignment_lhs, *alignment_type);
    } else {
      store.setAlignment(8);
    }
    builder_.create<mlir::LLVM::BrOp>(loc, mlir::ValueRange{cfg64_val}, cont_block);

    builder_.setInsertionPointToStart(slow_block);
    auto store_fn = ensure_runtime_fn(
        module_, builder_, "nrq_gc_root_store",
        mlir::LLVM::LLVMVoidType::get(ctx_), {ptr, ptr});
    builder_.create<mlir::LLVM::CallOp>(loc, store_fn,
                                        mlir::ValueRange{slot, value});
    mlir::Value slow_cfg64 = reload_inline_cfg64_uncached(loc);
    builder_.create<mlir::LLVM::BrOp>(
        loc, mlir::ValueRange{slow_cfg64}, cont_block);

    builder_.setInsertionPointToStart(cont_block);
    set_inline_cfg_cache(cont_block, cont_block->getArgument(0));
  }

  void emit_inline_pin_transfer(mlir::Value reference, mlir::Location loc) {
    const mlir::Type i8 = builder_.getI8Type();
    const mlir::Type i32 = builder_.getI32Type();
    const mlir::Type i64 = builder_.getI64Type();
    const mlir::Type ptr = llvm_ptr_type(ctx_);

    mlir::Block* cur_block = builder_.getInsertionBlock();
    mlir::Value cached_cfg =
        (cur_block == cached_cfg_block_) ? cached_cfg64_ : mlir::Value{};
    mlir::Block* cont_block = cur_block->splitBlock(builder_.getInsertionPoint());
    cont_block->addArgument(i64, loc);
    mlir::Block* fast_block = new mlir::Block();
    mlir::Block* slow_block = new mlir::Block();
    func_op_.getBlocks().insert(cont_block->getIterator(), fast_block);
    func_op_.getBlocks().push_back(slow_block);

    const std::pair<uint32_t, uint32_t> likely_weights{2000u, 1u};

    builder_.setInsertionPointToEnd(cur_block);
    auto c_i32 = [&](int32_t v) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, i32, builder_.getI32IntegerAttr(v)).getResult();
    };
    auto c_i64 = [&](int64_t v) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, i64, builder_.getI64IntegerAttr(v)).getResult();
    };
    auto byte_gep = [&](mlir::Value base, int64_t offset) {
      if (offset == 0) return base;
      return builder_.create<mlir::LLVM::GEPOp>(
          loc, ptr, i8, base, mlir::ValueRange{c_i64(offset)}).getResult();
    };

    mlir::Value tls = ensure_fast_tls_global(loc);
    mlir::Value cfg64_val = cached_cfg;
    if (!cfg64_val) {
      mlir::Value cfg_addr = ensure_inline_gc_config_global(loc);
      auto cfg64 = builder_.create<mlir::LLVM::LoadOp>(
          loc, i64,
          byte_gep(cfg_addr,
                   static_cast<int64_t>(offsetof(NrqGcInlineConfig, packed_state))));
      cfg64.setAlignment(8);
      cfg64.setOrdering(mlir::LLVM::AtomicOrdering::acquire);
      cfg64_val = cfg64.getResult();
    }
    mlir::Value gate =
        builder_.create<mlir::LLVM::TruncOp>(loc, i32, cfg64_val).getResult();
    mlir::Value phase_masked = builder_.create<mlir::LLVM::AndOp>(
        loc, gate, c_i32(static_cast<int32_t>(NRQ_GC_INLINE_GATE_PHASE_BIT)))
                                   .getResult();
    mlir::Value phase_clear = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::eq, phase_masked, c_i32(0)).getResult();

    mlir::Value null_ptr = builder_.create<mlir::LLVM::ZeroOp>(loc, ptr).getResult();
    auto fend = builder_.create<mlir::LLVM::LoadOp>(
        loc, ptr,
        byte_gep(tls, static_cast<int64_t>(offsetof(NrqGcFastTls, frame_end))));
    fend.setAlignment(8);
    mlir::Value mutator_ready = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::ne, fend.getResult(), null_ptr)
                                    .getResult();

    mlir::Value pin_slot = byte_gep(
        tls, static_cast<int64_t>(offsetof(NrqGcFastTls, transfer_pin_fast)));
    auto cur_pin = builder_.create<mlir::LLVM::LoadOp>(loc, ptr, pin_slot);
    cur_pin.setAlignment(8);
    mlir::Value pin_empty = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::eq, cur_pin.getResult(), null_ptr)
                                .getResult();

    mlir::Value ready =
        builder_.create<mlir::LLVM::AndOp>(loc, phase_clear, mutator_ready)
            .getResult();
    mlir::Value fast_ok =
        builder_.create<mlir::LLVM::AndOp>(loc, ready, pin_empty).getResult();
    builder_.create<mlir::LLVM::CondBrOp>(
        loc, fast_ok, fast_block, mlir::ValueRange{},
        slow_block, mlir::ValueRange{}, likely_weights);

    builder_.setInsertionPointToStart(fast_block);
    builder_.create<mlir::LLVM::StoreOp>(loc, reference, pin_slot)
        .setAlignment(8);
    builder_.create<mlir::LLVM::BrOp>(loc, mlir::ValueRange{cfg64_val}, cont_block);

    builder_.setInsertionPointToStart(slow_block);
    auto pin_fn = ensure_runtime_fn(
        module_, builder_, "nrq_gc_pin_transfer", ptr, {ptr});
    builder_.create<mlir::LLVM::CallOp>(loc, pin_fn,
                                        mlir::ValueRange{reference});
    mlir::Value slow_cfg64 = reload_inline_cfg64_uncached(loc);
    builder_.create<mlir::LLVM::BrOp>(
        loc, mlir::ValueRange{slow_cfg64}, cont_block);

    builder_.setInsertionPointToStart(cont_block);
    set_inline_cfg_cache(cont_block, cont_block->getArgument(0));
  }

  void emit_inline_unpin(mlir::Value reference, mlir::Location loc) {
    const mlir::Type i8 = builder_.getI8Type();
    const mlir::Type i32 = builder_.getI32Type();
    const mlir::Type i64 = builder_.getI64Type();
    const mlir::Type ptr = llvm_ptr_type(ctx_);

    mlir::Block* cur_block = builder_.getInsertionBlock();
    mlir::Value cached_cfg =
        (cur_block == cached_cfg_block_) ? cached_cfg64_ : mlir::Value{};
    mlir::Block* cont_block = cur_block->splitBlock(builder_.getInsertionPoint());
    cont_block->addArgument(i64, loc);
    mlir::Block* fast_block = new mlir::Block();
    mlir::Block* slow_block = new mlir::Block();
    func_op_.getBlocks().insert(cont_block->getIterator(), fast_block);
    func_op_.getBlocks().push_back(slow_block);

    const std::pair<uint32_t, uint32_t> likely_weights{2000u, 1u};

    builder_.setInsertionPointToEnd(cur_block);
    auto c_i32 = [&](int32_t v) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, i32, builder_.getI32IntegerAttr(v)).getResult();
    };
    auto c_i64 = [&](int64_t v) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, i64, builder_.getI64IntegerAttr(v)).getResult();
    };
    auto byte_gep = [&](mlir::Value base, int64_t offset) {
      if (offset == 0) return base;
      return builder_.create<mlir::LLVM::GEPOp>(
          loc, ptr, i8, base, mlir::ValueRange{c_i64(offset)}).getResult();
    };

    mlir::Value tls = ensure_fast_tls_global(loc);
    mlir::Value cfg64_val = cached_cfg;
    if (!cfg64_val) {
      mlir::Value cfg_addr = ensure_inline_gc_config_global(loc);
      auto cfg64 = builder_.create<mlir::LLVM::LoadOp>(
          loc, i64,
          byte_gep(cfg_addr,
                   static_cast<int64_t>(offsetof(NrqGcInlineConfig, packed_state))));
      cfg64.setAlignment(8);
      cfg64.setOrdering(mlir::LLVM::AtomicOrdering::acquire);
      cfg64_val = cfg64.getResult();
    }
    mlir::Value gate =
        builder_.create<mlir::LLVM::TruncOp>(loc, i32, cfg64_val).getResult();
    mlir::Value phase_masked = builder_.create<mlir::LLVM::AndOp>(
        loc, gate, c_i32(static_cast<int32_t>(NRQ_GC_INLINE_GATE_PHASE_BIT)))
                                   .getResult();
    mlir::Value phase_clear = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::eq, phase_masked, c_i32(0)).getResult();

    mlir::Value pin_slot = byte_gep(
        tls, static_cast<int64_t>(offsetof(NrqGcFastTls, transfer_pin_fast)));
    auto cur_pin = builder_.create<mlir::LLVM::LoadOp>(loc, ptr, pin_slot);
    cur_pin.setAlignment(8);
    mlir::Value pin_matches = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::eq, cur_pin.getResult(), reference)
                                  .getResult();

    mlir::Value fast_ok =
        builder_.create<mlir::LLVM::AndOp>(loc, phase_clear, pin_matches)
            .getResult();
    builder_.create<mlir::LLVM::CondBrOp>(
        loc, fast_ok, fast_block, mlir::ValueRange{},
        slow_block, mlir::ValueRange{}, likely_weights);

    builder_.setInsertionPointToStart(fast_block);
    mlir::Value null_ptr = builder_.create<mlir::LLVM::ZeroOp>(loc, ptr).getResult();
    builder_.create<mlir::LLVM::StoreOp>(loc, null_ptr, pin_slot)
        .setAlignment(8);
    builder_.create<mlir::LLVM::BrOp>(loc, mlir::ValueRange{cfg64_val}, cont_block);

    builder_.setInsertionPointToStart(slow_block);
    auto unpin_fn = ensure_runtime_fn(
        module_, builder_, "nrq_gc_unpin",
        mlir::LLVM::LLVMVoidType::get(ctx_), {ptr});
    builder_.create<mlir::LLVM::CallOp>(loc, unpin_fn,
                                        mlir::ValueRange{reference});
    mlir::Value slow_cfg64 = reload_inline_cfg64_uncached(loc);
    builder_.create<mlir::LLVM::BrOp>(
        loc, mlir::ValueRange{slow_cfg64}, cont_block);

    builder_.setInsertionPointToStart(cont_block);
    set_inline_cfg_cache(cont_block, cont_block->getArgument(0));
  }

  mlir::Type i8_type() { return builder_.getI8Type(); }

  // A reference-offset array is a constant that describes a type, not a value
  // this activation computes. Materialising it in a stack slot cost an alloca and
  // one store per offset on every registration, in every function that registers
  // the same layout. Each distinct layout is instead emitted once per module as a
  // private read-only array and registrations pass its address, so repeated
  // registrations of a layout share one descriptor in .rdata and cost no code.
  mlir::Value reference_layout_descriptor(llvm::ArrayRef<uint64_t> offsets,
                                         mlir::Location loc) {
    std::string name = "nrq_gc_layout";
    for (uint64_t offset : offsets)
      name += "_" + std::to_string(offset);
    if (module_.lookupSymbol<mlir::LLVM::GlobalOp>(name))
      return builder_.create<mlir::LLVM::AddressOfOp>(
          loc, llvm_ptr_type(ctx_), name).getResult();
    auto array_type = mlir::LLVM::LLVMArrayType::get(
        builder_.getI64Type(), static_cast<unsigned>(offsets.size()));
    llvm::SmallVector<int64_t, 4> values;
    for (uint64_t offset : offsets)
      values.push_back(static_cast<int64_t>(offset));
    auto initializer = mlir::DenseIntElementsAttr::get(
        mlir::RankedTensorType::get({static_cast<int64_t>(offsets.size())},
                                    builder_.getI64Type()),
        values);
    {
      mlir::OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(module_.getBody());
      if (!module_.lookupSymbol<mlir::LLVM::GlobalOp>(name))
        builder_.create<mlir::LLVM::GlobalOp>(loc, array_type,
                                              /*isConstant=*/true,
                                              mlir::LLVM::Linkage::Private,
                                              name, initializer,
                                              /*alignment=*/8);
    }
    return builder_.create<mlir::LLVM::AddressOfOp>(
        loc, llvm_ptr_type(ctx_), name).getResult();
  }

  struct AllocationSiteGlobals {
    mlir::Value site_cache;
    mlir::Value site_id;
  };

  // Compiler-emitted per-site cache pair. The pointer slot caches the runtime's
  // resolved `nrq_object_layout*`, while the adjacent `i32` global caches the
  // runtime's compact/extended header layout ID once resolved on the cold path.
  AllocationSiteGlobals allocation_site_globals(mlir::Location loc) {
    static int counter = 0;
    const int index = counter++;
    const std::string cache_name = "nrq_alloc_site_" + std::to_string(index);
    const std::string id_name = "nrq_alloc_site_id_" + std::to_string(index);
    auto storage_type = mlir::LLVM::LLVMArrayType::get(builder_.getI8Type(), 8);
    {
      mlir::OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(module_.getBody());
      builder_.create<mlir::LLVM::GlobalOp>(
          loc, storage_type, /*isConstant=*/false, mlir::LLVM::Linkage::Private,
          cache_name, builder_.getStringAttr(std::string(8, '\0')),
          /*alignment=*/8);
      builder_.create<mlir::LLVM::GlobalOp>(
          loc, builder_.getI32Type(), /*isConstant=*/false,
          mlir::LLVM::Linkage::Private, id_name,
          builder_.getI32IntegerAttr(0), /*alignment=*/4);
    }
    mlir::Value site_cache = builder_.create<mlir::LLVM::AddressOfOp>(
        loc, llvm_ptr_type(ctx_), cache_name).getResult();
    mlir::Value site_id = builder_.create<mlir::LLVM::AddressOfOp>(
        loc, llvm_ptr_type(ctx_), id_name).getResult();
    return AllocationSiteGlobals{site_cache, site_id};
  }

  mlir::Value ensure_inline_gc_config_global(mlir::Location loc) {
    constexpr const char* kSymbol = "nrq_gc_inline_config";
    if (!module_.lookupSymbol<mlir::LLVM::GlobalOp>(kSymbol)) {
      mlir::OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(module_.getBody());
      auto storage_type = mlir::LLVM::LLVMArrayType::get(
          builder_.getI8Type(), static_cast<unsigned>(sizeof(NrqGcInlineConfig)));
      builder_.create<mlir::LLVM::GlobalOp>(
          loc, storage_type, /*isConstant=*/false,
          mlir::LLVM::Linkage::External, kSymbol, mlir::Attribute{},
          /*alignment=*/64);
    }
    return builder_.create<mlir::LLVM::AddressOfOp>(
        loc, llvm_ptr_type(ctx_), kSymbol).getResult();
  }

  mlir::Value ensure_fast_tls_global(mlir::Location loc) {
    constexpr const char* kSymbol = "nrq_gc_fast_tls";
    if (!module_.lookupSymbol<mlir::LLVM::GlobalOp>(kSymbol)) {
      mlir::OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(module_.getBody());
      auto storage_type = mlir::LLVM::LLVMArrayType::get(
          builder_.getI8Type(), static_cast<unsigned>(sizeof(NrqGcFastTls)));
      builder_.create<mlir::LLVM::GlobalOp>(
          loc, storage_type, /*isConstant=*/false,
          mlir::LLVM::Linkage::External, kSymbol, mlir::Attribute{},
          /*alignment=*/64, /*addrSpace=*/0, /*dsoLocal=*/false,
          /*threadLocal=*/true);
    }
    mlir::Value raw_addr = builder_.create<mlir::LLVM::AddressOfOp>(
        loc, llvm_ptr_type(ctx_), kSymbol).getResult();
    return builder_.create<mlir::LLVM::ThreadlocalAddressOp>(
        loc, llvm_ptr_type(ctx_), raw_addr).getResult();
  }

  static std::optional<uint64_t> constant_u64_value(mlir::Value value) {
    if (!value) return std::nullopt;
    if (auto constant = value.getDefiningOp<mlir::LLVM::ConstantOp>()) {
      if (auto int_attr = mlir::dyn_cast<mlir::IntegerAttr>(constant.getValue()))
        return static_cast<uint64_t>(int_attr.getValue().getZExtValue());
      return std::nullopt;
    }
    if (auto zext = value.getDefiningOp<mlir::LLVM::ZExtOp>())
      return constant_u64_value(zext.getArg());
    if (auto trunc = value.getDefiningOp<mlir::LLVM::TruncOp>()) {
      auto inner = constant_u64_value(trunc.getArg());
      if (!inner) return std::nullopt;
      const unsigned bits = trunc.getType().getIntOrFloatBitWidth();
      if (bits >= 64) return inner;
      return *inner & ((uint64_t{1} << bits) - 1);
    }
    return std::nullopt;
  }

  static constexpr uint64_t align_span_bytes(uint64_t raw_bytes) {
    constexpr uint64_t align = NRQ_GC_INLINE_SPAN_ALIGNMENT;
    return (raw_bytes + align - 1) & ~(align - 1);
  }

  // Emits a TLAB bump-pointer fast path for eligible rooted array/SoA sites,
  // branching to the runtime planned allocation entry point (`slow_symbol`) on
  // cold site initialization, TLAB refill/exhaustion, active GC cycle, pending
  // accounting flush, or oversized dynamic requests.
  mlir::Value emit_inline_rooted_tlab_allocation(
      mlir::Location loc, mlir::Value root_slot, const Type& root_type,
      mlir::Value count, mlir::Value element_size, mlir::Value table_pointer,
      mlir::Value table_count, const char* slow_symbol, bool force_extended) {
    // NRQ_ALLOC_PLAN_ARRAY | NRQ_ALLOC_PLAN_REPEATED | NRQ_ALLOC_PLAN_REPR_BITS(...)
    constexpr uint32_t kCompactPlanBits = 1u | 2u | (2u << 8);
    constexpr uint32_t kExtendedPlanBits = 1u | 2u | (3u << 8);
    constexpr uint64_t kTlabLimit = NRQ_GC_INLINE_TLAB_BYTES;
    constexpr uint64_t kMaxCompactPayload = kTlabLimit - 8;
    constexpr uint64_t kMaxExtendedPayload = kTlabLimit - 16;

    const mlir::Type i8 = builder_.getI8Type();
    const mlir::Type i32 = builder_.getI32Type();
    const mlir::Type i64 = builder_.getI64Type();
    const mlir::Type ptr = llvm_ptr_type(ctx_);

    auto c_i8 = [&](int8_t v) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, i8, builder_.getI8IntegerAttr(v)).getResult();
    };
    auto c_i32 = [&](int32_t v) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, i32, builder_.getI32IntegerAttr(v)).getResult();
    };
    auto c_i64 = [&](uint64_t v) {
      return builder_.create<mlir::LLVM::ConstantOp>(
          loc, i64, builder_.getI64IntegerAttr(static_cast<int64_t>(v))).getResult();
    };
    auto byte_gep = [&](mlir::Value base, uint64_t offset) {
      if (offset == 0) return base;
      return builder_.create<mlir::LLVM::GEPOp>(
          loc, ptr, i8, base, mlir::ValueRange{c_i64(offset)}).getResult();
    };

    auto slow_fn = ensure_runtime_fn(
        module_, builder_, slow_symbol, ptr,
        {ptr, i64, i64, ptr, i64, ptr, i32});
    AllocationSiteGlobals site = allocation_site_globals(loc);

    const std::optional<uint64_t> const_elem = constant_u64_value(element_size);
    const std::optional<uint64_t> const_count = constant_u64_value(count);

    bool inline_eligible = false;
    bool fixed_geometry = false;
    uint32_t fixed_kind = NRQ_GC_INLINE_KIND_NONE;
    uint64_t fixed_header = 0;
    uint64_t fixed_payload = 0;
    uint64_t fixed_span = 0;
    uint64_t max_multi_count = 0;

    if (const_elem && *const_elem != 0) {
      if (const_count) {
        if (*const_count >= 1 &&
            *const_count <= std::numeric_limits<uint32_t>::max()) {
          const bool use_compact = !force_extended && *const_count == 1;
          const uint64_t hdr = use_compact ? 8 : 16;
          if (*const_elem <= (kTlabLimit - hdr) / *const_count) {
            const uint64_t payload = (*const_count) * (*const_elem);
            const uint64_t span = align_span_bytes(hdr + payload);
            if (span <= kTlabLimit) {
              inline_eligible = true;
              fixed_geometry = true;
              fixed_kind = use_compact ? NRQ_GC_INLINE_KIND_COMPACT
                                       : NRQ_GC_INLINE_KIND_EXTENDED;
              fixed_header = hdr;
              fixed_payload = payload;
              fixed_span = span;
            }
          }
        }
      } else if (force_extended) {
        if (*const_elem <= kMaxExtendedPayload) {
          max_multi_count = kMaxExtendedPayload / *const_elem;
          if (max_multi_count >= 1) inline_eligible = true;
        }
      } else if (*const_elem <= kMaxCompactPayload) {
        max_multi_count = kMaxExtendedPayload / *const_elem;
        inline_eligible = true;
      }
    }

    auto plan_value_for_slow = [&](mlir::Value is_one_val) {
      if (force_extended || (fixed_geometry && fixed_kind == NRQ_GC_INLINE_KIND_EXTENDED))
        return c_i32(static_cast<int32_t>(kExtendedPlanBits));
      if (fixed_geometry && fixed_kind == NRQ_GC_INLINE_KIND_COMPACT)
        return c_i32(static_cast<int32_t>(kCompactPlanBits));
      return builder_.create<mlir::LLVM::SelectOp>(
          loc, is_one_val, c_i32(static_cast<int32_t>(kCompactPlanBits)),
          c_i32(static_cast<int32_t>(kExtendedPlanBits))).getResult();
    };

    const bool cold_only_frame = gc_roots_.only_cold_alloc_roots;
    auto emit_cold_root_enter = [&]() -> mlir::Value {
      if (!cold_only_frame) return {};
      mlir::Value null_ptr =
          builder_.create<mlir::LLVM::ZeroOp>(loc, ptr).getResult();
      builder_.create<mlir::LLVM::StoreOp>(loc, null_ptr, root_slot)
          .setAlignment(8);
      auto begin_fn = ensure_runtime_fn(
          module_, builder_, "nrq_gc_root_frame_begin", i64, {});
      mlir::Value marker = builder_.create<mlir::LLVM::CallOp>(
          loc, begin_fn, mlir::ValueRange{}).getResult();
      emit_gc_root_registration(root_slot, root_type, loc);
      return marker;
    };
    auto emit_cold_root_exit = [&](mlir::Value marker) {
      if (!cold_only_frame || !marker) return;
      auto end_fn = ensure_runtime_fn(
          module_, builder_, "nrq_gc_root_frame_end",
          mlir::LLVM::LLVMVoidType::get(ctx_), {i64});
      builder_.create<mlir::LLVM::CallOp>(loc, end_fn, mlir::ValueRange{marker});
    };

    if (!inline_eligible) {
      invalidate_inline_cfg_cache();
      mlir::Value cold_marker = emit_cold_root_enter();
      mlir::Value plan = c_i32(static_cast<int32_t>(
          (!force_extended && const_count && *const_count == 1 &&
           const_elem && *const_elem <= kMaxCompactPayload)
              ? kCompactPlanBits
              : kExtendedPlanBits));
      mlir::Value res = builder_.create<mlir::LLVM::CallOp>(
          loc, slow_fn,
          mlir::ValueRange{root_slot, count, element_size, table_pointer,
                           table_count, site.site_cache, plan}).getResult();
      emit_cold_root_exit(cold_marker);
      return res;
    }

    // Dynamic count sizing and representation selection.
    mlir::Value is_one_cond;
    mlir::Value count_ok;
    mlir::Value header_bytes;
    mlir::Value payload_bytes;
    mlir::Value span_bytes;
    if (fixed_geometry) {
      header_bytes = c_i64(fixed_header);
      payload_bytes = c_i64(fixed_payload);
      span_bytes = c_i64(fixed_span);
    } else {
      is_one_cond = builder_.create<mlir::LLVM::ICmpOp>(
          loc, mlir::LLVM::ICmpPredicate::eq, count, c_i64(1)).getResult();
      if (max_multi_count == 0) {
        // Only count == 1 fits inside a TLAB page for this element size.
        count_ok = is_one_cond;
        header_bytes = c_i64(8);
        payload_bytes = c_i64(*const_elem);
        span_bytes = c_i64(align_span_bytes(8 + *const_elem));
      } else {
        mlir::Value count_m1 = builder_.create<mlir::LLVM::SubOp>(
            loc, count, c_i64(1)).getResult();
        count_ok = builder_.create<mlir::LLVM::ICmpOp>(
            loc, mlir::LLVM::ICmpPredicate::ule, count_m1,
            c_i64(max_multi_count - 1)).getResult();
        if (force_extended) {
          header_bytes = c_i64(16);
        } else {
          header_bytes = builder_.create<mlir::LLVM::SelectOp>(
              loc, is_one_cond, c_i64(8), c_i64(16)).getResult();
        }
        payload_bytes = builder_.create<mlir::LLVM::MulOp>(
            loc, count, c_i64(*const_elem)).getResult();
        mlir::Value raw_span = builder_.create<mlir::LLVM::AddOp>(
            loc, payload_bytes, header_bytes).getResult();
        mlir::Value padded = builder_.create<mlir::LLVM::AddOp>(
            loc, raw_span, c_i64(NRQ_GC_INLINE_SPAN_ALIGNMENT - 1)).getResult();
        span_bytes = builder_.create<mlir::LLVM::AndOp>(
            loc, padded,
            c_i64(~uint64_t{NRQ_GC_INLINE_SPAN_ALIGNMENT - 1})).getResult();
      }
    }

    auto site_id_load = builder_.create<mlir::LLVM::LoadOp>(
        loc, i32, site.site_id);
    site_id_load.setAlignment(4);
    site_id_load.setOrdering(mlir::LLVM::AtomicOrdering::acquire);
    mlir::Value site_id = site_id_load.getResult();

    mlir::Value cfg64 = load_inline_cfg64(loc);

    mlir::Value gate = builder_.create<mlir::LLVM::TruncOp>(
        loc, i32, cfg64).getResult();
    mlir::Value site_ready = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::ne, site_id, c_i32(0)).getResult();
    mlir::Value masked_gate = builder_.create<mlir::LLVM::AndOp>(
        loc, gate,
        c_i32(static_cast<int32_t>(NRQ_GC_INLINE_GATE_ALLOC_MASK))).getResult();
    mlir::Value gate_clear = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::eq, masked_gate, c_i32(0)).getResult();
    mlir::Value global_ok = builder_.create<mlir::LLVM::AndOp>(
        loc, site_ready, gate_clear).getResult();
    if (!fixed_geometry) {
      global_ok = builder_.create<mlir::LLVM::AndOp>(
          loc, global_ok, count_ok).getResult();
    }

    mlir::Block* check_tlab_block = func_op_.addBlock();
    mlir::Block* fast_block = func_op_.addBlock();
    mlir::Block* merge_block = func_op_.addBlock();
    merge_block->addArgument(ptr, loc);
    merge_block->addArgument(i64, loc);

    mlir::Block* slow_block = new mlir::Block();
    mlir::Block* populate_site_block = new mlir::Block();
    mlir::Block* slow_done_block = new mlir::Block();
    cold_alloc_blocks_.push_back(slow_block);
    cold_alloc_blocks_.push_back(populate_site_block);
    cold_alloc_blocks_.push_back(slow_done_block);

    const std::pair<uint32_t, uint32_t> likely_weights{2000u, 1u};
    builder_.create<mlir::LLVM::CondBrOp>(
        loc, global_ok, check_tlab_block, mlir::ValueRange{},
        slow_block, mlir::ValueRange{}, likely_weights);

    // Hot TLAB check: verifies the representation's clamped bump range has
    // room for `span_bytes`. `tlab_end`/`ext_end` is clamped by
    // `accounting_batch` in runtime slab installation/extension.
    builder_.setInsertionPointToStart(check_tlab_block);
    mlir::Value tls = ensure_fast_tls_global(loc);

    mlir::Value cursor_ptr;
    mlir::Value end_ptr;
    if (fixed_geometry) {
      const bool use_ext = fixed_kind == NRQ_GC_INLINE_KIND_EXTENDED;
      cursor_ptr = byte_gep(
          tls, static_cast<int64_t>(use_ext ? offsetof(NrqGcFastTls, ext_cursor)
                                            : offsetof(NrqGcFastTls, tlab_cursor)));
      end_ptr = byte_gep(
          tls, static_cast<int64_t>(use_ext ? offsetof(NrqGcFastTls, ext_end)
                                            : offsetof(NrqGcFastTls, tlab_end)));
    } else if (force_extended) {
      cursor_ptr = byte_gep(tls, static_cast<int64_t>(offsetof(NrqGcFastTls, ext_cursor)));
      end_ptr = byte_gep(tls, static_cast<int64_t>(offsetof(NrqGcFastTls, ext_end)));
    } else if (max_multi_count == 0) {
      cursor_ptr = byte_gep(tls, static_cast<int64_t>(offsetof(NrqGcFastTls, tlab_cursor)));
      end_ptr = byte_gep(tls, static_cast<int64_t>(offsetof(NrqGcFastTls, tlab_end)));
    } else {
      mlir::Value cursor_off = builder_.create<mlir::LLVM::SelectOp>(
          loc, is_one_cond,
          c_i64(static_cast<int64_t>(offsetof(NrqGcFastTls, tlab_cursor))),
          c_i64(static_cast<int64_t>(offsetof(NrqGcFastTls, ext_cursor)))).getResult();
      mlir::Value end_off = builder_.create<mlir::LLVM::SelectOp>(
          loc, is_one_cond,
          c_i64(static_cast<int64_t>(offsetof(NrqGcFastTls, tlab_end))),
          c_i64(static_cast<int64_t>(offsetof(NrqGcFastTls, ext_end)))).getResult();
      cursor_ptr = builder_.create<mlir::LLVM::GEPOp>(
          loc, ptr, i8, tls, mlir::ValueRange{cursor_off}).getResult();
      end_ptr = builder_.create<mlir::LLVM::GEPOp>(
          loc, ptr, i8, tls, mlir::ValueRange{end_off}).getResult();
    }

    auto cursor_load = builder_.create<mlir::LLVM::LoadOp>(loc, ptr, cursor_ptr);
    cursor_load.setAlignment(8);
    mlir::Value cursor = cursor_load.getResult();
    auto end_load = builder_.create<mlir::LLVM::LoadOp>(loc, ptr, end_ptr);
    end_load.setAlignment(8);
    mlir::Value cursor_int = builder_.create<mlir::LLVM::PtrToIntOp>(
        loc, i64, cursor).getResult();
    mlir::Value end_int = builder_.create<mlir::LLVM::PtrToIntOp>(
        loc, i64, end_load.getResult()).getResult();
    mlir::Value new_cursor_int = builder_.create<mlir::LLVM::AddOp>(
        loc, cursor_int, span_bytes).getResult();
    mlir::Value room_ok = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::ule, new_cursor_int, end_int).getResult();

    builder_.create<mlir::LLVM::CondBrOp>(
        loc, room_ok, fast_block, mlir::ValueRange{},
        slow_block, mlir::ValueRange{}, likely_weights);

    // Hot inline bump allocation: advance cursor, write Compact (H8) or
    // Extended (H16) header (slab storage is pre-zeroed at reservation time),
    // update pending counters, and publish root.
    builder_.setInsertionPointToStart(fast_block);
    mlir::Value new_cursor = builder_.create<mlir::LLVM::GEPOp>(
        loc, ptr, i8, cursor, mlir::ValueRange{span_bytes}).getResult();
    auto store_cursor = builder_.create<mlir::LLVM::StoreOp>(
        loc, new_cursor, cursor_ptr);
    store_cursor.setAlignment(8);

    mlir::Value initial_state64 = builder_.create<mlir::LLVM::LShrOp>(
        loc, cfg64, c_i64(32)).getResult();
    mlir::Value site_id64 = builder_.create<mlir::LLVM::ZExtOp>(
        loc, i64, site_id).getResult();

    auto write_h8_header = [&]() {
      mlir::Value shifted_id = builder_.create<mlir::LLVM::ShlOp>(
          loc, site_id64, c_i64(32)).getResult();
      mlir::Value hdr64 = builder_.create<mlir::LLVM::OrOp>(
          loc, initial_state64, shifted_id).getResult();
      auto store_hdr = builder_.create<mlir::LLVM::StoreOp>(loc, hdr64, cursor);
      store_hdr.setAlignment(8);
    };
    auto write_h16_header = [&]() {
      mlir::Value count32 = builder_.create<mlir::LLVM::TruncOp>(
          loc, i32, count).getResult();
      mlir::Value count64 = builder_.create<mlir::LLVM::ZExtOp>(
          loc, i64, count32).getResult();
      mlir::Value shifted_state = builder_.create<mlir::LLVM::ShlOp>(
          loc, initial_state64, c_i64(32)).getResult();
      mlir::Value w0 = builder_.create<mlir::LLVM::OrOp>(
          loc, count64, shifted_state).getResult();
      auto store_w0 = builder_.create<mlir::LLVM::StoreOp>(loc, w0, cursor);
      store_w0.setAlignment(8);
      auto store_w1 = builder_.create<mlir::LLVM::StoreOp>(
          loc, site_id64, byte_gep(cursor, 8));
      store_w1.setAlignment(8);
    };

    mlir::Value payload;
    if (fixed_geometry) {
      if (fixed_header == 8) {
        write_h8_header();
        payload = byte_gep(cursor, 8);
      } else {
        write_h16_header();
        payload = byte_gep(cursor, 16);
      }
    } else if (force_extended) {
      write_h16_header();
      payload = byte_gep(cursor, 16);
    } else if (max_multi_count == 0) {
      write_h8_header();
      payload = byte_gep(cursor, 8);
    } else {
      mlir::Block* h8_hdr_block = func_op_.addBlock();
      mlir::Block* h16_hdr_block = func_op_.addBlock();
      mlir::Block* commit_block = func_op_.addBlock();
      builder_.create<mlir::LLVM::CondBrOp>(
          loc, is_one_cond, h8_hdr_block, h16_hdr_block);
      builder_.setInsertionPointToStart(h8_hdr_block);
      write_h8_header();
      builder_.create<mlir::LLVM::BrOp>(loc, commit_block);
      builder_.setInsertionPointToStart(h16_hdr_block);
      write_h16_header();
      builder_.create<mlir::LLVM::BrOp>(loc, commit_block);
      builder_.setInsertionPointToStart(commit_block);
      payload = builder_.create<mlir::LLVM::GEPOp>(
          loc, ptr, i8, cursor, mlir::ValueRange{header_bytes}).getResult();
    }

    // Update per-mutator accounting and publish root. `tlab_active_range->end`
    // is synced lazily from `tlab_cursor` at publication/refill boundaries.
    mlir::Value pbytes_ptr = byte_gep(tls, offsetof(NrqGcFastTls, pending_bytes));
    auto pbytes_load = builder_.create<mlir::LLVM::LoadOp>(loc, i64, pbytes_ptr);
    pbytes_load.setAlignment(8);
    mlir::Value next_pbytes = builder_.create<mlir::LLVM::AddOp>(
        loc, pbytes_load.getResult(), payload_bytes).getResult();
    auto store_pbytes = builder_.create<mlir::LLVM::StoreOp>(
        loc, next_pbytes, pbytes_ptr);
    store_pbytes.setAlignment(8);

    mlir::Value pcount_ptr = byte_gep(tls, offsetof(NrqGcFastTls, pending_count));
    auto pcount_load = builder_.create<mlir::LLVM::LoadOp>(loc, i64, pcount_ptr);
    pcount_load.setAlignment(8);
    mlir::Value next_pcount = builder_.create<mlir::LLVM::AddOp>(
        loc, pcount_load.getResult(), c_i64(1)).getResult();
    auto store_pcount = builder_.create<mlir::LLVM::StoreOp>(
        loc, next_pcount, pcount_ptr);
    store_pcount.setAlignment(8);

    if (!cold_only_frame) {
      auto store_root = builder_.create<mlir::LLVM::StoreOp>(
          loc, payload, root_slot);
      store_root.setAlignment(8);
    }
    builder_.create<mlir::LLVM::BrOp>(
        loc, mlir::ValueRange{payload, cfg64}, merge_block);

    // Cold slow path: invoke existing planned runtime allocation entry point and
    // cache the resolved layout ID if this site is cold (`site_id == 0`).
    builder_.setInsertionPointToStart(slow_block);
    mlir::Value cold_marker = emit_cold_root_enter();
    mlir::Value plan = plan_value_for_slow(is_one_cond);
    mlir::Value slow_result = builder_.create<mlir::LLVM::CallOp>(
        loc, slow_fn,
        mlir::ValueRange{root_slot, count, element_size, table_pointer,
                         table_count, site.site_cache, plan}).getResult();
    mlir::Value need_site_id = builder_.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::eq, site_id, c_i32(0)).getResult();
    builder_.create<mlir::LLVM::CondBrOp>(
        loc, need_site_id, populate_site_block, slow_done_block);

    builder_.setInsertionPointToStart(populate_site_block);
    auto layout_load = builder_.create<mlir::LLVM::LoadOp>(
        loc, ptr, site.site_cache);
    layout_load.setAlignment(8);
    auto layout_id_fn = ensure_runtime_fn(
        module_, builder_, "nrq_gc_layout_id", i32, {ptr});
    mlir::Value resolved_id = builder_.create<mlir::LLVM::CallOp>(
        loc, layout_id_fn, mlir::ValueRange{layout_load.getResult()}).getResult();
    auto store_site_id = builder_.create<mlir::LLVM::StoreOp>(
        loc, resolved_id, site.site_id);
    store_site_id.setAlignment(4);
    store_site_id.setOrdering(mlir::LLVM::AtomicOrdering::release);
    builder_.create<mlir::LLVM::BrOp>(loc, slow_done_block);

    builder_.setInsertionPointToStart(slow_done_block);
    emit_cold_root_exit(cold_marker);
    mlir::Value slow_cfg64 = reload_inline_cfg64_uncached(loc);
    builder_.create<mlir::LLVM::BrOp>(
        loc, mlir::ValueRange{slow_result, slow_cfg64}, merge_block);

    builder_.setInsertionPointToStart(merge_block);
    set_inline_cfg_cache(merge_block, merge_block->getArgument(1));
    return merge_block->getArgument(0);
  }

  mlir::Value emit_planned_rooted_array(mlir::Location loc,
                                        mlir::Value root_slot,
                                        const Type& root_type,
                                        mlir::Value count,
                                        mlir::Value element_size,
                                        mlir::Value offsets_pointer,
                                        mlir::Value reference_count,
                                        bool force_extended = false) {
    return emit_inline_rooted_tlab_allocation(
        loc, root_slot, root_type, count, element_size, offsets_pointer,
        reference_count, "nrq_gc_allocate_array_planned", force_extended);
  }

  mlir::Value emit_planned_rooted_soa(mlir::Location loc,
                                      mlir::Value root_slot,
                                      const Type& root_type,
                                      mlir::Value count,
                                      mlir::Value element_size,
                                      mlir::Value pairs_pointer,
                                      mlir::Value pair_count) {
    return emit_inline_rooted_tlab_allocation(
        loc, root_slot, root_type, count, element_size, pairs_pointer,
        pair_count, "nrq_gc_allocate_soa_planned", /*force_extended=*/true);
  }

  void emit_gc_root_registration(mlir::Value slot, const Type& value_type,
                                 mlir::Location loc) {
    std::vector<uint64_t> offsets;
    collect_managed_reference_offsets(value_type, 0, offsets);
    if (offsets.empty()) return;
    const std::string symbol = "nrq_gc_root_register_typed";
    auto register_fn = ensure_runtime_fn(module_, builder_, symbol,
                                         mlir::LLVM::LLVMVoidType::get(ctx_),
                                         {llvm_ptr_type(ctx_), builder_.getI64Type(),
                                          llvm_ptr_type(ctx_), builder_.getI64Type()});
    mlir::Value offsets_slot = reference_layout_descriptor(offsets, loc);
    if (!offsets_slot) return;
    mlir::Value object_size = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI64Type(),
        builder_.getI64IntegerAttr(type_size(value_type))).getResult();
    mlir::Value reference_count = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI64Type(),
        builder_.getI64IntegerAttr(static_cast<int64_t>(offsets.size()))).getResult();
    builder_.create<mlir::LLVM::CallOp>(loc, register_fn,
        mlir::ValueRange{slot, object_size, offsets_slot, reference_count});
  }

  // Stack-placed explicit storage lives only for this activation. Its root must
  // be published after the frame begins so the matching frame end removes it;
  // registering at the current operation position would put it outside the
  // frame and leave a stale slot after return.
  void emit_frame_gc_root_registration(mlir::Value slot, const Type& value_type,
                                       mlir::Location loc) {
    has_late_root_registrations_ = true;
    mlir::Operation* slot_definition = slot.getDefiningOp();
    if (!slot_definition) {
      emit_gc_root_registration(slot, value_type, loc);
      return;
    }
    // Publish immediately after the storage address exists. The activation root
    // frame already begins before any operation in the body, so this is inside
    // the frame, and the slot operand dominates the registration.
    mlir::OpBuilder::InsertionGuard guard(builder_);
    builder_.setInsertionPointAfter(slot_definition);
    emit_gc_root_registration(slot, value_type, loc);
  }

  void emit_gc_root_unregistration(mlir::Value slot, const Type& value_type,
                                   mlir::Location loc) {
    std::vector<uint64_t> offsets;
    collect_managed_reference_offsets(value_type, 0, offsets);
    if (offsets.empty()) return;
    auto unregister_fn = ensure_runtime_fn(
        module_, builder_, "nrq_gc_root_unregister",
        mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
    builder_.create<mlir::LLVM::CallOp>(loc, unregister_fn,
                                        mlir::ValueRange{slot});
  }

  // The layout words of a frame descriptor and the derived per-root reference
  // counts, computed once. The emitter and the descriptor builder used to walk
  // the same types separately, so the words a frame publishes and the counts it
  // believes could drift apart; a single derivation makes an incomplete
  // descriptor impossible rather than unlikely.
  struct FrameRootLayout {
    llvm::SmallVector<int64_t, 8> words;
    std::vector<std::size_t> reference_counts;
    bool valid = false;
  };

  FrameRootLayout build_frame_root_layout(llvm::ArrayRef<PendingGcRoot> roots) {
    FrameRootLayout layout;
    // Word 0 is the slot count and word 1 is the total word count, so the
    // consumer can reject a table that ends early instead of reading past it.
    layout.words.push_back(static_cast<int64_t>(roots.size()));
    layout.words.push_back(0);  // patched once every root is described.
    layout.reference_counts.reserve(roots.size());
    for (const PendingGcRoot& root : roots) {
      std::vector<uint64_t> offsets;
      collect_managed_reference_offsets(root.type, 0, offsets);
      if (offsets.empty()) return layout;
      layout.words.push_back(static_cast<int64_t>(type_size(root.type)));
      layout.words.push_back(static_cast<int64_t>(offsets.size()));
      layout.reference_counts.push_back(offsets.size());
      for (uint64_t offset : offsets)
        layout.words.push_back(static_cast<int64_t>(offset));
    }
    layout.words[1] = static_cast<int64_t>(layout.words.size());
    // Every root contributes its size word, its count word and its offsets.
    std::size_t described = 2;
    for (std::size_t count : layout.reference_counts) described += 2 + count;
    layout.valid = layout.reference_counts.size() == roots.size() &&
                   described == static_cast<std::size_t>(layout.words.size());
    return layout;
  }

  // One read-only table per frame describing every root layout the activation
  // registers. The table is interned per module, so two functions that register
  // the same layouts share it and an activation pays no stores for metadata.
  mlir::Value frame_root_descriptor(const FrameRootLayout& layout,
                                    mlir::Location loc) {
    if (!layout.valid) return {};
    const llvm::SmallVector<int64_t, 8>& words = layout.words;
    std::string name = "nrq_gc_roots";
    for (int64_t word : words) name += "_" + std::to_string(word);
    if (!module_.lookupSymbol<mlir::LLVM::GlobalOp>(name)) {
      auto array_type = mlir::LLVM::LLVMArrayType::get(
          builder_.getI64Type(), static_cast<unsigned>(words.size()));
      auto initializer = mlir::DenseIntElementsAttr::get(
          mlir::RankedTensorType::get({static_cast<int64_t>(words.size())},
                                      builder_.getI64Type()),
          words);
      mlir::OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(module_.getBody());
      builder_.create<mlir::LLVM::GlobalOp>(loc, array_type, /*isConstant=*/true,
                                           mlir::LLVM::Linkage::Private, name,
                                           initializer, /*alignment=*/8);
    }
    return builder_.create<mlir::LLVM::AddressOfOp>(
        loc, llvm_ptr_type(ctx_), name).getResult();
  }

  // Publishes every pending root of this activation with one call: the
  // descriptor carries the layouts, a stack array carries the slot addresses.
  // Returns false when the caller has to fall back to per-slot registration.
  bool emit_gc_frame_root_registration(llvm::ArrayRef<PendingGcRoot> roots) {
    if (roots.size() < 2 || entry_block_ == nullptr) return false;
    for (const PendingGcRoot& root : roots) {
      if (!tir_type_to_mlir(root.type, ctx_, builder_)) return false;
    }
    FrameRootLayout layout = build_frame_root_layout(roots);
    if (!layout.valid) return false;
    mlir::Location loc = roots.front().loc;
    mlir::Value descriptor = frame_root_descriptor(layout, loc);
    if (!descriptor) return false;
    auto slots_type = mlir::LLVM::LLVMArrayType::get(
        llvm_ptr_type(ctx_), static_cast<unsigned>(roots.size()));
    mlir::Value slots = alloca_at_entry(slots_type, loc);
    if (!slots) return false;
    mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI32Type(), builder_.getI32IntegerAttr(0)).getResult();
    for (std::size_t index = 0; index < roots.size(); ++index) {
      mlir::Value position = builder_.create<mlir::LLVM::ConstantOp>(
          loc, builder_.getI64Type(),
          builder_.getI64IntegerAttr(static_cast<int64_t>(index))).getResult();
      mlir::Value element = builder_.create<mlir::LLVM::GEPOp>(
          loc, llvm_ptr_type(ctx_), slots_type, slots,
          mlir::ValueRange{zero, position}).getResult();
      builder_.create<mlir::LLVM::StoreOp>(loc, roots[index].slot, element);
    }
    mlir::Value count = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI64Type(),
        builder_.getI64IntegerAttr(static_cast<int64_t>(roots.size()))).getResult();
    // The descriptor is the constant global this function just emitted, so the
    // runtime does not have to re-validate it on every activation: the fused and
    // static entry points keep the development build's validation and drop it
    // from the production build.
    //
    // The frame was opened at function entry, before these slots could be
    // published. Opening it in the same call that publishes them is the same
    // sequence - frame record first, roots second - with one call and one
    // shadow-stack entry instead of two. The eager begin call is erased once
    // this call owns the frame record.
    mlir::Operation* begin_op =
        gc_root_frame_marker_ ? gc_root_frame_marker_.getDefiningOp() : nullptr;
    if (begin_op) {
      auto open_fn = ensure_runtime_fn(
          module_, builder_, "nrq_gc_root_frame_open",
          builder_.getI64Type(),
          {llvm_ptr_type(ctx_), builder_.getI64Type(), llvm_ptr_type(ctx_)});
      mlir::Value marker = builder_.create<mlir::LLVM::CallOp>(
          loc, open_fn, mlir::ValueRange{slots, count, descriptor}).getResult();
      // Every frame-end already emitted for this function reads the marker, and
      // they all sit in blocks dominated by this entry-block call.
      gc_root_frame_marker_.replaceAllUsesWith(marker);
      gc_root_frame_marker_ = marker;
      begin_op->erase();
      return true;
    }
    auto register_fn = ensure_runtime_fn(
        module_, builder_, "nrq_gc_root_register_frame_static",
        mlir::LLVM::LLVMVoidType::get(ctx_),
        {llvm_ptr_type(ctx_), builder_.getI64Type(), llvm_ptr_type(ctx_)});
    builder_.create<mlir::LLVM::CallOp>(
        loc, register_fn, mlir::ValueRange{slots, count, descriptor});
    return true;
  }

  void emit_gc_array_root_registration(mlir::Value data, mlir::Value count,
                                       const Type& element_type,
                                       mlir::Location loc) {
    std::vector<uint64_t> offsets;
    collect_managed_reference_offsets(element_type, 0, offsets);
    if (offsets.empty()) return;
    const mlir::Type i64 = builder_.getI64Type();
    // The element layout is a property of the element type, so the array path
    // shares the same read-only descriptor the scalar path uses. Building it in
    // a stack slot cost an alloca and one store per offset on every
    // registration of every managed array.
    mlir::Value offsets_slot = reference_layout_descriptor(offsets, loc);
    if (!offsets_slot) return;
    auto register_fn = ensure_runtime_fn(
        module_, builder_, "nrq_gc_root_register_array",
        mlir::LLVM::LLVMVoidType::get(ctx_),
        {llvm_ptr_type(ctx_), i64, i64, llvm_ptr_type(ctx_), i64});
    mlir::Value element_size = builder_.create<mlir::LLVM::ConstantOp>(
        loc, i64, builder_.getI64IntegerAttr(type_size(element_type))).getResult();
    mlir::Value offset_count = builder_.create<mlir::LLVM::ConstantOp>(
        loc, i64,
        builder_.getI64IntegerAttr(static_cast<int64_t>(offsets.size()))).getResult();
    builder_.create<mlir::LLVM::CallOp>(
        loc, register_fn,
        mlir::ValueRange{data, count, element_size, offsets_slot, offset_count});
  }

  mlir::Value pin_managed_references_for_return(
      mlir::Value value, const Type& type, mlir::Location loc) {
    std::vector<uint64_t> offsets;
    collect_managed_reference_offsets(type, 0, offsets);
    if (offsets.empty() || !value) return value;
    if (type.is_managed_pointer() && offsets.size() == 1 && offsets[0] == 0) {
      emit_inline_pin_transfer(value, loc);
      return value;
    }
    mlir::Type value_type = tir_type_to_mlir(type, ctx_, builder_);
    if (!value_type) return value;
    mlir::Value slot = alloca_at_entry(value_type, loc);
    builder_.create<mlir::LLVM::StoreOp>(loc, value, slot);
    auto pin = ensure_runtime_fn(
        module_, builder_, "nrq_gc_pin_transfer", llvm_ptr_type(ctx_),
        {llvm_ptr_type(ctx_)});
    for (uint64_t offset : offsets) {
      mlir::Value byte_offset = builder_.create<mlir::LLVM::ConstantOp>(
          loc, builder_.getI64Type(),
          builder_.getI64IntegerAttr(static_cast<int64_t>(offset))).getResult();
      mlir::Value field = builder_.create<mlir::LLVM::GEPOp>(
          loc, llvm_ptr_type(ctx_), builder_.getI8Type(), slot,
          mlir::ValueRange{byte_offset}).getResult();
      mlir::Value reference = builder_.create<mlir::LLVM::LoadOp>(
          loc, llvm_ptr_type(ctx_), field).getResult();
      builder_.create<mlir::LLVM::CallOp>(
          loc, pin, mlir::ValueRange{reference});
    }
    return builder_.create<mlir::LLVM::LoadOp>(loc, value_type, slot).getResult();
  }

  // A callee hands a managed result back pinned (`nrq_gc_pin_transfer`); the
  // caller either publishes it into a registered root or, when the result is
  // provably dead at every future collection point, only releases the pin.
  void root_managed_references_and_release_handoffs(
      mlir::Value value, const Type& type, mlir::Location loc, bool keep_root) {
    std::vector<uint64_t> offsets;
    collect_managed_reference_offsets(type, 0, offsets);
    if (offsets.empty() || !value) return;
    mlir::Type value_type = tir_type_to_mlir(type, ctx_, builder_);
    if (!value_type) return;
    // A managed reference is itself the payload of the barrier, so the root that
    // owns it can receive the reference directly. Staging it in another slot and
    // copying eight bytes out of that slot cost an alloca, a store and a copy
    // call for a value the barrier can take as an argument.
    const bool single_pointer = type.is_managed_pointer() &&
                                offsets.size() == 1 && offsets[0] == 0;
    const bool direct_pointer_root = keep_root && single_pointer;
    mlir::Value slot;
    if (direct_pointer_root) {
      slot = alloca_at_entry(value_type, loc);
      register_gc_root(slot, type, loc);
      emit_inline_root_store(slot, value, loc, /*known_stack_slot=*/true);
    } else if (!single_pointer) {
      slot = alloca_at_entry(value_type, loc);
      builder_.create<mlir::LLVM::StoreOp>(loc, value, slot);
      if (keep_root) {
        mlir::Value root_slot = alloca_at_entry(value_type, loc);
        register_gc_root(root_slot, type, loc);
        auto copy = ensure_runtime_fn(
            module_, builder_, "nrq_gc_root_copy",
            mlir::LLVM::LLVMVoidType::get(ctx_),
            {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_), builder_.getI64Type()});
        mlir::Value byte_count = builder_.create<mlir::LLVM::ConstantOp>(
            loc, builder_.getI64Type(),
            builder_.getI64IntegerAttr(static_cast<int64_t>(type_size(type)))).getResult();
        builder_.create<mlir::LLVM::CallOp>(
            loc, copy, mlir::ValueRange{root_slot, slot, byte_count});
        slot = root_slot;
      }
    }
    if (single_pointer) {
      // The root holds exactly the value that was just published into it, so the
      // release needs no reload of the slot it was stored into.
      emit_inline_unpin(value, loc);
      return;
    }
    auto unpin = ensure_runtime_fn(
        module_, builder_, "nrq_gc_unpin",
        mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
    for (uint64_t offset : offsets) {
      mlir::Value byte_offset = builder_.create<mlir::LLVM::ConstantOp>(
          loc, builder_.getI64Type(),
          builder_.getI64IntegerAttr(static_cast<int64_t>(offset))).getResult();
      mlir::Value field = builder_.create<mlir::LLVM::GEPOp>(
          loc, llvm_ptr_type(ctx_), builder_.getI8Type(), slot,
          mlir::ValueRange{byte_offset}).getResult();
      mlir::Value reference = builder_.create<mlir::LLVM::LoadOp>(
          loc, llvm_ptr_type(ctx_), field).getResult();
      builder_.create<mlir::LLVM::CallOp>(
          loc, unpin, mlir::ValueRange{reference});
    }
  }

  // Converts a runtime call's ABI result to the type the language assigned to
  // the call, e.g. an i32 status code to a 64-bit `int` or a 0/1 code to
  // `bool`. A no-op when the types already match.
  mlir::Value adapt_runtime_result(mlir::Value result, const Type& wanted_type,
                                   mlir::Location loc) {
    if (!result) return result;
    mlir::Type wanted = tir_type_to_mlir(wanted_type, ctx_, builder_);
    if (!wanted || wanted == result.getType()) return result;

    if (!mlir::isa<mlir::IntegerType>(result.getType()) ||
        !mlir::isa<mlir::IntegerType>(wanted)) {
      return result;
    }

    const unsigned result_bits = result.getType().getIntOrFloatBitWidth();
    const unsigned wanted_bits = wanted.getIntOrFloatBitWidth();

    // A 1-bit language type is `bool`; the runtime reports it as 0/1.
    if (wanted_bits == 1 && result_bits > 1) {
      mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
          loc, result.getType(), builder_.getIntegerAttr(result.getType(), 0)).getResult();
      return builder_.create<mlir::LLVM::ICmpOp>(loc, mlir::LLVM::ICmpPredicate::ne, result, zero)
          .getResult();
    }
    if (wanted_bits == result_bits) return result;
    if (wanted_bits < result_bits) {
      return builder_.create<mlir::LLVM::TruncOp>(loc, wanted, result).getResult();
    }
    return wanted_type.is_unsigned_integer()
               ? builder_.create<mlir::LLVM::ZExtOp>(loc, wanted, result).getResult()
               : builder_.create<mlir::LLVM::SExtOp>(loc, wanted, result).getResult();
  }

  // Emits (once per distinct literal) a private global holding `text` plus a
  // terminating NUL and returns a pointer to its first byte.
  mlir::Value materialize_string(const std::string& text, mlir::Location loc) {
    auto cached = string_literals_.symbols.find(text);
    std::string symbol;
    if (cached != string_literals_.symbols.end()) {
      symbol = cached->second;
    } else {
      symbol = "__nrq_strlit_" + std::to_string(string_literals_.counter++);
      const std::string stored = text + '\0';
      auto array_type = mlir::LLVM::LLVMArrayType::get(
          builder_.getI8Type(), static_cast<unsigned>(stored.size()));

      // Globals live at module scope, not at the current insertion point.
      mlir::OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(module_.getBody());
      builder_.create<mlir::LLVM::GlobalOp>(loc, array_type, /*isConstant=*/true,
                                            mlir::LLVM::Linkage::Private, symbol,
                                            builder_.getStringAttr(stored));
      string_literals_.symbols.emplace(text, symbol);
    }
    return builder_.create<mlir::LLVM::AddressOfOp>(loc, llvm_ptr_type(ctx_), symbol)
        .getResult();
  }

  // -- Development-only allocation-site instrumentation ---------------------
  //
  // A site id is a stable hash of the source position, so the id for unchanged
  // code is the same in every build and in every translation unit; the runtime
  // is the only side that keeps a table. Nothing here runs unless the program
  // was compiled with --debug-enabled, and the flag also selects the runtime
  // that provides these entry points.
  static uint64_t gc_site_id(const SourceLocation& location) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);  // FNV-1a offset basis
    for (unsigned char byte : location.file) {
      hash ^= byte;
      hash *= UINT64_C(0x100000001b3);
    }
    hash ^= static_cast<uint64_t>(location.line);
    hash *= UINT64_C(0x100000001b3);
    hash ^= static_cast<uint64_t>(location.column);
    hash *= UINT64_C(0x100000001b3);
    // Zero is the runtime's "no site" value.
    return hash == 0 ? 1 : hash;
  }

  void emit_gc_site_declaration(uint64_t site, const SourceLocation& location,
                                mlir::Location loc) {
    auto fn = ensure_runtime_fn(module_, builder_, "nrq_gc_debug_declare_site",
                                mlir::LLVM::LLVMVoidType::get(ctx_),
                                {builder_.getI64Type(), llvm_ptr_type(ctx_),
                                 builder_.getI64Type(), builder_.getI32Type()});
    auto constant_i64 = [&](uint64_t value) {
      return builder_
          .create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(static_cast<int64_t>(value)))
          .getResult();
    };
    mlir::Value id = constant_i64(site);
    mlir::Value file = materialize_string(location.file, loc);
    mlir::Value length = constant_i64(location.file.size());
    mlir::Value line = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI32Type(),
        builder_.getI32IntegerAttr(static_cast<int32_t>(location.line))).getResult();
    builder_.create<mlir::LLVM::CallOp>(loc, fn,
                                       mlir::ValueRange{id, file, length, line});
  }

  // Records which source site produced `object`. The runtime keeps the record
  // next to the object's address, so a trace can attribute every tagged
  // allocation without the object carrying a tag of its own.
  void emit_gc_allocation_tag(mlir::Value object, uint64_t site,
                              mlir::Location loc) {
    auto fn = ensure_runtime_fn(module_, builder_, "nrq_gc_debug_tag_allocation",
                                mlir::LLVM::LLVMVoidType::get(ctx_),
                                {llvm_ptr_type(ctx_), builder_.getI64Type()});
    mlir::Value id = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI64Type(),
        builder_.getI64IntegerAttr(static_cast<int64_t>(site))).getResult();
    builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{object, id});
  }

  mlir::Block* get_or_create_block(int label_id) {
    auto it = label_blocks_.find(label_id);
    if (it != label_blocks_.end()) return it->second;
    mlir::Block* block = func_op_.addBlock();
    label_blocks_[label_id] = block;
    return block;
  }

  bool translate_op(const tir::Op& op) {
    mlir::Location loc = to_mlir_loc(op.location, ctx_);

    switch (op.kind) {
      case tir::OpKind::Label: {
        mlir::Block* target_block = get_or_create_block(op.label_id);
        mlir::Block* current = builder_.getBlock();
        const bool falls_through =
            current != nullptr &&
            (current->empty() ||
             !current->back().hasTrait<mlir::OpTrait::IsTerminator>());
        // A loop-scoped region is created on the edge into its header, so it runs
        // once per entry into the loop and no path inside the loop sees a missing
        // handle. When the front end branched here explicitly that branch already
        // carried the create, and this block is not the edge.
        if (falls_through) emit_loop_header_creates(op.label_id);
        if (falls_through) builder_.create<mlir::LLVM::BrOp>(loc, target_block);
        builder_.setInsertionPointToStart(target_block);
        return true;
      }

      case tir::OpKind::Branch: {
        mlir::Block* target_block = get_or_create_block(op.label_id);
        // The front end branches to a loop header explicitly, so the entry edge
        // that carries a loop region's create is usually this branch rather than
        // a fall-through. A back edge re-enters the loop it is already inside and
        // creates nothing.
        if (!is_backedge(op.label_id)) emit_loop_header_creates(op.label_id);
        // An edge that leaves the loop is where a loop-scoped region dies, so a
        // loop reclaims its storage at its own exit rather than at the end of the
        // activation.
        emit_loop_region_destroys(current_operation_index_, loc, {op.label_id});
        if (is_backedge(op.label_id) && emit_gc_safepoint(loc, target_block))
          return true;
        builder_.create<mlir::LLVM::BrOp>(loc, target_block);
        return true;
      }

      case tir::OpKind::BranchCond: {
        mlir::Block* true_block = get_or_create_block(op.true_label);
        mlir::Block* false_block = get_or_create_block(op.false_label);
        mlir::Value cond = get(op.operand);
        if (is_backedge(op.true_label) || is_backedge(op.false_label))
          emit_gc_safepoint(loc);
        // A conditional edge cannot carry the destroy inline: the edge that stays
        // in the loop has to keep the region alive. Each leaving edge therefore
        // branches through a block of its own that destroys and continues.
        if (label_leaves_loop(current_operation_index_, op.true_label))
          true_block = destroy_edge_block(loc, true_block, op.true_label);
        if (label_leaves_loop(current_operation_index_, op.false_label))
          false_block = destroy_edge_block(loc, false_block, op.false_label);
        builder_.create<mlir::LLVM::CondBrOp>(loc, cond, true_block, false_block);
        return true;
      }

      case tir::OpKind::ConstantInt: {
        mlir::Type ty = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!ty) ty = builder_.getI64Type();
        mlir::Value v = builder_.create<mlir::LLVM::ConstantOp>(
            loc, ty, builder_.getIntegerAttr(ty, op.integer_value)).getResult();
        set(op.result, v);
        return true;
      }

      case tir::OpKind::ConstantNull: {
        mlir::Type ty = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!ty) {
          diag_.error("NRQ4005", op.location,
                      "internal: null constant has an unsupported type");
          return false;
        }
        set(op.result, builder_.create<mlir::LLVM::ZeroOp>(loc, ty).getResult());
        return true;
      }

      case tir::OpKind::ConstantFloat: {
        mlir::Type ty = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!ty) ty = builder_.getF64Type();
        mlir::Value v = builder_.create<mlir::LLVM::ConstantOp>(
            loc, ty, builder_.getFloatAttr(ty, op.float_value)).getResult();
        set(op.result, v);
        return true;
      }

      case tir::OpKind::ConstantString: {
        // A string literal becomes the address of a private, NUL-terminated
        // byte array, which is what the runtime ABI expects for `const char*`.
        set(op.result, materialize_string(op.string_value, loc));
        return true;
      }

      case tir::OpKind::AddressOf: {
        // The slot handle already denotes the alloca, so the address is that
        // pointer itself: no load and no copy of the value in the slot.
        mlir::Value slot = get(op.operand);
        if (!slot) return true;
        set(op.result, slot);
        return true;
      }
      case tir::OpKind::Alloca: {
        // A promoted parameter slot has no storage: the slot name is bound to the
        // incoming argument, so the loads below read it directly.
        if (auto promoted = promoted_slots_.slots.find(op.result.id);
            promoted != promoted_slots_.slots.end()) {
          set(op.result, get(tir::Value{promoted->second}));
          return true;
        }
        mlir::Type elem_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!elem_type) {
          diag_.error("NRQ4005", op.location, "internal: alloca of unsupported type");
          return false;
        }
        // A managed local initialised directly from a container allocation is
        // declared after that allocation, which already created its storage so
        // the object could be published into it. Reuse that slot.
        mlir::Value slot;
        if (auto precreated = precreated_slots_.find(op.result.id);
            precreated != precreated_slots_.end()) {
          slot = precreated->second;
          precreated_slots_.erase(precreated);
        } else {
          slot = alloca_at_entry(elem_type, loc);
        }
        if (!slot) {
          diag_.error("NRQ4005", op.location, "internal: no entry block for a stack slot");
          return false;
        }
        record_pointer_alignment(op.result, type_alignment(op.result.type));
        std::vector<uint64_t> root_offsets;
        collect_managed_reference_offsets(op.result.type, 0, root_offsets);
        if (!root_offsets.empty()) {
          const bool rooted = gc_roots_.rooted_slots.count(op.result.id) != 0;
          // A slot whose contents no collection can observe still gets a
          // deterministic zero value, but it is not registered as a root: the
          // slot is then only reached by plain loads and stores, so mem2reg and
          // SROA can promote it like an ordinary local.
          //
          // A rooted slot is different: the frame preamble clears every pending
          // root slot before it publishes the frame, and the slot is fresh, so
          // nothing can have written it before that preamble. Zeroing it again
          // here repeats a store whose effect is already in the slot; the only
          // earlier write is the preamble's, which dominates every block.
          // Without a frame the preamble does not run, so the store stays.
          if (!rooted || !gc_root_frame_marker_) {
            mlir::Value zero_value = builder_.create<mlir::LLVM::ZeroOp>(
                loc, elem_type).getResult();
            builder_.create<mlir::LLVM::StoreOp>(loc, zero_value, slot);
          }
          if (rooted) register_gc_root(slot, op.result.type, loc);
        }
        set(op.result, slot);
        return true;
      }

      case tir::OpKind::Store: {
        // The initialization of a promoted slot is the binding itself, and the
        // analysis proved nothing else writes the slot.
        if (promoted_slots_.slots.count(op.lhs.id) != 0) return true;
        mlir::Value ptr = get(op.lhs);
        mlir::Value val = get(op.operand);
        if (!ptr || !val) return true;
        // A destination no collection can observe needs neither a write barrier
        // nor a root copy. Keeping the store plain also leaves the slot free for
        // SSA promotion instead of making every access a runtime call.
        const bool unobserved =
            gc_roots_.unobserved_addresses.count(op.lhs.id) != 0 ||
            (!has_collection_point_ &&
             unobserved_stack_owned_addresses_.count(op.lhs.id) != 0);
        if (op.operand.type.is_managed_pointer()) {
          if (unobserved) {
            apply_access_alignment(
                builder_.create<mlir::LLVM::StoreOp>(loc, val, ptr),
                op.lhs, op.operand.type);
            forwarded_root_values_.erase(op.lhs.id);
            return true;
          }
          emit_inline_root_store(
              ptr, val, loc,
              ptr.getDefiningOp<mlir::LLVM::AllocaOp>() != nullptr, &op.lhs,
              &op.operand.type);
          // The barrier writes exactly this reference into the slot before it
          // returns, so a later load of the same address can use the value
          // directly. Without this the emitted code reloads the pointer it just
          // stored, and the call in between stops LLVM from forwarding it.
          forwarded_root_values_[op.lhs.id] = val;
        } else if (!collect_has_managed_reference(op.operand.type) || unobserved) {
          forwarded_root_values_.erase(op.lhs.id);
          apply_access_alignment(
              builder_.create<mlir::LLVM::StoreOp>(loc, val, ptr),
              op.lhs, op.operand.type);
        } else {
          // The barrier needs the value in memory. Allocating at entry rather
          // than here keeps a store inside a loop from growing the frame once
          // per iteration, and puts the slot where mem2reg can see it.
          mlir::Value temporary = alloca_at_entry(val.getType(), loc);
          if (!temporary) {
            diag_.error("NRQ4006", op.location,
                        "internal: no entry block for an aggregate store temporary");
            return false;
          }
          builder_.create<mlir::LLVM::StoreOp>(loc, val, temporary);
          auto copy_fn = ensure_runtime_fn(module_, builder_, "nrq_gc_root_copy",
                                           mlir::LLVM::LLVMVoidType::get(ctx_),
                                           {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_),
                                            builder_.getI64Type()});
          mlir::Value bytes = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(type_size(op.operand.type))).getResult();
          builder_.create<mlir::LLVM::CallOp>(loc, copy_fn,
                                              mlir::ValueRange{ptr, temporary, bytes});
        }
        return true;
      }

      case tir::OpKind::Load: {
        if (auto promoted = promoted_slots_.slots.find(op.lhs.id);
            promoted != promoted_slots_.slots.end()) {
          set(op.result, get(tir::Value{promoted->second}));
          return true;
        }
        if (auto forwarded = forwarded_root_values_.find(op.lhs.id);
            forwarded != forwarded_root_values_.end()) {
          set(op.result, forwarded->second);
          return true;
        }
        mlir::Value ptr = get(op.lhs);
        if (!ptr) return true;
        mlir::Type elem_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!elem_type) {
          diag_.error("NRQ4005", op.location, "internal: load of unsupported type");
          return false;
        }
        auto load = builder_.create<mlir::LLVM::LoadOp>(loc, elem_type, ptr);
        apply_access_alignment(load, op.lhs, op.result.type);
        set(op.result, load.getResult());
        return true;
      }

      case tir::OpKind::Unary: {
        mlir::Value val = get(op.operand);
        if (!val) return true;
        mlir::Value result;
        const Type& ty = op.operand.type;

        switch (op.unary_op) {
          case tir::UnaryOp::Negate:
            if (ty.is_float()) {
              result = builder_.create<mlir::LLVM::FNegOp>(loc, val).getResult();
            } else {
              mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
                  loc, val.getType(), builder_.getIntegerAttr(val.getType(), 0)).getResult();
              result = builder_.create<mlir::LLVM::SubOp>(loc, zero, val).getResult();
            }
            break;
          case tir::UnaryOp::Plus:
            result = val;
            break;
          case tir::UnaryOp::LogicalNot: {
            mlir::Value true_val = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI1Type(), builder_.getBoolAttr(true)).getResult();
            result = builder_.create<mlir::LLVM::XOrOp>(loc, val, true_val).getResult();
            break;
          }
          case tir::UnaryOp::BitwiseNot: {
            mlir::Value all_ones = builder_.create<mlir::LLVM::ConstantOp>(
                loc, val.getType(), builder_.getIntegerAttr(val.getType(), -1)).getResult();
            result = builder_.create<mlir::LLVM::XOrOp>(loc, val, all_ones).getResult();
            break;
          }
        }
        set(op.result, result);
        return true;
      }

      case tir::OpKind::Binary: {
        mlir::Value lhs = get(op.lhs);
        mlir::Value rhs = get(op.rhs);
        if (!lhs || !rhs) return true;
        mlir::Value result;
        const Type& lhs_type = op.lhs.type;

        if (op.binary_op == tir::BinaryOp::Exponentiate) {
          if (lhs_type.is_float()) {
            auto pow_fn = ensure_runtime_fn(module_, builder_, "nrq_pow_f64",
                                            builder_.getF64Type(),
                                            {builder_.getF64Type(), builder_.getF64Type()});
            mlir::Value l_f64 = lhs_type.is_f32()
                ? builder_.create<mlir::LLVM::FPExtOp>(loc, builder_.getF64Type(), lhs).getResult()
                : lhs;
            mlir::Value r_f64 = op.rhs.type.is_f32()
                ? builder_.create<mlir::LLVM::FPExtOp>(loc, builder_.getF64Type(), rhs).getResult()
                : rhs;
            result = builder_.create<mlir::LLVM::CallOp>(
                loc, mlir::TypeRange{builder_.getF64Type()},
                mlir::FlatSymbolRefAttr::get(ctx_, pow_fn.getSymName()),
                mlir::ValueRange{l_f64, r_f64}).getResult();
            if (lhs_type.is_f32()) {
              result = builder_.create<mlir::LLVM::FPTruncOp>(loc, builder_.getF32Type(), result).getResult();
            }
          } else {
            auto pow_fn = ensure_runtime_fn(module_, builder_, "nrq_pow_i64",
                                            builder_.getI64Type(),
                                            {builder_.getI64Type(), builder_.getI64Type()});
            mlir::Value l_i64 = lhs;
            if (lhs_type.bit_width() < 64) {
              l_i64 = lhs_type.is_signed_integer()
                  ? builder_.create<mlir::LLVM::SExtOp>(loc, builder_.getI64Type(), lhs).getResult()
                  : builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), lhs).getResult();
            }
            mlir::Value r_i64 = rhs;
            if (op.rhs.type.bit_width() < 64) {
              r_i64 = op.rhs.type.is_signed_integer()
                  ? builder_.create<mlir::LLVM::SExtOp>(loc, builder_.getI64Type(), rhs).getResult()
                  : builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), rhs).getResult();
            }
            result = builder_.create<mlir::LLVM::CallOp>(
                loc, mlir::TypeRange{builder_.getI64Type()},
                mlir::FlatSymbolRefAttr::get(ctx_, pow_fn.getSymName()),
                mlir::ValueRange{l_i64, r_i64}).getResult();
            if (lhs_type.bit_width() < 64) {
              mlir::Type orig_ty = tir_type_to_mlir(lhs_type, ctx_, builder_);
              result = builder_.create<mlir::LLVM::TruncOp>(loc, orig_ty, result).getResult();
            }
          }
          set(op.result, result);
          return true;
        }

        if (lhs_type.is_float()) {
          switch (op.binary_op) {
            case tir::BinaryOp::Add: result = create_fp_binary<mlir::LLVM::FAddOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::Subtract: result = create_fp_binary<mlir::LLVM::FSubOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::Multiply: result = create_fp_binary<mlir::LLVM::FMulOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::Divide: result = create_fp_binary<mlir::LLVM::FDivOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::Remainder: result = builder_.create<mlir::LLVM::FRemOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Equal:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::oeq, lhs, rhs).getResult(); break;
            case tir::BinaryOp::NotEqual:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::one, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Less:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::olt, lhs, rhs).getResult(); break;
            case tir::BinaryOp::LessEqual:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::ole, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Greater:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::ogt, lhs, rhs).getResult(); break;
            case tir::BinaryOp::GreaterEqual:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::oge, lhs, rhs).getResult(); break;
            default: break;
          }
        } else {
          const bool is_signed = lhs_type.is_signed_integer();
          switch (op.binary_op) {
            case tir::BinaryOp::Add: result = builder_.create<mlir::LLVM::AddOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Subtract: result = builder_.create<mlir::LLVM::SubOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Multiply: result = builder_.create<mlir::LLVM::MulOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Divide:
              result = is_signed ? builder_.create<mlir::LLVM::SDivOp>(loc, lhs, rhs).getResult()
                                 : builder_.create<mlir::LLVM::UDivOp>(loc, lhs, rhs).getResult();
              break;
            case tir::BinaryOp::Remainder:
              result = is_signed ? builder_.create<mlir::LLVM::SRemOp>(loc, lhs, rhs).getResult()
                                 : builder_.create<mlir::LLVM::URemOp>(loc, lhs, rhs).getResult();
              break;
            case tir::BinaryOp::BitwiseAnd: result = builder_.create<mlir::LLVM::AndOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::BitwiseOr: result = builder_.create<mlir::LLVM::OrOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::BitwiseXor: result = builder_.create<mlir::LLVM::XOrOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::ShiftLeft: result = builder_.create<mlir::LLVM::ShlOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::ShiftRight:
              result = is_signed ? builder_.create<mlir::LLVM::AShrOp>(loc, lhs, rhs).getResult()
                                 : builder_.create<mlir::LLVM::LShrOp>(loc, lhs, rhs).getResult();
              break;
            case tir::BinaryOp::Equal:
              result = builder_.create<mlir::LLVM::ICmpOp>(loc, mlir::LLVM::ICmpPredicate::eq, lhs, rhs).getResult(); break;
            case tir::BinaryOp::NotEqual:
              result = builder_.create<mlir::LLVM::ICmpOp>(loc, mlir::LLVM::ICmpPredicate::ne, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Less:
              result = builder_.create<mlir::LLVM::ICmpOp>(
                  loc, is_signed ? mlir::LLVM::ICmpPredicate::slt : mlir::LLVM::ICmpPredicate::ult, lhs, rhs).getResult();
              break;
            case tir::BinaryOp::LessEqual:
              result = builder_.create<mlir::LLVM::ICmpOp>(
                  loc, is_signed ? mlir::LLVM::ICmpPredicate::sle : mlir::LLVM::ICmpPredicate::ule, lhs, rhs).getResult();
              break;
            case tir::BinaryOp::Greater:
              result = builder_.create<mlir::LLVM::ICmpOp>(
                  loc, is_signed ? mlir::LLVM::ICmpPredicate::sgt : mlir::LLVM::ICmpPredicate::ugt, lhs, rhs).getResult();
              break;
            case tir::BinaryOp::GreaterEqual:
              result = builder_.create<mlir::LLVM::ICmpOp>(
                  loc, is_signed ? mlir::LLVM::ICmpPredicate::sge : mlir::LLVM::ICmpPredicate::uge, lhs, rhs).getResult();
              break;
            default: break;
          }
        }
        set(op.result, result);
        return true;
      }

      case tir::OpKind::Cast: {
        mlir::Value val = get(op.operand);
        if (!val) return true;
        mlir::Type target_ty = tir_type_to_mlir(op.result.type, ctx_, builder_);
        mlir::Value result;

        switch (op.cast_kind) {
          case tir::CastKind::Trunc:
            result = builder_.create<mlir::LLVM::TruncOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::SExt:
            result = builder_.create<mlir::LLVM::SExtOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::ZExt:
          case tir::CastKind::BoolToInt:
          case tir::CastKind::CharToInt:
            if (target_ty.getIntOrFloatBitWidth() < val.getType().getIntOrFloatBitWidth()) {
              result = builder_.create<mlir::LLVM::TruncOp>(loc, target_ty, val).getResult();
            } else if (target_ty.getIntOrFloatBitWidth() > val.getType().getIntOrFloatBitWidth()) {
              result = builder_.create<mlir::LLVM::ZExtOp>(loc, target_ty, val).getResult();
            } else {
              result = val;
            }
            break;
          case tir::CastKind::IntToChar:
            if (val.getType().getIntOrFloatBitWidth() > 32) {
              result = builder_.create<mlir::LLVM::TruncOp>(loc, target_ty, val).getResult();
            } else if (val.getType().getIntOrFloatBitWidth() < 32) {
              result = builder_.create<mlir::LLVM::ZExtOp>(loc, target_ty, val).getResult();
            } else {
              result = val;
            }
            break;
          case tir::CastKind::SIToFP:
            result = builder_.create<mlir::LLVM::SIToFPOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::UIToFP:
            result = builder_.create<mlir::LLVM::UIToFPOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::FPToSI:
            result = builder_.create<mlir::LLVM::FPToSIOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::FPToUI:
            result = builder_.create<mlir::LLVM::FPToUIOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::FPExt:
            result = builder_.create<mlir::LLVM::FPExtOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::FPTrunc:
            result = builder_.create<mlir::LLVM::FPTruncOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::IntToBool: {
            mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
                loc, val.getType(), builder_.getIntegerAttr(val.getType(), 0)).getResult();
            result = builder_.create<mlir::LLVM::ICmpOp>(loc, mlir::LLVM::ICmpPredicate::ne, val, zero).getResult();
            break;
          }
        }
        set(op.result, result);
        return true;
      }

      case tir::OpKind::Call: {
        std::vector<mlir::Value> args;
        args.reserve(op.arguments.size());
        for (const tir::Value arg : op.arguments) {
          mlir::Value arg_val = get(arg);
          if (arg_val) args.push_back(arg_val);
        }

        // A C symbol has no TIR signature to read, so the declaration emitted here
        // is the ABI the call site agrees on. The ownership handoff is the one
        // place the argument type and the parameter type differ: an owned
        // aggregate is passed as its buffer pointer alone, because length and
        // capacity are Nirqoba's bookkeeping and mean nothing to C. The caller's
        // destruction obligation was already discharged in sema, so this is the
        // only release the buffer gets.
        if (op.external_call) {
          std::vector<mlir::Type> abi_parameter_types;
          abi_parameter_types.reserve(op.external_parameter_types.size());
          llvm::SmallVector<mlir::Value> abi_args;
          abi_args.reserve(args.size());
          for (size_t index = 0; index < args.size(); ++index) {
            const bool described = index < op.external_parameter_types.size();
            if (described && op.external_parameter_types[index].is_dynamic_array()) {
              // The declaration asked for the container itself, so the whole
              // header crosses the boundary: the buffer, then the length and the
              // capacity as separate scalars. C gets the bounds it needs to
              // respect, and no struct layout has to be agreed on.
              if (!mlir::isa<mlir::LLVM::LLVMStructType>(args[index].getType())) {
                diag_.error("NRQ4005", op.location,
                            "internal: a destructured extern argument is not a "
                            "container header value");
                return false;
              }
              for (const int64_t field : {int64_t{0}, int64_t{1}, int64_t{2}}) {
                mlir::Value part = builder_
                                       .create<mlir::LLVM::ExtractValueOp>(
                                           loc, args[index],
                                           builder_.getDenseI64ArrayAttr({field}))
                                       .getResult();
                abi_args.push_back(part);
                abi_parameter_types.push_back(part.getType());
              }
              continue;
            }
            mlir::Type target = described
                ? tir_type_to_mlir(op.external_parameter_types[index], ctx_, builder_)
                : mlir::Type{};
            if (!target) target = args[index].getType();
            abi_parameter_types.push_back(target);
            mlir::Value value = args[index];
            if (value.getType() != target) {
              if (described && op.external_parameter_types[index].is_pointer() &&
                  mlir::isa<mlir::LLVM::LLVMStructType>(value.getType())) {
                // Field 0 is the buffer; the descriptor's length and capacity stay
                // with Nirqoba.
                value = builder_
                            .create<mlir::LLVM::ExtractValueOp>(
                                loc, value, builder_.getDenseI64ArrayAttr({0}))
                            .getResult();
              } else if (mlir::isa<mlir::IntegerType>(value.getType()) &&
                         mlir::isa<mlir::IntegerType>(target)) {
                const unsigned from = value.getType().getIntOrFloatBitWidth();
                const unsigned to = target.getIntOrFloatBitWidth();
                value = from < to
                    ? builder_.create<mlir::LLVM::ZExtOp>(loc, target, value).getResult()
                    : builder_.create<mlir::LLVM::TruncOp>(loc, target, value).getResult();
              }
            }
            abi_args.push_back(value);
          }
          mlir::Type result_type = op.result.is_valid()
              ? tir_type_to_mlir(op.result.type, ctx_, builder_)
              : mlir::Type(mlir::LLVM::LLVMVoidType::get(ctx_));
          auto foreign_fn = ensure_runtime_fn(module_, builder_, op.callee,
                                              result_type, abi_parameter_types);
          auto foreign_call = builder_.create<mlir::LLVM::CallOp>(
              loc, foreign_fn, abi_args);
          if (op.result.is_valid() && foreign_call->getNumResults() != 0) {
            set(op.result, adapt_runtime_result(foreign_call.getResult(),
                                                op.result.type, loc));
          }
          return true;
        }

        // A container is one GC object whose payload is every column, packed in
        // physical field order. `describe_soa` is the only authority for that
        // geometry: the byte extent is count * element_bytes, and the element size
        // passed to the runtime is that same element_bytes, so the header records
        // the element count and the payload size agrees with the descriptor.
        if (op.callee == "memory.owned_soa" && op.result.is_valid() &&
            op.result.type.is_soa()) {
          const SoaLayout layout = describe_soa(op.result.type);
          if (layout.element_bytes == 0 || args.size() != 1) {
            diag_.error("NRQ4005", op.location,
                        "internal: invalid soa<T> layout or missing count");
            return false;
          }
          mlir::Value count = args[0];
          const mlir::Type i64 = builder_.getI64Type();
          if (count.getType() != i64) {
            const unsigned bits = count.getType().getIntOrFloatBitWidth();
            count = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, i64, count).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, i64, count).getResult();
          }
          mlir::Value root_slot = alloca_at_entry(llvm_ptr_type(ctx_), loc);
          if (!root_slot) {
            diag_.error("NRQ4005", op.location,
                        "internal: no entry block for a managed allocation root");
            return false;
          }
          if (!gc_roots_.only_cold_alloc_roots)
            register_gc_root(root_slot, op.result.type, loc);
          mlir::Value size = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(static_cast<int64_t>(layout.element_bytes)))
                                .getResult();
          // Reference columns need strided tracing: each managed column is
          // contiguous with its own stride, so the runtime receives (offset, stride)
          // pairs and walks every column for the recorded element count.
          std::vector<uint64_t> column_pairs;
          for (const SoaColumn& column : layout.columns) {
            std::vector<uint64_t> offsets;
            collect_managed_reference_offsets(column.type, 0, offsets);
            for (uint64_t offset : offsets) {
              column_pairs.push_back(column.index_in_bytes + offset);
              column_pairs.push_back(column.element_bytes);
            }
          }
          mlir::Value result;
          if (column_pairs.empty()) {
            mlir::Value ref_count = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI64Type(), builder_.getI64IntegerAttr(0)).getResult();
            mlir::Value offsets_pointer = builder_.create<mlir::LLVM::ZeroOp>(
                loc, llvm_ptr_type(ctx_)).getResult();
            result = emit_planned_rooted_array(loc, root_slot, op.result.type,
                                               count, size, offsets_pointer,
                                               ref_count,
                                               /*force_extended=*/true);
          } else {
            mlir::Value pairs_slot = reference_layout_descriptor(column_pairs, loc);
            if (!pairs_slot) {
              diag_.error("NRQ4005", op.location,
                          "internal: no descriptor for the reference columns");
              return false;
            }
            mlir::Value pair_count = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI64Type(),
                builder_.getI64IntegerAttr(
                    static_cast<int64_t>(column_pairs.size()))).getResult();
            result = emit_planned_rooted_soa(loc, root_slot, op.result.type,
                                             count, size, pairs_slot,
                                             pair_count);
          }
          set(op.result, result);
          return true;
        }
        // `memory.dynamic_array<T>()` creates the empty container. Capacity 0 is
        // the documented empty representation, so the runtime returns a null
        // buffer and the value is {nullptr, 0, 0}: a declaration costs no memory
        // until the growth path runs.
        if (op.callee == "memory.dynamic_array" && op.result.is_valid() &&
            op.result.type.is_dynamic_array()) {
          const Type element_type = op.result.type.element_type();
          const std::uint32_t element_size = element_type.byte_size();
          if (element_size == 0) {
            diag_.error("NRQ4005", op.location,
                        "internal: element type '" + element_type.to_string() +
                            "' has no fixed layout for a dynamic array");
            return false;
          }
          const mlir::Type i64 = builder_.getI64Type();
          mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
              loc, i64, builder_.getI64IntegerAttr(0)).getResult();
          mlir::Value element_bytes = builder_.create<mlir::LLVM::ConstantOp>(
              loc, i64,
              builder_.getI64IntegerAttr(static_cast<int64_t>(element_size)))
                                           .getResult();
          mlir::Value element_alignment = builder_.create<mlir::LLVM::ConstantOp>(
              loc, i64,
              builder_.getI64IntegerAttr(
                  static_cast<int64_t>(element_type.alignment())))
                                              .getResult();
          auto create = ensure_runtime_fn(
              module_, builder_, "nrq_array_create", llvm_ptr_type(ctx_),
              {i64, i64, i64});
          mlir::Value buffer = builder_.create<mlir::LLVM::CallOp>(
              loc, create,
              mlir::ValueRange{zero, element_bytes, element_alignment})
                                   .getResult();
          mlir::Type array_type =
              tir_type_to_mlir(op.result.type, ctx_, builder_);
          mlir::Value array = builder_.create<mlir::LLVM::UndefOp>(
              loc, array_type).getResult();
          array = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, array, buffer, builder_.getDenseI64ArrayAttr({0})).getResult();
          array = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, array, zero, builder_.getDenseI64ArrayAttr({1})).getResult();
          array = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, array, zero, builder_.getDenseI64ArrayAttr({2})).getResult();
          set(op.result, array);
          return true;
        }

        if ((op.callee == "memory.owned_array" ||
             op.callee == "memory.owned_array_aligned") && op.result.is_valid() &&
            op.result.type.is_owned_array()) {
          const uint64_t element_size = type_size(op.result.type.element_type());
          const uint64_t element_alignment =
              type_alignment(op.result.type.element_type());
          const bool custom_alignment = op.callee == "memory.owned_array_aligned";
          const size_t minimum_arguments = custom_alignment ? 2 : 1;
          const size_t maximum_arguments = minimum_arguments + 1;
          if (element_size == 0 || args.size() < minimum_arguments ||
              args.size() > maximum_arguments) {
            diag_.error("NRQ4005", op.location,
                        "internal: invalid element type or missing count for memory.owned_array<T>");
            return false;
          }
          const bool has_allocator = args.size() == maximum_arguments;
          mlir::Value count = args[0];
          const mlir::Type i64 = builder_.getI64Type();
          if (count.getType() != i64) {
            const unsigned bits = count.getType().getIntOrFloatBitWidth();
            count = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, i64, count).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, i64, count).getResult();
          }
          const auto stack_alloc = stack_allocations_.find(op.result.id);
          if (stack_alloc != stack_allocations_.end() &&
              stack_alloc->second.kind == StackAllocationLayout::Kind::OwnedArray) {
            const std::size_t count_value = stack_alloc->second.count;
            mlir::Type element_type = tir_type_to_mlir(
                op.result.type.element_type(), ctx_, builder_);
            mlir::Type storage_type = mlir::LLVM::LLVMArrayType::get(
                element_type, static_cast<unsigned>(count_value));
            mlir::Value storage = alloca_at_entry(storage_type, loc);
            if (!storage) {
              diag_.error("NRQ4005", op.location,
                          "internal: failed to allocate stack storage for owned array");
              return false;
            }
            if (mlir::Operation* storage_op = storage.getDefiningOp())
              storage_op->setAttr("nirqoba.stack_placed", builder_.getUnitAttr());
            if (stack_alloc->second.alignment > element_alignment) {
              if (mlir::Operation* storage_op = storage.getDefiningOp())
                storage_op->setAttr(
                    "alignment", builder_.getI64IntegerAttr(
                                     static_cast<int64_t>(
                                         stack_alloc->second.alignment)));
            }
            mlir::Value zero_index = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI32Type(), builder_.getI32IntegerAttr(0)).getResult();
            mlir::Value data = builder_.create<mlir::LLVM::GEPOp>(
                loc, llvm_ptr_type(ctx_), storage_type, storage,
                mlir::ValueRange{zero_index, zero_index}).getResult();
            std::vector<uint64_t> reference_offsets;
            collect_managed_reference_offsets(
                op.result.type.element_type(), 0, reference_offsets);
            if (!reference_offsets.empty()) {
              // The element layout is a constant: the shared read-only descriptor
              // describes it once per module instead of once per activation.
              mlir::Value offsets_slot =
                  reference_layout_descriptor(reference_offsets, loc);
              if (!offsets_slot) return false;
              auto clear_references = ensure_runtime_fn(
                  module_, builder_, "nrq_gc_root_clear_array_references",
                  mlir::LLVM::LLVMVoidType::get(ctx_),
                  {llvm_ptr_type(ctx_), i64, i64, llvm_ptr_type(ctx_), i64});
              mlir::Value element_bytes = builder_.create<mlir::LLVM::ConstantOp>(
                  loc, i64, builder_.getI64IntegerAttr(element_size)).getResult();
              mlir::Value reference_count = builder_.create<mlir::LLVM::ConstantOp>(
                  loc, i64, builder_.getI64IntegerAttr(
                                static_cast<int64_t>(reference_offsets.size()))).getResult();
              builder_.create<mlir::LLVM::CallOp>(
                  loc, clear_references,
                  mlir::ValueRange{data, count, element_bytes, offsets_slot,
                                   reference_count});
            }
            emit_gc_array_root_registration(
                data, count, op.result.type.element_type(), loc);
            mlir::Type descriptor_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
            mlir::Value descriptor = builder_.create<mlir::LLVM::UndefOp>(
                loc, descriptor_type).getResult();
            descriptor = builder_.create<mlir::LLVM::InsertValueOp>(
                loc, descriptor, data,
                builder_.getDenseI64ArrayAttr({0})).getResult();
            descriptor = builder_.create<mlir::LLVM::InsertValueOp>(
                loc, descriptor, count,
                builder_.getDenseI64ArrayAttr({1})).getResult();
            set(op.result, descriptor);
            return true;
          }
          const std::string allocate_symbol = has_allocator
              ? "nrq_memory_allocate_array_from"
              : "nrq_memory_allocate_array";
          const std::vector<mlir::Type> allocate_arguments = has_allocator
              ? std::vector<mlir::Type>{llvm_ptr_type(ctx_), i64, i64, i64}
              : std::vector<mlir::Type>{i64, i64, i64};
          auto allocate = ensure_runtime_fn(
              module_, builder_, allocate_symbol, llvm_ptr_type(ctx_),
              allocate_arguments);
          mlir::Value element_bytes = builder_.create<mlir::LLVM::ConstantOp>(
              loc, i64, builder_.getI64IntegerAttr(element_size)).getResult();
          mlir::Value alignment = builder_.create<mlir::LLVM::ConstantOp>(
              loc, i64, builder_.getI64IntegerAttr(element_alignment)).getResult();
          if (custom_alignment) {
            mlir::Value requested = args[1];
            if (requested.getType() != i64) {
              const unsigned bits = requested.getType().getIntOrFloatBitWidth();
              requested = bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, i64, requested).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, i64, requested).getResult();
            }
            mlir::Value minimum = builder_.create<mlir::LLVM::ConstantOp>(
                loc, i64, builder_.getI64IntegerAttr(element_alignment)).getResult();
            mlir::Value below_natural = builder_.create<mlir::LLVM::ICmpOp>(
                loc, mlir::LLVM::ICmpPredicate::ult, requested, minimum).getResult();
            alignment = builder_.create<mlir::LLVM::SelectOp>(
                loc, below_natural, minimum, requested).getResult();
          }
          mlir::Value data;
          if (has_allocator) {
            mlir::Value allocator = args[custom_alignment ? 2 : 1];
            if (allocator.getType() != i64) {
              const unsigned bits = allocator.getType().getIntOrFloatBitWidth();
              allocator = bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, i64, allocator).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, i64, allocator).getResult();
            }
            allocator = builder_.create<mlir::LLVM::IntToPtrOp>(
                loc, llvm_ptr_type(ctx_), allocator).getResult();
            data = builder_.create<mlir::LLVM::CallOp>(
                loc, allocate,
                mlir::ValueRange{allocator, count, element_bytes, alignment})
                       .getResult();
          } else {
            data = builder_.create<mlir::LLVM::CallOp>(
                loc, allocate, mlir::ValueRange{count, element_bytes, alignment})
                       .getResult();
          }
          std::vector<uint64_t> root_offsets;
          collect_managed_reference_offsets(
              op.result.type.element_type(), 0, root_offsets);
          if (!root_offsets.empty()) {
            auto clear_roots = ensure_runtime_fn(
                module_, builder_, "nrq_gc_root_clear_array_references",
                mlir::LLVM::LLVMVoidType::get(ctx_),
                {llvm_ptr_type(ctx_), i64, i64, llvm_ptr_type(ctx_), i64});
            mlir::Value offsets_slot =
                reference_layout_descriptor(root_offsets, loc);
            if (!offsets_slot) return false;
            mlir::Value root_count = builder_.create<mlir::LLVM::ConstantOp>(
                loc, i64,
                builder_.getI64IntegerAttr(static_cast<int64_t>(root_offsets.size()))).getResult();
            builder_.create<mlir::LLVM::CallOp>(
                loc, clear_roots,
                mlir::ValueRange{data, count, element_bytes, offsets_slot,
                                 root_count});
          }
          emit_gc_array_root_registration(
              data, count, op.result.type.element_type(), loc);
          mlir::Value null_pointer = builder_.create<mlir::LLVM::ZeroOp>(
              loc, llvm_ptr_type(ctx_)).getResult();
          mlir::Value succeeded = builder_.create<mlir::LLVM::ICmpOp>(
              loc, mlir::LLVM::ICmpPredicate::ne, data, null_pointer).getResult();
          mlir::Value empty_length = builder_.create<mlir::LLVM::ConstantOp>(
              loc, i64, builder_.getI64IntegerAttr(0)).getResult();
          mlir::Value actual_length = builder_.create<mlir::LLVM::SelectOp>(
              loc, succeeded, count, empty_length).getResult();
          mlir::Type descriptor_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
          mlir::Value descriptor = builder_.create<mlir::LLVM::UndefOp>(
              loc, descriptor_type).getResult();
          descriptor = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, descriptor, data,
              builder_.getDenseI64ArrayAttr({0})).getResult();
          descriptor = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, descriptor, actual_length,
              builder_.getDenseI64ArrayAttr({1})).getResult();
          set(op.result, descriptor);
          return true;
        }

        if ((op.callee == "memory.array_view" ||
             op.callee == "memory.array_view_mut") && op.result.is_valid() &&
            op.result.type.is_array_view() && args.size() == 3) {
          const mlir::Type i64 = builder_.getI64Type();
          auto as_i64 = [&](mlir::Value value) {
            if (value.getType() == i64) return value;
            const unsigned bits = value.getType().getIntOrFloatBitWidth();
            return bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, i64, value).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, i64, value).getResult();
          };
          mlir::Value source_data = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, args[0], builder_.getDenseI64ArrayAttr({0})).getResult();
          mlir::Value source_length = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, args[0], builder_.getDenseI64ArrayAttr({1})).getResult();
          mlir::Value start = as_i64(args[1]);
          mlir::Value count = as_i64(args[2]);
          auto range_check = ensure_runtime_fn(
              module_, builder_, "nrq_memory_array_range_check",
              mlir::LLVM::LLVMVoidType::get(ctx_), {i64, i64, i64});
          builder_.create<mlir::LLVM::CallOp>(
              loc, range_check, mlir::ValueRange{start, count, source_length});
          mlir::Type element_type = tir_type_to_mlir(
              op.result.type.element_type(), ctx_, builder_);
          mlir::Value data = builder_.create<mlir::LLVM::GEPOp>(
              loc, llvm_ptr_type(ctx_), element_type, source_data,
              mlir::ValueRange{start}).getResult();
          mlir::Type descriptor_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
          mlir::Value descriptor = builder_.create<mlir::LLVM::UndefOp>(
              loc, descriptor_type).getResult();
          descriptor = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, descriptor, data, builder_.getDenseI64ArrayAttr({0})).getResult();
          descriptor = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, descriptor, count, builder_.getDenseI64ArrayAttr({1})).getResult();
          set(op.result, descriptor);
          return true;
        }

        // The container owns one block: unregister its root, then free it. There is
        // no descriptor aggregate to extract, unlike an owned array value.
        if (op.callee == "soa.owned_release" && args.size() == 1) {
          auto unregister = ensure_runtime_fn(
              module_, builder_, "nrq_gc_root_unregister",
              mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, unregister,
                                              mlir::ValueRange{args[0]});
          // The block belongs to the collector, so releasing ownership is exactly
          // unregistering the root: freeing it here would be a double free the
          // sweep would then undo a second time.
          return true;
        }
        if (op.callee == "array.owned_release" && args.size() == 1) {
          mlir::Value data = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, args[0], builder_.getDenseI64ArrayAttr({0})).getResult();
          if (release_payload_has_managed_reference(op.arguments[0].type)) {
            auto unregister = ensure_runtime_fn(
                module_, builder_, "nrq_gc_root_unregister",
                mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
            builder_.create<mlir::LLVM::CallOp>(
                loc, unregister, mlir::ValueRange{data});
          }
          if (op.arguments.size() == 1 &&
              stack_allocations_.find(op.arguments[0].id) !=
                  stack_allocations_.end())
            return true;
          auto release = ensure_runtime_fn(
              module_, builder_, "nrq_memory_deallocate_checked",
              mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, release,
                                               mlir::ValueRange{data});
          return true;
        }

        // A dynamic array's destructor releases the buffer the container owns,
        // not the three-word value itself, so the runtime receives field 0. An
        // element that owns storage is destroyed first, in reverse index order
        // and only up to `length`, and the buffer is released afterwards.
        // An array a C function filled in place owns a buffer C allocated, so its
        // release is the foreign one and only the buffer field is handed over.
        if (op.callee == "nrq_ffi_free" && args.size() == 1 &&
            op.arguments[0].type.is_dynamic_array()) {
          mlir::Value buffer = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, args[0], builder_.getDenseI64ArrayAttr({0})).getResult();
          auto release = ensure_runtime_fn(
              module_, builder_, "nrq_ffi_free",
              mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, release,
                                             mlir::ValueRange{buffer});
          return true;
        }
        if (op.callee == "nrq_array_free" && args.size() == 1 &&
            op.arguments[0].type.is_dynamic_array()) {
          const Type& element_type =
              op.arguments[0].type.element_type();
          mlir::Value buffer = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, args[0], builder_.getDenseI64ArrayAttr({0})).getResult();
          const std::string element_release =
              element_release_symbol(element_type);
          if (!element_release.empty()) {
            mlir::Type element_mlir =
                tir_type_to_mlir(element_type, ctx_, builder_);
            if (!element_mlir) {
              diag_.error("NRQ4005", op.location,
                          "internal: element type '" +
                              element_type.to_string() +
                              "' has no lowering for array teardown");
              return false;
            }
            mlir::Value length = builder_.create<mlir::LLVM::ExtractValueOp>(
                loc, args[0], builder_.getDenseI64ArrayAttr({1})).getResult();
            emit_element_teardown_loop(buffer, length, element_type,
                                       element_mlir, element_release,
                                       op.location, loc);
          } else if (element_owns_storage(element_type)) {
            // Sema admits only the kinds with a teardown, so reaching here means
            // the two halves of the contract have drifted apart.
            diag_.error("NRQ4005", op.location,
                        "internal: no element teardown for dynamic array element "
                        "type '" +
                            element_type.to_string() + "'");
            return false;
          }
          auto release = ensure_runtime_fn(
              module_, builder_, "nrq_array_free",
              mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, release,
                                              mlir::ValueRange{buffer});
          return true;
        }

        if (op.callee == "aggregate.owned_drop" && args.size() == 1) {
          llvm::SmallVector<int64_t, 8> path;
          drop_aggregate_fields(op.arguments[0].type, args[0], path, loc);
          return true;
        }

        // Copying an aggregate retains every shared pointer field it carries.
        if (op.callee == "aggregate.shared_retain" && args.size() == 1) {
          llvm::SmallVector<int64_t, 8> path;
          retain_aggregate_fields(op.arguments[0].type, args[0], path, loc);
          return true;
        }

        // Memory allocation is exposed as an integer handle in the language,
        // while the runtime ABI uses void*. Keep that boundary conversion
        // explicit so the call is emitted as an LLVM runtime call (rather
        // than an unresolved func.call to the runtime symbol).
        if (op.callee == "array.allocate" && op.result.is_valid() &&
            op.result.type.is_managed_pointer()) {
          const Type& element_type = op.result.type.element_type();
          const uint64_t element_size = type_size(element_type);
          if (element_size == 0 || args.empty()) {
            diag_.error("NRQ4005", op.location,
                        "internal: invalid element type or missing count for array.allocate<T>");
            return false;
          }
          mlir::Value count = args[0];
          if (count.getType() != builder_.getI64Type()) {
            const unsigned bits = count.getType().getIntOrFloatBitWidth();
            count = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), count).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), count).getResult();
          }
          std::vector<uint64_t> reference_offsets;
          collect_managed_reference_offsets(element_type, 0, reference_offsets);
          mlir::Value offsets_pointer = builder_.create<mlir::LLVM::ZeroOp>(
              loc, llvm_ptr_type(ctx_)).getResult();
          if (!reference_offsets.empty()) {
            // The offsets describe the element type, not this call, so one
            // read-only descriptor per module serves every allocation site.
            offsets_pointer = reference_layout_descriptor(reference_offsets, loc);
            if (!offsets_pointer) {
              diag_.error("NRQ4006", op.location,
                          "internal: no reference layout descriptor available");
              return false;
            }
          }
          mlir::Value root_slot;
          if (!gc_roots_.only_cold_alloc_roots)
            root_slot = coalesced_allocation_root(op, loc);
          if (!root_slot) {
            root_slot = alloca_at_entry(llvm_ptr_type(ctx_), loc);
            if (root_slot && !gc_roots_.only_cold_alloc_roots)
              register_gc_root(root_slot, op.result.type, loc);
          }
          if (!root_slot) {
            diag_.error("NRQ4005", op.location,
                        "internal: no entry block for a managed allocation root");
            return false;
          }
          mlir::Value size = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(element_size)).getResult();
          mlir::Value ref_count = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(static_cast<int64_t>(reference_offsets.size()))).getResult();
          mlir::Value result = emit_planned_rooted_array(
              loc, root_slot, op.result.type, count, size, offsets_pointer,
              ref_count);
          set(op.result, result);
          if (is_development(configuration_) && op.location.is_valid()) {
            const uint64_t site = gc_site_id(op.location);
            emit_gc_site_declaration(site, op.location, loc);
            emit_gc_allocation_tag(result, site, loc);
            invalidate_inline_cfg_cache();
          }
          return true;
        }
        if (op.callee == "memory.manual_allocate" && op.result.is_valid() &&
            op.result.type.is_pointer()) {
          mlir::Value bytes = args.empty() ? mlir::Value{} : args[0];
          if (!bytes) {
            diag_.error("NRQ4005", op.location,
                        "internal: memory.manual_allocate<T> has no byte-count operand");
            return false;
          }
          if (bytes.getType() != builder_.getI64Type()) {
            const unsigned bits = bytes.getType().getIntOrFloatBitWidth();
            bytes = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), bytes).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), bytes).getResult();
          }
          const std::size_t alignment_bytes =
              type_alignment(op.result.type.element_type());
          mlir::Value alignment = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(alignment_bytes)).getResult();
          mlir::Value result;
          if (args.size() == 2) {
            mlir::Value allocator = args[1];
            if (allocator.getType() != builder_.getI64Type()) {
              const unsigned bits = allocator.getType().getIntOrFloatBitWidth();
              allocator = bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), allocator).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), allocator).getResult();
            }
            allocator = builder_.create<mlir::LLVM::IntToPtrOp>(
                loc, llvm_ptr_type(ctx_), allocator).getResult();
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_allocate_from",
                llvm_ptr_type(ctx_),
                {llvm_ptr_type(ctx_), builder_.getI64Type(), builder_.getI64Type()});
            result = builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{allocator, bytes, alignment}).getResult();
          } else {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_allocate",
                llvm_ptr_type(ctx_), {builder_.getI64Type(), builder_.getI64Type()});
            auto call = builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{bytes, alignment});
            mark_fresh_allocation(fn, alignment_bytes);
            result = call.getResult();
          }
          emit_gc_root_registration(result, op.result.type.element_type(), loc);
          set(op.result, result);
          return true;
        }
        if (op.callee == "memory.allocate" && op.result.is_valid() &&
            op.result.type.is_pointer()) {
          const auto stack_alloc = stack_allocations_.find(op.result.id);
          if (stack_alloc != stack_allocations_.end() &&
              stack_alloc->second.kind == StackAllocationLayout::Kind::RawPointer) {
            const std::size_t byte_count = stack_alloc->second.count;
            const std::size_t align = stack_alloc->second.alignment;
            mlir::Type storage_type = mlir::LLVM::LLVMArrayType::get(
                builder_.getI8Type(), static_cast<unsigned>(byte_count));
            mlir::Value storage = alloca_at_entry(storage_type, loc);
            if (!storage) {
              diag_.error("NRQ4005", op.location,
                          "internal: failed to allocate stack storage for memory.allocate");
              return false;
            }
            if (mlir::Operation* storage_op = storage.getDefiningOp()) {
              storage_op->setAttr("nirqoba.stack_placed", builder_.getUnitAttr());
              if (align > 1) {
                storage_op->setAttr("alignment",
                                    builder_.getI64IntegerAttr(static_cast<int64_t>(align)));
              }
            }
            mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI32Type(), builder_.getI32IntegerAttr(0)).getResult();
            mlir::Value ptr = builder_.create<mlir::LLVM::GEPOp>(
                loc, llvm_ptr_type(ctx_), storage_type, storage,
                mlir::ValueRange{zero, zero}).getResult();
            // A stack-placed raw allocation is still an explicitly owned object.
            // Publish its typed root inside this activation's root frame only when
            // this function has a point where collection can observe it; otherwise
            // the storage dies before any snapshot and registration is pure cost.
            if (has_collection_point_)
              emit_frame_gc_root_registration(ptr, op.result.type.element_type(), loc);
            set(op.result, ptr);
            return true;
          }
          mlir::Value bytes = args.empty() ? mlir::Value{} : args[0];
          if (!bytes) {
            diag_.error("NRQ4005", op.location,
                        "internal: memory.allocate<T> has no byte-count operand");
            return false;
          }
          if (bytes.getType() != builder_.getI64Type()) {
            const unsigned bits = bytes.getType().getIntOrFloatBitWidth();
            bytes = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), bytes).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), bytes).getResult();
          }
          const std::size_t alignment_bytes =
              type_alignment(op.result.type.element_type());
          mlir::Value alignment = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(alignment_bytes)).getResult();
          mlir::Value result;
          if (args.size() == 2) {
            mlir::Value allocator = args[1];
            if (allocator.getType() != builder_.getI64Type()) {
              const unsigned bits = allocator.getType().getIntOrFloatBitWidth();
              allocator = bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), allocator).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), allocator).getResult();
            }
            allocator = builder_.create<mlir::LLVM::IntToPtrOp>(
                loc, llvm_ptr_type(ctx_), allocator).getResult();
            const bool arena_fast_path =
                op.result.ownership == ValueOwnership::ArenaBound &&
                op.result.arena_kind == 1;
            auto fn = ensure_runtime_fn(module_, builder_,
                arena_fast_path ? "nrq_arena_allocate"
                                : "nrq_memory_allocate_from",
                llvm_ptr_type(ctx_),
                {llvm_ptr_type(ctx_), builder_.getI64Type(), builder_.getI64Type()});
            auto call = builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{allocator, bytes, alignment});
            mark_fresh_allocation(fn, alignment_bytes);
            result = call.getResult();
          } else if (caller_arena_arg_index_ >= 0 &&
                     arena_param_sites_.count(op.result.id) != 0) {
            // The storage leaves through the return, so its home is the caller's
            // arena - when the caller had one to give. An escaping call site
            // passes null instead, and this body must then allocate the storage
            // itself: one signature serves both. The decision belongs to the call
            // site, so neither allocation carries a strategy attribute of its
            // own.
            mlir::Value handle = func_op_.getArgument(
                static_cast<unsigned>(caller_arena_arg_index_));
            mlir::Value storage = allocate_for_returned_storage(
                handle, bytes, alignment, alignment_bytes, loc);
            set(op.result, storage);
            emit_arena_field_teardowns(handle, storage, op.result.type, loc);
            return true;
          } else if (activation_sites_.count(op.result.id) != 0) {
            // A promoted activation fills its caller's chunk rather than opening
            // one of its own; the caller's destroy releases everything this
            // activation and its callees put there.
            mlir::Value handle = func_op_.getArgument(
                static_cast<unsigned>(caller_arena_arg_index_));
            auto arena_fn = ensure_runtime_fn(
                module_, builder_, "nrq_cdmm_arena_allocate",
                llvm_ptr_type(ctx_),
                {llvm_ptr_type(ctx_), builder_.getI64Type(),
                 builder_.getI64Type()});
            auto call = builder_.create<mlir::LLVM::CallOp>(
                loc, arena_fn, mlir::ValueRange{handle, bytes, alignment});
            call->setAttr("nirqoba.hybrid_strategy",
                          builder_.getStringAttr("arena"));
            mark_fresh_allocation(arena_fn, alignment_bytes);
            set(op.result, call.getResult());
            emit_arena_field_teardowns(handle, call.getResult(), op.result.type,
                                       loc);
            return true;
          } else if (const auto region = region_of_site_.find(op.result.id);
                     region != region_of_site_.end()) {
            // Phase Psi: the lifetime classifier proved this pointer never leaves
            // its region's scope, so its storage comes from the region shared by
            // every site in the same scope with the same layout. The payload is
            // plain data by selection - no managed reference to trace and no
            // field destructor to run - which is why neither a root registration
            // nor an element cleanup follows this call.
            mlir::Value handle = region_handle(region->second, loc);
            if (!handle) {
              diag_.error("NRQ4005", op.location,
                          "internal: selected allocation has no region handle");
              return false;
            }
            if (regions_[region->second].pooled) {
              // Repeated work: the slot comes from a pool whose slots are this
              // exact layout - the group was built from identical ones - and the
              // Drop below hands the slot back, which is what keeps a loop to one
              // slot per iteration.
              auto pool_fn = ensure_runtime_fn(
                  module_, builder_, "nrq_cdmm_pool_allocate",
                  llvm_ptr_type(ctx_), {llvm_ptr_type(ctx_)});
              auto call = builder_.create<mlir::LLVM::CallOp>(
                  loc, pool_fn, mlir::ValueRange{handle});
              call->setAttr("nirqoba.hybrid_strategy",
                            builder_.getStringAttr("pool"));
              // A pool hands out one fixed-size slot at a time and never the same
              // slot twice while it is live, so the answer is as fresh as a
              // malloc's and as aligned as the layout it was built from.
              mark_fresh_allocation(pool_fn, regions_[region->second].alignment);
              set(op.result, call.getResult());
              emit_arena_field_teardowns(handle, call.getResult(), op.result.type,
                                         loc, /*pool_slot=*/true);
              return true;
            }
            auto arena_fn = ensure_runtime_fn(
                module_, builder_, "nrq_cdmm_arena_allocate",
                llvm_ptr_type(ctx_),
                {llvm_ptr_type(ctx_), builder_.getI64Type(),
                 builder_.getI64Type()});
            auto call = builder_.create<mlir::LLVM::CallOp>(
                loc, arena_fn, mlir::ValueRange{handle, bytes, alignment});
            // Names the decision for the pass that audits it and for anyone
            // reading the emitted IR.
            call->setAttr("nirqoba.hybrid_strategy",
                          builder_.getStringAttr("arena"));
            mark_fresh_allocation(arena_fn, alignment_bytes);
            result = call.getResult();
            set(op.result, result);
            emit_arena_field_teardowns(handle, result, op.result.type, loc);
            return true;
          } else if (collector_sites_.count(op.result.id) != 0) {
            // The collector owns this storage. Every cheaper strategy had its
            // say and declined - a cyclic payload escapes by construction, a
            // stack copy would dangle, an arena would free it wholesale - so the
            // allocation goes to the managed heap with the object's reference
            // offsets attached, and the collector walks those fields itself.
            const Type& payload = op.result.type.element_type();
            // The slot that holds the object is itself a managed reference: the
            // collector publishes into it, and marking must be able to read it
            // while the allocation is still in flight.
            const Type root_type = Type::managed_pointer_type(payload);
            std::vector<uint64_t> reference_offsets;
            collect_managed_reference_offsets(payload, 0, reference_offsets);
            mlir::Value root_slot = alloca_at_entry(llvm_ptr_type(ctx_), loc);
            if (!root_slot) {
              diag_.error("NRQ4005", op.location,
                          "internal: no entry block for a collected allocation");
              return false;
            }
            mlir::Value cold_marker;
            if (gc_roots_.only_cold_alloc_roots) {
              // Nothing else in this activation holds a managed reference, so the
              // frame covers this one call and closes as soon as the collector
              // has published the object.
              mlir::Value null_ptr = builder_.create<mlir::LLVM::ZeroOp>(
                  loc, llvm_ptr_type(ctx_)).getResult();
              builder_.create<mlir::LLVM::StoreOp>(loc, null_ptr, root_slot)
                  .setAlignment(8);
              auto begin_fn = ensure_runtime_fn(
                  module_, builder_, "nrq_gc_root_frame_begin",
                  builder_.getI64Type(), {});
              cold_marker = builder_.create<mlir::LLVM::CallOp>(
                  loc, begin_fn, mlir::ValueRange{}).getResult();
              emit_gc_root_registration(root_slot, root_type, loc);
            } else {
              // The activation already has a frame; the root joins it and stays
              // published until the frame ends.
              register_gc_root(root_slot, root_type, loc);
            }
            // One read-only descriptor per payload layout, and one
            // zero-initialised cache word per site: the runtime resolves the
            // layout once and the site reuses it.
            constexpr uint32_t kHasReferencesPlanBit = 1u << 3;
            mlir::Value offsets_pointer = builder_.create<mlir::LLVM::ZeroOp>(
                loc, llvm_ptr_type(ctx_)).getResult();
            if (!reference_offsets.empty()) {
              offsets_pointer = reference_layout_descriptor(reference_offsets, loc);
              if (!offsets_pointer) {
                diag_.error("NRQ4005", op.location,
                            "internal: no descriptor for the collected layout");
                return false;
              }
            }
            AllocationSiteGlobals site = allocation_site_globals(loc);
            mlir::Value reference_count = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI64Type(),
                builder_.getI64IntegerAttr(
                    static_cast<int64_t>(reference_offsets.size()))).getResult();
            mlir::Value plan = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI32Type(),
                builder_.getI32IntegerAttr(
                    reference_offsets.empty()
                        ? 0
                        : static_cast<int32_t>(kHasReferencesPlanBit))).getResult();
            auto gc_fn = ensure_runtime_fn(
                module_, builder_, "nrq_gc_allocate_planned",
                llvm_ptr_type(ctx_),
                {llvm_ptr_type(ctx_), builder_.getI64Type(),
                 llvm_ptr_type(ctx_), builder_.getI64Type(),
                 llvm_ptr_type(ctx_), builder_.getI32Type()});
            auto gc_call = builder_.create<mlir::LLVM::CallOp>(
                loc, gc_fn,
                mlir::ValueRange{root_slot, bytes, offsets_pointer,
                                 reference_count, site.site_cache, plan});
            // Names the decision for the pass that audits it and for anyone
            // reading the emitted IR.
            gc_call->setAttr("nirqoba.hybrid_strategy",
                             builder_.getStringAttr("gc"));
            if (cold_marker) {
              auto end_fn = ensure_runtime_fn(
                  module_, builder_, "nrq_gc_root_frame_end",
                  mlir::LLVM::LLVMVoidType::get(ctx_), {builder_.getI64Type()});
              builder_.create<mlir::LLVM::CallOp>(loc, end_fn,
                                                  mlir::ValueRange{cold_marker});
            }
            set(op.result, gc_call.getResult());
            return true;
          } else {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_allocate",
                llvm_ptr_type(ctx_), {builder_.getI64Type(), builder_.getI64Type()});
            auto call = builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{bytes, alignment});
            mark_fresh_allocation(
                fn, type_alignment(op.result.type.element_type()));
            result = call.getResult();
          }
          // Explicitly owned storage is outside the collector's heap, but a
          // managed field inside it still has to be visible to tracing. Register
          // the object as a normal typed root; destruction unregisters it.
          emit_gc_root_registration(result, op.result.type.element_type(), loc);
          set(op.result, result);
          return true;
        }
        if (op.callee == "memory.shared_allocate" && op.result.is_valid() &&
            op.result.type.is_pointer()) {
          if (args.empty()) return false;
          mlir::Value bytes = args[0];
          if (bytes.getType() != builder_.getI64Type()) {
            const unsigned bits = bytes.getType().getIntOrFloatBitWidth();
            bytes = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), bytes).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), bytes).getResult();
          }
          mlir::Value alignment = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(type_alignment(op.result.type.element_type()))).getResult();
          std::vector<uint64_t> root_offsets;
          collect_managed_reference_offsets(
              op.result.type.element_type(), 0, root_offsets);
          if (!root_offsets.empty()) {
            mlir::Value offsets_slot = reference_layout_descriptor(root_offsets, loc);
            if (!offsets_slot) return false;
            auto fn = ensure_runtime_fn(
                module_, builder_, "nrq_memory_shared_allocate_rooted",
                llvm_ptr_type(ctx_),
                {builder_.getI64Type(), builder_.getI64Type(),
                 builder_.getI64Type(), llvm_ptr_type(ctx_),
                 builder_.getI64Type()});
            mlir::Value object_size = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI64Type(),
                builder_.getI64IntegerAttr(type_size(op.result.type.element_type()))).getResult();
            mlir::Value reference_count = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI64Type(),
                builder_.getI64IntegerAttr(
                    static_cast<int64_t>(root_offsets.size()))).getResult();
            set(op.result, builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{bytes, alignment, object_size,
                                          offsets_slot, reference_count})
                               .getResult());
            return true;
          }
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_shared_allocate",
              llvm_ptr_type(ctx_), {builder_.getI64Type(), builder_.getI64Type()});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{bytes, alignment}).getResult());
          return true;
        }
        if (op.callee == "memory.shared_retain" ||
            op.callee == "memory.shared_release" ||
            op.callee == "memory.shared_count") {
          if (args.empty()) return false;
          const char* symbol = op.callee == "memory.shared_retain"
              ? "nrq_memory_shared_retain"
              : op.callee == "memory.shared_release"
                  ? "nrq_memory_shared_release" : "nrq_memory_shared_count";
          const bool count = op.callee == "memory.shared_count";
          const bool retain = op.callee == "memory.shared_retain";
          mlir::Type result_type = (count ? builder_.getI64Type()
                                          : retain ? llvm_ptr_type(ctx_)
                                                   : builder_.getI32Type());
          auto fn = ensure_runtime_fn(module_, builder_, symbol, result_type,
                                      {llvm_ptr_type(ctx_)});
          auto result = builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{args[0]});
          if (op.result.is_valid()) set(op.result, result.getResult());
          return true;
        }
        if (op.callee == "memory.secure_allocate" && op.result.is_valid() &&
            op.result.type.is_pointer()) {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_secure_allocate",
                                     llvm_ptr_type(ctx_),
                                     {builder_.getI64Type(), builder_.getI64Type()});
          if (args.empty()) return false;
          mlir::Value bytes = args[0];
          if (bytes.getType() != builder_.getI64Type()) {
            const unsigned bits = bytes.getType().getIntOrFloatBitWidth();
            bytes = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), bytes).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), bytes).getResult();
          }
          mlir::Value alignment = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(type_alignment(op.result.type.element_type()))).getResult();
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{bytes, alignment}).getResult());
          return true;
        }
        if (op.callee == "memory.mapped_read" ||
            op.callee == "memory.mapped_write") {
          const bool write = op.callee == "memory.mapped_write";
          if (args.size() != (write ? 3u : 2u)) return false;
          mlir::Value offset = args[1];
          if (offset.getType() != builder_.getI64Type()) {
            const unsigned bits = offset.getType().getIntOrFloatBitWidth();
            offset = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), offset).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), offset).getResult();
          }
          mlir::Type element_type = tir_type_to_mlir(
              write ? op.arguments[2].type : op.result.type, ctx_, builder_);
          // Mapped access moves a single element through a stack slot. The slot
          // is per-activation, not per-iteration.
          mlir::Value temporary = alloca_at_entry(element_type, loc);
          if (!temporary) {
            diag_.error("NRQ4006", op.location,
                        "internal: no entry block for a mapped access temporary");
            return false;
          }
          mlir::Value byte_count = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(type_size(write ? op.arguments[2].type
                                                         : op.result.type))).getResult();
          auto fn = ensure_runtime_fn(module_, builder_,
              write ? "nrq_memory_mapping_write" : "nrq_memory_mapping_read",
              builder_.getI32Type(),
              {llvm_ptr_type(ctx_), builder_.getI64Type(), builder_.getI64Type(),
               llvm_ptr_type(ctx_)});
          if (write) builder_.create<mlir::LLVM::StoreOp>(loc, args[2], temporary);
          builder_.create<mlir::LLVM::CallOp>(loc, fn,
              mlir::ValueRange{args[0], offset, byte_count, temporary});
          if (!write) set(op.result, builder_.create<mlir::LLVM::LoadOp>(
              loc, element_type, temporary).getResult());
          return true;
        }
        if (op.callee == "memory.profiler.start" ||
            op.callee == "memory.profiler.stop") {
          auto fn = ensure_runtime_fn(module_, builder_,
              op.callee == "memory.profiler.start" ? "nrq_memory_profiler_start"
                                                     : "nrq_memory_profiler_stop",
              mlir::LLVM::LLVMVoidType::get(ctx_), {});
          builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{});
          return true;
        }
        if (op.callee == "memory.profiler.capture") {
          mlir::Type profile_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
          // The capture writes one profile record per activation of the call.
          mlir::Value slot = alloca_at_entry(profile_type, loc);
          if (!slot) {
            diag_.error("NRQ4006", op.location,
                        "internal: no entry block for a profiler capture slot");
            return false;
          }
          auto capture = ensure_runtime_fn(module_, builder_,
              "nrq_memory_profiler_capture_into",
              mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, capture, mlir::ValueRange{slot});
          set(op.result, builder_.create<mlir::LLVM::LoadOp>(
              loc, profile_type, slot).getResult());
          return true;
        }
        if (op.callee == "memory.available_bytes") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_available_bytes",
                                      builder_.getI64Type(), {});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{}).getResult());
          return true;
        }
        if (op.callee == "memory.runtime_version") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_runtime_version",
                                      llvm_ptr_type(ctx_), {});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{}).getResult());
          return true;
        }
        if (op.callee == "memory.cpu_count") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_system_cpu_count",
                                      builder_.getI32Type(), {});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{}).getResult());
          return true;
        }
        if (op.callee == "memory.cpu_features") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_system_cpu_features",
                                      builder_.getI64Type(), {});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{}).getResult());
          return true;
        }
        if (op.callee == "memory.thread_count") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_system_thread_count",
                                      builder_.getI32Type(), {});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{}).getResult());
          return true;
        }
        if (op.callee == "memory.allocator_kind") {
          mlir::Value handle;
          if (args.empty()) {
            handle = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI64Type(), builder_.getI64IntegerAttr(0)).getResult();
          } else {
            handle = args.front();
          }
          if (handle.getType() != builder_.getI64Type()) {
            const unsigned bits = handle.getType().getIntOrFloatBitWidth();
            handle = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), handle).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), handle).getResult();
          }
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_allocator_kind",
              builder_.getI32Type(), {builder_.getI64Type()});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{handle}).getResult());
          return true;
        }
        if (op.callee == "memory.start_ref_count" ||
            op.callee == "memory.stop_ref_count") {
          auto fn = ensure_runtime_fn(module_, builder_,
              op.callee == "memory.start_ref_count" ? "nrq_gc_ref_count_start"
                                                      : "nrq_gc_ref_count_stop",
              mlir::LLVM::LLVMVoidType::get(ctx_), {});
          builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{});
          return true;
        }
        if (op.callee == "memory.get_ref_count") {
          if (args.empty()) return false;
          mlir::Value pointer = args[0];
          if (!mlir::isa<mlir::LLVM::LLVMPointerType>(pointer.getType()))
            pointer = builder_.create<mlir::LLVM::IntToPtrOp>(
                loc, llvm_ptr_type(ctx_), pointer).getResult();
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_gc_get_ref_count",
              builder_.getI64Type(), {llvm_ptr_type(ctx_)});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{pointer}).getResult());
          return true;
        }
        if (op.callee == "memory.map_file") {
          if (args.size() != 2) return false;
          mlir::Value writable = args[1];
          if (writable.getType() != builder_.getI32Type())
            writable = builder_.create<mlir::LLVM::ZExtOp>(
                loc, builder_.getI32Type(), writable).getResult();
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_map_file",
              llvm_ptr_type(ctx_), {llvm_ptr_type(ctx_), builder_.getI32Type()});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{args[0], writable}).getResult());
          return true;
        }
        if (op.callee == "memory.mapping_size" ||
            op.callee == "memory.unmap_file" ||
            op.callee == "memory.secure_release" ||
            op.callee == "memory.secure_zero" ||
            op.callee == "memory.pin" || op.callee == "memory.unpin") {
          if (args.empty()) return false;
          mlir::Value pointer = args[0];
          if (!mlir::isa<mlir::LLVM::LLVMPointerType>(pointer.getType()))
            pointer = builder_.create<mlir::LLVM::IntToPtrOp>(
                loc, llvm_ptr_type(ctx_), pointer).getResult();
          if (op.callee == "memory.mapping_size") {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_mapping_size",
                builder_.getI64Type(), {llvm_ptr_type(ctx_)});
            set(op.result, builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{pointer}).getResult());
          } else if (op.callee == "memory.unmap_file") {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_unmap_file",
                builder_.getI32Type(), {llvm_ptr_type(ctx_)});
            set(op.result, builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{pointer}).getResult());
          } else if (op.callee == "memory.secure_release") {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_secure_release",
                mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
            builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{pointer});
          } else if (op.callee == "memory.secure_zero") {
            if (args.size() != 2) return false;
            mlir::Value bytes = args[1];
            if (bytes.getType() != builder_.getI64Type()) {
              const unsigned bits = bytes.getType().getIntOrFloatBitWidth();
              bytes = bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), bytes).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), bytes).getResult();
            }
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_secure_zero",
                mlir::LLVM::LLVMVoidType::get(ctx_),
                {llvm_ptr_type(ctx_), builder_.getI64Type()});
            builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{pointer, bytes});
          } else if (op.callee == "memory.pin") {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_gc_pin",
                llvm_ptr_type(ctx_), {llvm_ptr_type(ctx_)});
            set(op.result, builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{pointer}).getResult());
          } else {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_gc_unpin",
                mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
            builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{pointer});
          }
          return true;
        }
        {
          static const std::unordered_map<std::string, int> allocator_kinds = {
              {"memory.general_allocator", 0}, {"memory.pool_allocator", 1},
              {"memory.pool", 1},
              {"memory.arena_allocator", 2}, {"memory.region", 2},
              {"memory.arena", 2},
              {"memory.stack_allocator", 3},
              {"memory.slab_allocator", 4}, {"memory.buddy_allocator", 5},
              {"memory.free_list_allocator", 6}, {"memory.tlsf_allocator", 7}};
          auto constructor = allocator_kinds.find(op.callee);
          if (constructor != allocator_kinds.end()) {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_allocator_create",
                                       llvm_ptr_type(ctx_),
                                       {builder_.getI32Type(), builder_.getI64Type()});
            mlir::Value capacity = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI64Type(), builder_.getI64IntegerAttr(1024 * 1024)).getResult();
            if (!args.empty()) {
              capacity = args[0];
              if (capacity.getType() != builder_.getI64Type()) {
                const unsigned bits = capacity.getType().getIntOrFloatBitWidth();
                capacity = bits < 64
                    ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), capacity).getResult()
                    : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), capacity).getResult();
              }
            }
            mlir::Value kind = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI32Type(),
                builder_.getI32IntegerAttr(constructor->second)).getResult();
            auto result = builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{kind, capacity}).getResult();
            set(op.result, builder_.create<mlir::LLVM::PtrToIntOp>(
                loc, builder_.getI64Type(), result).getResult());
            return true;
          }
        }
        if (op.callee == "memory.arena_reset" ||
            op.callee == "memory.pool_reset" ||
            op.callee == "memory.arena_destroy" ||
            op.callee == "memory.pool_destroy") {
          if (args.empty()) return false;
          mlir::Value handle = args[0];
          if (handle.getType() != builder_.getI64Type()) {
            const unsigned bits = handle.getType().getIntOrFloatBitWidth();
            handle = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), handle).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), handle).getResult();
          }
          auto pointer = builder_.create<mlir::LLVM::IntToPtrOp>(
              loc, llvm_ptr_type(ctx_), handle).getResult();
          const bool destroy = op.callee.find("destroy") != std::string::npos;
          auto fn = ensure_runtime_fn(module_, builder_,
              destroy ? "nrq_arena_destroy" : "nrq_arena_reset",
              builder_.getI32Type(), {llvm_ptr_type(ctx_)});
          auto result = builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{pointer}).getResult();
          if (op.result.is_valid())
            set(op.result, result);
          return true;
        }
        if (op.callee == "memory.pool_release") {
          if (args.empty()) return false;
          auto fn = ensure_runtime_fn(module_, builder_,
              "nrq_memory_deallocate_checked",
              mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{args[0]});
          return true;
        }
        if (op.callee == "memory.get_allocator" ||
            op.callee == "memory.use_allocator" ||
            op.callee == "memory.reset_allocator" ||
            op.callee == "memory.destroy_allocator" ||
            op.callee == "memory.get_default_allocator" ||
            op.callee == "memory.set_default_allocator") {
          if (op.callee == "memory.get_allocator" ||
              op.callee == "memory.get_default_allocator") {
            auto fn = ensure_runtime_fn(module_, builder_,
                op.callee == "memory.get_allocator" ? "nrq_memory_get_allocator"
                                                     : "nrq_memory_get_default_allocator",
                llvm_ptr_type(ctx_), {});
            auto result = builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{}).getResult();
            set(op.result, builder_.create<mlir::LLVM::PtrToIntOp>(loc, builder_.getI64Type(), result).getResult());
          } else {
            mlir::Value handle = args.empty() ? mlir::Value{} : args[0];
            if (!handle) return false;
            if (handle.getType() != builder_.getI64Type()) {
              const unsigned bits = handle.getType().getIntOrFloatBitWidth();
              handle = bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), handle).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), handle).getResult();
            }
            auto pointer = builder_.create<mlir::LLVM::IntToPtrOp>(loc, llvm_ptr_type(ctx_), handle).getResult();
            if (op.callee == "memory.use_allocator" ||
                op.callee == "memory.set_default_allocator") {
              auto fn = ensure_runtime_fn(module_, builder_,
                  op.callee == "memory.use_allocator" ? "nrq_memory_use_allocator"
                                                       : "nrq_memory_set_default_allocator",
                  llvm_ptr_type(ctx_), {llvm_ptr_type(ctx_)});
              auto result = builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{pointer}).getResult();
              set(op.result, builder_.create<mlir::LLVM::PtrToIntOp>(loc, builder_.getI64Type(), result).getResult());
            } else {
              const bool reset = op.callee == "memory.reset_allocator";
              auto fn = ensure_runtime_fn(module_, builder_, reset ? "nrq_allocator_reset" : "nrq_allocator_destroy",
                                         builder_.getI32Type(), {llvm_ptr_type(ctx_)});
              auto result = builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{pointer}).getResult();
              set(op.result, result);
            }
          }
          return true;
        }
        if (op.callee == "memory.reallocate" && op.result.is_valid() &&
            op.result.type.is_pointer()) {
          if (args.size() != 2) {
            diag_.error("NRQ4005", op.location,
                        "internal: memory.reallocate<T> needs a pointer and a byte count");
            return false;
          }
          mlir::Value pointer = args[0];
          if (!mlir::isa<mlir::LLVM::LLVMPointerType>(pointer.getType()))
            pointer = builder_.create<mlir::LLVM::IntToPtrOp>(
                loc, llvm_ptr_type(ctx_), pointer).getResult();
          mlir::Value bytes = args[1];
          if (bytes.getType() != builder_.getI64Type()) {
            const unsigned bits = bytes.getType().getIntOrFloatBitWidth();
            bytes = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), bytes).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), bytes).getResult();
          }
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_reallocate",
              llvm_ptr_type(ctx_), {llvm_ptr_type(ctx_), builder_.getI64Type()});
          mlir::Value result = builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{pointer, bytes}).getResult();
          // Reallocation moves the object, so the old address must stop being a
          // root before the replacement becomes one. Keep the old registration
          // when reallocation fails and the original block remains valid.
          if (result) {
            emit_gc_root_unregistration(pointer, op.result.type.element_type(), loc);
            emit_gc_root_registration(result, op.result.type.element_type(), loc);
          }
          set(op.result, result);
          return true;
        }
        if (op.callee == "memory.deallocate" ||
            op.callee == "memory.manual_release") {
          if (op.callee == "memory.deallocate" && !op.arguments.empty() &&
              stack_allocations_.find(op.arguments[0].id) !=
                  stack_allocations_.end()) {
            return true;
          }
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_deallocate_checked",
                                     mlir::LLVM::LLVMVoidType::get(ctx_),
                                     {llvm_ptr_type(ctx_)});
          if (!args.empty() && caller_arena_arg_index_ >= 0 &&
              arena_param_aliases_.count(op.arguments[0].id) != 0) {
            // A promoted callee's returned storage lives in the caller's arena when
            // a handle arrived, and on the general allocator when null did. This
            // release is on a path where the storage is not handed back, so it is
            // valid only for the null path: the arena frees in bulk.
            emit_release_unless_arena(args[0],
                                      op.arguments[0].type.element_type(), loc);
            return true;
          }
          if (!args.empty()) {
            mlir::Value pointer = args[0];
            if (!mlir::isa<mlir::LLVM::LLVMPointerType>(pointer.getType())) {
              pointer = builder_.create<mlir::LLVM::IntToPtrOp>(
                  loc, llvm_ptr_type(ctx_), pointer).getResult();
            }
            // The pointer owns collector-visible storage when its pointee has
            // managed fields; remove that root before the block is freed.
            emit_gc_root_unregistration(pointer,
                                        op.arguments[0].type.element_type(), loc);
            builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{pointer});
          }
          return true;
        }
        if (op.callee == "memory.allocate") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_allocate",
                                     llvm_ptr_type(ctx_),
                                     {builder_.getI64Type(), builder_.getI64Type()});
          auto call_op = builder_.create<mlir::LLVM::CallOp>(loc, fn, args);
          if (op.result.is_valid()) {
            mlir::Value result = builder_.create<mlir::LLVM::PtrToIntOp>(
                loc, builder_.getI64Type(), call_op.getResult()).getResult();
            set(op.result, result);
          }
          return true;
        }
        if (op.callee == "gc.get_statistics") {
          static constexpr const char* stat_symbols[] = {
              "nrq_gc_get_allocated_bytes", "nrq_gc_get_collected_bytes",
              "nrq_gc_get_collection_count", "nrq_gc_get_live_objects"};
          mlir::Type stats_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
          mlir::Value stats = builder_.create<mlir::LLVM::UndefOp>(loc, stats_type).getResult();
          for (int index = 0; index < 4; ++index) {
            auto getter = ensure_runtime_fn(module_, builder_, stat_symbols[index],
                                            builder_.getI64Type(), {});
            mlir::Value field = builder_.create<mlir::LLVM::CallOp>(
                loc, getter, mlir::ValueRange{}).getResult();
            stats = builder_.create<mlir::LLVM::InsertValueOp>(
                loc, stats, field,
                builder_.getDenseI64ArrayAttr({index})).getResult();
          }
          set(op.result, stats);
          return true;
        }
        if (op.callee == "gc.set_threshold") {
          auto setter = ensure_runtime_fn(module_, builder_, "nrq_gc_set_threshold",
                                         mlir::LLVM::LLVMVoidType::get(ctx_),
                                         {builder_.getI64Type()});
          if (!args.empty()) {
            mlir::Value threshold = args[0];
            if (threshold.getType() != builder_.getI64Type()) {
              const unsigned bits = threshold.getType().getIntOrFloatBitWidth();
              threshold = bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), threshold).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), threshold).getResult();
            }
            builder_.create<mlir::LLVM::CallOp>(loc, setter, mlir::ValueRange{threshold});
          }
          return true;
        }
        if (op.callee == "gc.enable" || op.callee == "gc.disable" ||
            op.callee == "gc.collect") {
          const char* runtime_sym = op.callee == "gc.enable"
                                        ? "nrq_gc_enable"
                                        : op.callee == "gc.disable"
                                              ? "nrq_gc_disable"
                                              : "nrq_gc_collect";
          auto fn = ensure_runtime_fn(module_, builder_, runtime_sym,
                                     mlir::LLVM::LLVMVoidType::get(ctx_), {});
          builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{});
          return true;
        }

        if (op.callee == "memory.array_bounds_check" && args.size() == 2) {
          emit_bounds_guard(args[0], args[1], loc);
          return true;
        }

        // Check if this is a runtime library builtin call:
        llvm::StringRef callee = op.callee;
        std::string runtime_sym;
        mlir::Type runtime_ret_ty;
        std::vector<mlir::Type> runtime_arg_tys;

        if (callee == "mutex.create") {
          runtime_sym = "nrq_mutex_create";
          runtime_ret_ty = llvm_ptr_type(ctx_);
          runtime_arg_tys = {};
        } else if (callee == "mutex.lock") {
          runtime_sym = "nrq_mutex_lock";
          runtime_ret_ty = mlir::LLVM::LLVMVoidType::get(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "mutex.unlock") {
          runtime_sym = "nrq_mutex_unlock";
          runtime_ret_ty = mlir::LLVM::LLVMVoidType::get(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "mutex.destroy") {
          runtime_sym = "nrq_mutex_destroy";
          runtime_ret_ty = mlir::LLVM::LLVMVoidType::get(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "file.open") {
          runtime_sym = "nrq_file_open_handle";
          runtime_ret_ty = llvm_ptr_type(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_), builder_.getI32Type()};
        } else if (callee == "file.close") {
          runtime_sym = "nrq_file_close_handle";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "file.write") {
          runtime_sym = "nrq_file_write_handle";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_)};
        } else if (callee == "file.read") {
          runtime_sym = "nrq_file_read_handle";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_), builder_.getI64Type()};
        } else if (callee == "file.read_text") {
          runtime_sym = "nrq_file_read_text";
          runtime_ret_ty = llvm_ptr_type(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "file.read_text_owned") {
          runtime_sym = "nrq_file_read_text_owned";
          runtime_ret_ty = llvm_ptr_type(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "file.free_text") {
          runtime_sym = "nrq_file_text_release";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "nrq_ffi_free") {
          // The only release for storage the language did not allocate: a C
          // function's buffer goes back to the C allocator that produced it.
          runtime_sym = "nrq_ffi_free";
          runtime_ret_ty = mlir::LLVM::LLVMVoidType::get(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "file.text_live_count") {
          runtime_sym = "nrq_file_text_live_count";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {};
        } else if (callee == "memory.array_live_count") {
          runtime_sym = "nrq_array_live_count";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {};
        } else if (callee == "file.write_text") {
          runtime_sym = "nrq_file_write_text";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_)};
        } else if (callee == "file.append_text") {
          runtime_sym = "nrq_file_append_text";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_)};
        } else if (callee == "file.exists") {
          runtime_sym = "nrq_file_exists";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "socket.connect") {
          runtime_sym = "nrq_socket_connect";
          runtime_ret_ty = llvm_ptr_type(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_), builder_.getI32Type()};
        } else if (callee == "socket.close") {
          runtime_sym = "nrq_socket_close_handle";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "socket.send") {
          runtime_sym = "nrq_socket_send_text";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_)};
        } else if (callee == "socket.receive") {
          runtime_sym = "nrq_socket_receive";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_),
                             builder_.getI64Type()};
        } else if (callee == "time.cycles") {
          runtime_sym = "nrq_time_cycles";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "time.now") {
          runtime_sym = "nrq_time_now_ms";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "memory.used_bytes") {
          runtime_sym = "nrq_system_used_bytes";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "memory.total_bytes") {
          runtime_sym = "nrq_system_total_bytes";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "memory.device_memory_total_bytes") {
          runtime_sym = "nrq_system_device_memory_total_bytes";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "memory.device_memory_used_bytes") {
          runtime_sym = "nrq_system_device_memory_used_bytes";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "memory.pin_count") {
          runtime_sym = "nrq_gc_pin_count";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "memory.pin_violations") {
          runtime_sym = "nrq_gc_pin_violations";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "gc.is_enabled") {
          // The runtime reports a 32-bit flag, while the language type is bool;
          // adapt_runtime_result performs the narrowing.
          runtime_sym = "nrq_gc_is_enabled";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {};
        } else if (callee == "system.exit") {
          runtime_sym = "nrq_system_exit";
          runtime_ret_ty = mlir::LLVM::LLVMVoidType::get(ctx_);
          runtime_arg_tys = {builder_.getI32Type()};
        } else if (callee == "math.sqrt" || callee == "sqrt") {
          runtime_sym = "nrq_sqrt";
          runtime_ret_ty = builder_.getF64Type();
          runtime_arg_tys = {builder_.getF64Type()};
        } else if (callee == "math.sin" || callee == "sin") {
          runtime_sym = "nrq_sin";
          runtime_ret_ty = builder_.getF64Type();
          runtime_arg_tys = {builder_.getF64Type()};
        } else if (callee == "math.cos" || callee == "cos") {
          runtime_sym = "nrq_cos";
          runtime_ret_ty = builder_.getF64Type();
          runtime_arg_tys = {builder_.getF64Type()};
        } else if (callee == "math.abs" || callee == "abs") {
          runtime_sym = "nrq_abs";
          runtime_ret_ty = builder_.getF64Type();
          runtime_arg_tys = {builder_.getF64Type()};
        } else if (callee == "math.floor" || callee == "floor") {
          runtime_sym = "nrq_floor";
          runtime_ret_ty = builder_.getF64Type();
          runtime_arg_tys = {builder_.getF64Type()};
        } else if (callee == "math.ceil" || callee == "ceil") {
          runtime_sym = "nrq_ceil";
          runtime_ret_ty = builder_.getF64Type();
          runtime_arg_tys = {builder_.getF64Type()};
        }

        if (!runtime_sym.empty()) {
          auto fn = ensure_runtime_fn(module_, builder_, runtime_sym, runtime_ret_ty, runtime_arg_tys);
          llvm::SmallVector<mlir::Value> abi_args;
          abi_args.reserve(args.size());
          for (size_t index = 0; index < args.size(); ++index) {
            mlir::Value value = args[index];
            if (index < runtime_arg_tys.size() && value.getType() != runtime_arg_tys[index]) {
              mlir::Type target = runtime_arg_tys[index];
              if (value.getType().isInteger(1) && target.isInteger()) {
                value = builder_.create<mlir::LLVM::ZExtOp>(loc, target, value).getResult();
              } else if (target.isInteger() &&
                         value.getType().getIntOrFloatBitWidth() != 0) {
                const unsigned source_width = value.getType().getIntOrFloatBitWidth();
                const unsigned target_width = target.getIntOrFloatBitWidth();
                if (source_width < target_width)
                  value = builder_.create<mlir::LLVM::ZExtOp>(loc, target, value).getResult();
                else
                  value = builder_.create<mlir::LLVM::TruncOp>(loc, target, value).getResult();
              }
            }
            abi_args.push_back(value);
          }
          auto call_op = builder_.create<mlir::LLVM::CallOp>(loc, fn, abi_args);
          if (op.result.is_valid()) {
            // The runtime ABI is deliberately narrow (i32 status codes, i64
            // timestamps), while the language types the same call as `int`,
            // `bool` or `string`. Bridge the two here so the TIR result type
            // and the produced SSA value always agree.
            set(op.result, adapt_runtime_result(call_op.getResult(), op.result.type, loc));
          }
          return true;
        }

        // The callee's signature carries a trailing arena handle. A bounded site
        // names the arena this activation created for it; every other site passes
        // null, and the callee picks the general allocator from the handle at
        // runtime. Passing null is what makes one signature serve both kinds of
        // caller, instead of the callee's storage decision being global.
        if (promoted_callees_.count(op.callee) != 0) {
          mlir::Value handle;
          const bool forwarded =
              activation_handle_guaranteed_ ||
              (op.result.is_valid() &&
               forwarded_call_results_.count(op.result.id) != 0);
          const auto promoted = op.result.is_valid()
                                    ? arena_call_regions_.find(op.result.id)
                                    : arena_call_regions_.end();
          if (forwarded) {
            // A direct recursive call that returns what this activation returns:
            // hand it the handle this activation was given, so the whole chain
            // fills one chunk and the outermost caller releases it once.
            if (caller_arena_arg_index_ < 0) {
              diag_.error("NRQ4005", op.location,
                          "internal: forwarded call has no arena parameter");
              return false;
            }
            handle = func_op_.getArgument(
                static_cast<unsigned>(caller_arena_arg_index_));
          } else if (promoted != arena_call_regions_.end()) {
            handle = region_handle(promoted->second, loc);
            if (!handle) {
              diag_.error("NRQ4005", op.location,
                          "internal: promoted call site has no arena handle");
              return false;
            }
          } else {
            handle = builder_
                         .create<mlir::LLVM::ZeroOp>(loc, llvm_ptr_type(ctx_))
                         .getResult();
          }
          args.push_back(handle);
        }
        if (op.result.is_valid()) {
          mlir::Type ret_ty = tir_type_to_mlir(op.result.type, ctx_, builder_);
          auto call_op = builder_.create<mlir::func::CallOp>(
              loc, op.callee, mlir::TypeRange{ret_ty}, args);
          mlir::Value result = call_op.getResult(0);
          root_managed_references_and_release_handoffs(
              result, op.result.type, loc,
              gc_roots_.rooted_call_results.count(op.result.id) != 0);
          set(op.result, result);
        } else {
          builder_.create<mlir::func::CallOp>(
              loc, op.callee, mlir::TypeRange{}, args);
        }
        return true;
      }

      case tir::OpKind::PrintString: {
        builder_.create<nrq::PrintStringOp>(loc, builder_.getStringAttr(op.string_value));
        return true;
      }

      case tir::OpKind::Print: {
        mlir::Value val = get(op.operand);
        if (!val) return true;
        const Type& ty = op.operand.type;

        if (ty.is_bool()) {
          builder_.create<nrq::PrintBoolOp>(loc, val);
        } else if (ty.is_string()) {
          // A string value (for example `file.read_text(...)`) is a NUL-terminated
          // pointer at this point, so it is printed through the runtime.
          auto print_fn = ensure_runtime_fn(module_, builder_, "nrq_print_cstr",
                                            builder_.getI32Type(), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, print_fn, mlir::ValueRange{val});
        } else if (ty.is_char()) {
          builder_.create<nrq::PrintCharOp>(loc, val);
        } else if (ty.is_float()) {
          mlir::Value f64_val = ty.is_f32()
              ? builder_.create<mlir::LLVM::FPExtOp>(loc, builder_.getF64Type(), val).getResult()
              : val;
          builder_.create<nrq::PrintF64Op>(loc, f64_val);
        } else if (ty.is_pointer()) {
          // §8.6: print the address and never dereference. The runtime prints NULL
          // for a null pointer, so absence is visible instead of a bare 0.
          auto print_fn = ensure_runtime_fn(module_, builder_, "nrq_print_pointer",
                                            builder_.getI32Type(), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, print_fn, mlir::ValueRange{val});
        } else if (ty.is_integer()) {
          mlir::Value i64_val = val;
          if (ty.bit_width() < 64) {
            i64_val = ty.is_signed_integer()
                ? builder_.create<mlir::LLVM::SExtOp>(loc, builder_.getI64Type(), val).getResult()
                : builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), val).getResult();
          }
          if (ty.is_signed_integer()) {
            builder_.create<nrq::PrintIntOp>(loc, i64_val);
          } else {
            builder_.create<nrq::PrintUintOp>(loc, i64_val);
          }
        }
        return true;
      }

      case tir::OpKind::Return: {
        mlir::Value val;
        if (op.operand.is_valid()) {
          val = get(op.operand);
          val = pin_managed_references_for_return(
              val, op.operand.type, loc);
        }
        // A return inside a loop leaves that loop too, so the regions scoped to
        // it are destroyed here, innermost first. What remains is the frame's own
        // regions, whose lifetime is the activation. This frame's element
        // cleanups were already emitted earlier in the stream, so nothing that
        // reads region storage runs after this point.
        emit_loop_region_destroys(current_operation_index_, loc, {});
        for (std::size_t index = 0; index < regions_.size(); ++index) {
          if (regions_[index].scope.loop) continue;
          emit_region_destroy(index, loc);
        }
        if (gc_root_frame_marker_) {
          auto end_frame = ensure_runtime_fn(
              module_, builder_, "nrq_gc_root_frame_end",
              mlir::LLVM::LLVMVoidType::get(ctx_),
              {builder_.getI64Type()});
          builder_.create<mlir::LLVM::CallOp>(
              loc, end_frame, mlir::ValueRange{gc_root_frame_marker_});
        }
        if (uses_gc_mutator_) {
          auto mutator_leave = ensure_runtime_fn(
              module_, builder_, "nrq_gc_mutator_leave",
              mlir::LLVM::LLVMVoidType::get(ctx_), {});
          builder_.create<mlir::LLVM::CallOp>(
              loc, mutator_leave, mlir::ValueRange{});
        }
        // The entry function is the only place that can retire process-owned
        // runtime buffers. Doing it at every return edge covers the no-argument
        // `main` as well as the argument wrapper's callee. `system.exit` does
        // not return, so that path still relies on process teardown.
        if (emit_abi_reference_) {
          auto shutdown = ensure_runtime_fn(
              module_, builder_, "nrq_runtime_shutdown",
              mlir::LLVM::LLVMVoidType::get(ctx_), {});
          builder_.create<mlir::LLVM::CallOp>(
              loc, shutdown, mlir::ValueRange{});
        }
        if (op.operand.is_valid()) {
          if (val) {
            builder_.create<mlir::func::ReturnOp>(loc, mlir::ValueRange{val});
          } else {
            builder_.create<mlir::func::ReturnOp>(loc, mlir::ValueRange{});
          }
        } else {
          builder_.create<mlir::func::ReturnOp>(loc, mlir::ValueRange{});
        }
        return true;
      }

      case tir::OpKind::Move: {
        // Ownership transfer is a semantic alias, not a physical copy. Keeping
        // the result mapped to the source value is what preserves the existing
        // zero-copy behaviour.
        mlir::Value source = get(op.operand);
        if (source) set(op.result, source);
        return true;
      }

      case tir::OpKind::Borrow:
      case tir::OpKind::EndBorrow:
      // Already expanded into a release per field, or dropped because a strategy's
      // registry owns the storage. Nothing reaches lowering.
      case tir::OpKind::AggregateFieldDestroy:
        return true;
      case tir::OpKind::Drop: {
        // A pooled allocation goes back to its pool where its owner dies, so the
        // next iteration's site reuses that slot instead of taking another one
        // out of the chunk. Every other Drop is a pure ownership marker: its
        // information lives in the attributes around it.
        const auto region = region_of_value_.find(op.operand.id);
        if (region != region_of_value_.end() &&
            regions_[region->second].pooled) {
          mlir::Value handle = region_handle(region->second, loc);
          mlir::Value slot = get(op.operand);
          if (handle && slot) {
            auto release_fn = ensure_runtime_fn(
                module_, builder_, "nrq_cdmm_pool_release",
                builder_.getI32Type(),
                {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_)});
            builder_.create<mlir::LLVM::CallOp>(
                loc, release_fn, mlir::ValueRange{handle, slot});
          }
        }
        return true;
      }

      case tir::OpKind::Unreachable: {
        builder_.create<mlir::LLVM::UnreachableOp>(loc);
        return true;
      }

      case tir::OpKind::ArrayAlloca: {
        // Alloca for a fixed-size array, hoisted into the entry block.
        mlir::Type arr_mlir = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!arr_mlir) return true;
        set(op.result, alloca_at_entry(arr_mlir, loc));
        record_pointer_alignment(op.result, type_alignment(op.result.type));
        return true;
      }

      case tir::OpKind::SoaColumnPtr: {
        // One element of one column: payload + column_base * count + row * stride.
        // The container has no array-of-structs address, so the offset comes from the
        // described column layout and the object's own element count.
        mlir::Value payload = get(op.lhs);
        mlir::Value row = get(op.operand);
        if (!payload || !row) return true;
        const SoaLayout layout = describe_soa(op.lhs.type);
        const auto order = aggregate_physical_order(op.lhs.type.soa_element());
        std::size_t position = order.size();
        for (std::size_t k = 0; k < order.size(); ++k)
          if (static_cast<int>(order[k]) == op.field_index) { position = k; break; }
        if (position >= layout.columns.size()) {
          diag_.error("NRQ4005", op.location,
                      "internal: container column missing from its layout");
          return true;
        }
        const SoaColumn& column = layout.columns[position];
        mlir::Type i64 = builder_.getI64Type();
        auto constant = [&](int64_t v) {
          return builder_.create<mlir::LLVM::ConstantOp>(
              loc, i64, builder_.getI64IntegerAttr(v)).getResult();
        };
        // `element_count` is the first word of the extended (H16) header and the
        // payload starts 16 bytes after it, so the count is the word at
        // payload - 16. An array always takes the extended representation, so this
        // read can never land in a headerless or compact object. It is the same
        // word the collector sizes the payload with, so it cannot disagree with
        // the block that was actually allocated.
        mlir::Value count_slot = builder_.create<mlir::LLVM::GEPOp>(
            loc, llvm_ptr_type(ctx_), builder_.getI8Type(), payload,
            mlir::ValueRange{constant(-16)}).getResult();
        mlir::Value count_words = builder_.create<mlir::LLVM::LoadOp>(
            loc, builder_.getI32Type(), count_slot).getResult();
        mlir::Value count = builder_.create<mlir::LLVM::ZExtOp>(
            loc, i64, count_words).getResult();
        mlir::Value row64 = row.getType().isInteger(64)
            ? row
            : builder_.create<mlir::LLVM::SExtOp>(loc, i64, row).getResult();
        // Check the row against the container's own element count before any address
        // is formed. An out-of-range row would land inside another column, so this has
        // to fail ahead of the offset arithmetic rather than after it. The same guard
        // the owned-array path uses keeps one cold failure block per function.
        emit_bounds_guard(row64, count, loc);
        mlir::Value column_base = builder_.create<mlir::LLVM::MulOp>(
            loc, i64, count,
            constant(static_cast<int64_t>(column.index_in_bytes))).getResult();
        mlir::Value row_offset = builder_.create<mlir::LLVM::MulOp>(
            loc, i64, row64,
            constant(static_cast<int64_t>(column.element_bytes))).getResult();
        mlir::Value offset = builder_.create<mlir::LLVM::AddOp>(
            loc, i64, column_base, row_offset).getResult();
        mlir::Value address = builder_.create<mlir::LLVM::GEPOp>(
            loc, llvm_ptr_type(ctx_), builder_.getI8Type(), payload,
            mlir::ValueRange{offset}).getResult();
        set(op.result, address);
        // Every column starts at a multiple of its own alignment and the payload is
        // at least max-aligned, so the address keeps the smaller of the two.
        const uint64_t base_alignment = known_pointer_alignment(op.lhs);
        record_pointer_alignment(op.result,
            base_alignment > 1 ? std::min(base_alignment, column.alignment)
                               : column.alignment);
        return true;
      }
      case tir::OpKind::GetElementPtr: {
        // Pointer into an array or struct field.
        mlir::Value base_ptr = get(op.lhs);
        if (!base_ptr) return true;

        mlir::Type result_elem_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!result_elem_type) return true;

        // Build GEP indices: [0, dynamic_index] for arrays, [0, const_field] for structs.
        mlir::Value idx;
        if (op.operand.is_valid()) {
          idx = get(op.operand);
        } else {
          int field_index = op.field_index >= 0 ? op.field_index : 0;
          if (op.lhs.type.is_struct() || op.lhs.type.is_tuple()) {
            field_index = aggregate_physical_index(op.lhs.type, field_index);
          }
          idx = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(field_index)).getResult();
        }

        // The base type for GEP is the array/struct type; we use a zero-based outer index.
        mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
            loc, builder_.getI32Type(), builder_.getI32IntegerAttr(0)).getResult();

        // Determine the pointee type from the base value's allocation type.
        const Type& base_type = op.lhs.type;
        mlir::Type pointee_type = (base_type.is_pointer() || base_type.is_managed_pointer())
            ? tir_type_to_mlir(op.result.type, ctx_, builder_)
            : tir_type_to_mlir(base_type, ctx_, builder_);
        if (!pointee_type) {
          // Fall back: opaque pointer arithmetic using i8 GEP.
          set(op.result, base_ptr);
          return true;
        }

        mlir::Value gep = (base_type.is_pointer() || base_type.is_managed_pointer())
            ? builder_.create<mlir::LLVM::GEPOp>(
                  loc, llvm_ptr_type(ctx_), pointee_type, base_ptr,
                  mlir::ValueRange{idx}).getResult()
            : builder_.create<mlir::LLVM::GEPOp>(
                  loc, llvm_ptr_type(ctx_), pointee_type, base_ptr,
                  mlir::ValueRange{zero, idx}).getResult();
        set(op.result, gep);
        // A field or element sits at a multiple of its own alignment inside a
        // naturally aligned aggregate, so the derived pointer keeps the smaller
        // of the base's alignment and the member's.
        const uint64_t base_alignment = known_pointer_alignment(op.lhs);
        const uint64_t member_alignment = type_alignment(op.result.type);
        const bool array_element = op.lhs.type.is_array() ||
                                   op.lhs.type.is_owned_array() ||
                                   op.lhs.type.is_array_view();
        if (base_alignment > 1) {
          record_pointer_alignment(op.result,
                                   std::min(base_alignment, member_alignment));
        } else if (array_element) {
          // Element `k` sits at `k * stride`, and every element type's size is
          // a multiple of its alignment, so an element pointer is aligned even
          // when the array's own base pointer came from somewhere unknown.
          record_pointer_alignment(op.result, member_alignment);
        }
        return true;
      }

      case tir::OpKind::AggregatePack: {
        // Pack multiple values into a struct/tuple value using llvm.insertvalue.
        mlir::Type agg_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!agg_type) {
          // Unsupported aggregate type — emit undef.
          set(op.result, {});
          return true;
        }

        const Type& agg_tir_type = op.result.type;
        const bool is_aggregate = agg_tir_type.is_struct() || agg_tir_type.is_tuple();
        mlir::Value agg = builder_.create<mlir::LLVM::UndefOp>(loc, agg_type).getResult();
        for (std::size_t i = 0; i < op.aggregate_elements.size(); ++i) {
          mlir::Value elem = get(op.aggregate_elements[i]);
          if (!elem) continue;
          const int64_t slot = is_aggregate
              ? aggregate_physical_index(agg_tir_type, static_cast<int>(i))
              : static_cast<int64_t>(i);
          agg = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, agg, elem, builder_.getDenseI64ArrayAttr({slot})).getResult();
        }
        set(op.result, agg);
        return true;
      }

      case tir::OpKind::AggregateExtract: {
        // Extract a field from a struct/tuple/array value using llvm.extractvalue.
        mlir::Value agg = get(op.operand);
        if (!agg) return true;

        mlir::Type result_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!result_type) return true;

        int idx = op.field_index >= 0 ? op.field_index : 0;
        if (op.operand.type.is_struct() || op.operand.type.is_tuple()) {
          idx = aggregate_physical_index(op.operand.type, idx);
        }

        mlir::Value extracted = builder_.create<mlir::LLVM::ExtractValueOp>(
            loc, agg,
            builder_.getDenseI64ArrayAttr({static_cast<int64_t>(idx)})).getResult();
        set(op.result, extracted);
        return true;
      }

      case tir::OpKind::Length: {
        mlir::Value value = get(op.operand);
        if (!value) return true;

        std::string runtime_sym;
        if (op.operand.type.is_string()) {
          runtime_sym = "nrq_strlen";
        } else if (op.operand.type.is_dynamic_array() ||
                   op.operand.type.is_owned_array() ||
                   op.operand.type.is_array_view()) {
          // Length is the container's second word, so reading it is a field
          // extraction; only text still needs a runtime call.
          mlir::Value length = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, value, builder_.getDenseI64ArrayAttr({1})).getResult();
          set(op.result, adapt_runtime_result(length, op.result.type, loc));
          return true;
        } else {
          diag_.error("NRQ4005", op.location,
                      "internal: no length operation for type '" +
                          op.operand.type.to_string() + "'");
          return false;
        }

        auto fn = ensure_runtime_fn(module_, builder_, runtime_sym,
                                    builder_.getI64Type(), {llvm_ptr_type(ctx_)});
        mlir::Value length = builder_.create<mlir::LLVM::CallOp>(
            loc, fn, mlir::ValueRange{value}).getResult();
        set(op.result, adapt_runtime_result(length, op.result.type, loc));
        return true;
      }

      case tir::OpKind::DynamicIndex: {
        mlir::Value array = get(op.lhs);
        mlir::Value index = get(op.operand);
        if (!array || !index) return true;

        if (op.lhs.type.is_dynamic_array() || op.lhs.type.is_owned_array() ||
            op.lhs.type.is_array_view()) {
          const mlir::Type i64 = builder_.getI64Type();
          if (index.getType() != i64) {
            const unsigned bits = index.getType().getIntOrFloatBitWidth();
            index = bits < 64
                ? (op.operand.type.is_signed_integer()
                       ? builder_.create<mlir::LLVM::SExtOp>(loc, i64, index).getResult()
                       : builder_.create<mlir::LLVM::ZExtOp>(loc, i64, index).getResult())
                : builder_.create<mlir::LLVM::TruncOp>(loc, i64, index).getResult();
          }
          mlir::Value data = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, array, builder_.getDenseI64ArrayAttr({0})).getResult();
          mlir::Value length = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, array, builder_.getDenseI64ArrayAttr({1})).getResult();
          emit_bounds_guard(index, length, loc);
          mlir::Type element_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
          mlir::Value pointer = builder_.create<mlir::LLVM::GEPOp>(
              loc, llvm_ptr_type(ctx_), element_type, data,
              mlir::ValueRange{index}).getResult();
          auto element = builder_.create<mlir::LLVM::LoadOp>(
              loc, element_type, pointer);
          // The element sits at `index * stride`, so it is aligned as long as
          // the data pointer is, which the runtime and the compiler guarantee.
          const uint64_t element_alignment =
              std::max<uint64_t>(1, type_alignment(op.result.type));
          if (element_alignment > 1) element.setAlignment(element_alignment);
          set(op.result, element.getResult());
          return true;
        }

        diag_.error("NRQ4005", op.location,
                    "internal: dynamic indexing of non-dynamic array type '" +
                        op.lhs.type.to_string() + "'");
        return false;
      }

      case tir::OpKind::ArrayAppend: {
        mlir::Value slot = get(op.lhs);
        mlir::Value element = get(op.operand);
        if (!slot || !element) return true;
        const Type element_type = op.lhs.type.element_type();
        const std::uint32_t element_size = element_type.byte_size();
        if (element_size == 0) {
          diag_.error("NRQ4005", op.location,
                      "internal: element type '" + element_type.to_string() +
                          "' has no fixed layout for a dynamic array");
          return false;
        }
        mlir::Type array_type = tir_type_to_mlir(op.lhs.type, ctx_, builder_);
        mlir::Type element_mlir =
            tir_type_to_mlir(element_type, ctx_, builder_);
        const mlir::Type i64 = builder_.getI64Type();
        auto constant = [&](int64_t value) {
          return builder_
              .create<mlir::LLVM::ConstantOp>(
                  loc, i64, builder_.getI64IntegerAttr(value))
              .getResult();
        };
        auto field = [&](mlir::Value aggregate, int64_t index) {
          return builder_
              .create<mlir::LLVM::ExtractValueOp>(
                  loc, aggregate, builder_.getDenseI64ArrayAttr({index}))
              .getResult();
        };

        // The slot is the array's only storage. Every step reads the header it
        // just wrote, so the value the destructor loads at scope exit is the one
        // growth produced - the write-back is what makes the buffer reachable
        // for the release.
        mlir::Value header = builder_.create<mlir::LLVM::LoadOp>(
            loc, array_type, slot).getResult();
        mlir::Value length = field(header, 1);
        mlir::Value capacity = field(header, 2);
        mlir::Value full = builder_.create<mlir::LLVM::ICmpOp>(
            loc, mlir::LLVM::ICmpPredicate::eq, length, capacity).getResult();
        mlir::Block* grow_block = func_op_.addBlock();
        mlir::Block* append_block = func_op_.addBlock();
        builder_.create<mlir::LLVM::CondBrOp>(loc, full, grow_block,
                                             append_block);

        builder_.setInsertionPointToStart(grow_block);
        mlir::Value data = field(header, 0);
        // Growth policy: the first element starts at eight slots, and every
        // later overflow doubles, so appending n elements costs O(log n)
        // reallocations and the amortized cost per element stays constant.
        mlir::Value empty = builder_.create<mlir::LLVM::ICmpOp>(
            loc, mlir::LLVM::ICmpPredicate::eq, capacity, constant(0))
                                .getResult();
        mlir::Value doubled = builder_.create<mlir::LLVM::MulOp>(
            loc, capacity, constant(2)).getResult();
        mlir::Value new_capacity = builder_
            .create<mlir::LLVM::SelectOp>(loc, empty, constant(8), doubled)
            .getResult();
        auto reallocate = ensure_runtime_fn(
            module_, builder_, "nrq_array_reallocate", llvm_ptr_type(ctx_),
            {llvm_ptr_type(ctx_), i64, i64, i64, i64});
        mlir::Value grown =
            builder_
                .create<mlir::LLVM::CallOp>(
                    loc, reallocate,
                    mlir::ValueRange{data, capacity, new_capacity,
                                     constant(static_cast<int64_t>(element_size)),
                                     constant(static_cast<int64_t>(
                                         std::max<std::uint64_t>(
                                             1, element_type.alignment())))})
                .getResult();
        mlir::Value grown_header = builder_
            .create<mlir::LLVM::InsertValueOp>(
                loc, header, grown, builder_.getDenseI64ArrayAttr({0}))
            .getResult();
        grown_header = builder_
            .create<mlir::LLVM::InsertValueOp>(
                loc, grown_header, new_capacity,
                builder_.getDenseI64ArrayAttr({2}))
            .getResult();
        builder_.create<mlir::LLVM::StoreOp>(loc, grown_header, slot);
        builder_.create<mlir::LLVM::BrOp>(loc, append_block);

        // Both paths converge here and take the header from the slot, so the
        // element lands in the buffer the slot names whichever path ran.
        builder_.setInsertionPointToStart(append_block);
        mlir::Value stored = builder_.create<mlir::LLVM::LoadOp>(
            loc, array_type, slot).getResult();
        mlir::Value stored_data = field(stored, 0);
        mlir::Value stored_length = field(stored, 1);
        mlir::Value destination = builder_
            .create<mlir::LLVM::GEPOp>(loc, llvm_ptr_type(ctx_), element_mlir,
                                       stored_data,
                                       mlir::ValueRange{stored_length})
            .getResult();
        auto element_store =
            builder_.create<mlir::LLVM::StoreOp>(loc, element, destination);
        // The element sits at `length * stride`, so it is aligned whenever the
        // buffer is; the runtime allocated the buffer for that alignment.
        const uint64_t element_alignment =
            std::max<uint64_t>(1, element_type.alignment());
        if (element_alignment > 1) element_store.setAlignment(element_alignment);
        mlir::Value new_length = builder_.create<mlir::LLVM::AddOp>(
            loc, stored_length, constant(1)).getResult();
        mlir::Value updated = builder_
            .create<mlir::LLVM::InsertValueOp>(
                loc, stored, new_length, builder_.getDenseI64ArrayAttr({1}))
            .getResult();
        builder_.create<mlir::LLVM::StoreOp>(loc, updated, slot);
        set(op.result, adapt_runtime_result(new_length, op.result.type, loc));
        return true;
      }
    }
    return true;
  }

  mlir::ModuleOp module_;
  const tir::Function& tir_fn_;
  mlir::func::FuncOp func_op_;
  mlir::OpBuilder& builder_;
  mlir::MLIRContext* ctx_;
  DiagnosticEngine& diag_;
  bool fast_math_ = false;
  // Development configuration: emit the observable hooks the debug runtime
  // provides. Off in every production build, which therefore has no hooks to
  // strip and no calls to them.
  RuntimeConfiguration configuration_ = RuntimeConfiguration::Production;
  // True for the one function that is the program's C entry point.
  bool emit_abi_reference_ = false;
  std::unordered_map<int, StackAllocationLayout> stack_allocations_;
  // One entry per shared region automated strategy selection chose: the scratch
  // arena of a scope, or one pool per layout group, each carrying the scope whose
  // lifetime its storage has.
  std::vector<RegionSelection> regions_;
  // Where each region publishes its handle. Always a frame slot: an alloca inside
  // a loop body would grow the stack once per iteration.
  struct RegionSlotState {
    mlir::Value handle_slot;
  };
  std::vector<RegionSlotState> region_slots_;
  std::unordered_map<int, std::size_t> region_of_site_;
  // This activation's arena, when a caller created one for the storage this
  // function returns: the trailing argument its signature gained.
  int caller_arena_arg_index_ = -1;
  // Allocation sites whose storage leaves through the return, so they are the
  // ones a caller's arena can serve.
  std::unordered_set<int> arena_param_sites_;
  // Every value that names the storage those sites fill. A release of one of them
  // is only valid when the caller passed null: a real handle means the arena owns
  // the storage and frees it wholesale.
  std::unordered_set<int> arena_param_aliases_;
  // Call sites served from this activation's arena, and the region publishing
  // the handle they receive.
  std::unordered_map<int, std::size_t> arena_call_regions_;
  // Callees in this module whose signature carries a trailing arena handle: every
  // call to one of them supplies that argument, whether it names an arena or not.
  std::unordered_set<std::string> promoted_callees_;
  // Recursive call sites that forward this activation's own arena handle. They are
  // the chain that fills one chunk: the outermost bounded caller created it, and
  // each activation hands the same handle to the next.
  std::unordered_set<int> forwarded_call_results_;
  // Sites this activation fills from the handle its caller gave it, and whether
  // every call site passes one. A guaranteed handle means the body needs no null
  // arm and no chunk of its own: this activation lives inside its caller's arena.
  std::unordered_set<int> activation_sites_;
  // Allocation sites the collector owns: their payload's type can close a cycle
  // through a reference count, so no deterministic release describes them.
  std::unordered_set<int> collector_sites_;

  // The declared field types of every struct in the program. Aggregate layouts in
  // TIR have their pointer ownership erased, so a store through a field address or
  // a collected payload's reference counts are read from here.
  const StructFieldTable* struct_types_ = nullptr;
  // Payload struct name -> the fields that own storage, with the release each
  // one calls. The arena teardown registry is fed from this table.
  std::unordered_map<std::string, PayloadTeardowns> payload_field_releases_;
  bool activation_handle_guaranteed_ = false;
  std::unordered_map<int, std::size_t> region_of_value_;
  // Loop-scoped regions whose create belongs at the header label at this position
  // in the operation stream.
  std::unordered_map<std::size_t, std::vector<std::size_t>>
      loop_regions_at_header_;
  std::unordered_map<int, uint64_t> pointer_alignment_;
  GcRootPlan gc_roots_;
  bool uses_gc_mutator_ = false;
  bool needs_gc_root_frame_ = false;
  bool has_collection_point_ = false;
  bool has_late_root_registrations_ = false;
  std::unordered_set<int> unobserved_stack_owned_addresses_;
  std::unordered_map<int, std::size_t> label_positions_;
  mlir::Block* entry_block_;
  std::vector<PendingGcRoot> pending_gc_roots_;
  // Container allocations whose object is published through a root slot that
  // exists for the call alone, keyed by the call's TIR result. A following store
  // into a fresh managed slot takes that slot over as the publish target.
  // Slots created ahead of the declaration that introduces them, so an
  // allocation can publish into the local that will hold its result.
  std::unordered_map<int, mlir::Value> precreated_slots_;
  // Reference most recently stored through the write barrier into a stack
  // address, keyed by the TIR address value. `nrq_gc_root_store` writes the
  // reference into the slot before returning, so a load of the same address in
  // the same straight-line region can use the value instead of reloading it, and
  // LLVM cannot make that substitution itself because a call sits in between.
  // Calls and control-flow joins clear the map: only straight-line reasoning is
  // sound here.
  std::unordered_map<int, mlir::Value> forwarded_root_values_;
  std::vector<mlir::Value> values_;
  std::unordered_map<int, mlir::Block*> label_blocks_;
  StringLiteralTable& string_literals_;
  std::size_t current_operation_index_ = 0;
  mlir::Value gc_root_frame_marker_;
  mlir::Block* cached_cfg_block_ = nullptr;
  mlir::Value cached_cfg64_;
  mlir::Block* bounds_fail_block_ = nullptr;
  std::vector<mlir::Block*> cold_alloc_blocks_;
  mlir::Value safepoint_counter_;
  SlotPromotion promoted_slots_;
};

// ---------------------------------------------------------------------------
// Automated strategy selection (Phase Psi)
// ---------------------------------------------------------------------------
//
// An explicit allocation is the point where the language lets the programmer
// choose storage. When the programmer said nothing, the compiler answers:
//
//   * the stack, for a site the stack analysis already proved placeable; that
//     promotion is cheaper than any region and keeps precedence;
//   * the activation's scratch arena, for a site whose pointer the lifetime
//     classifier proved bounded, whose payload is plain data, and which no
//     hand-written release or allocator switch overrides;
//   * the general allocator otherwise, which is the fallback the language
//     already had.
//
// Every condition is required. An arena cannot hold managed references (nothing
// traces its memory) or payloads that need field destruction (it releases in
// bulk), so the payload check is a correctness gate rather than a heuristic.
// An explicit release is a promise about who frees the storage, and an allocator
// switch is the programmer's own answer to the same question; both win over the
// classifier.
struct HybridSelection {
  std::vector<RegionSelection> regions;
  // Call sites whose result a promoted callee fills into this activation's arena,
  // and the region that publishes the handle those calls receive.
  std::unordered_map<int, std::size_t> arena_call_regions;
  // Every value naming a region-selected allocation. The Drop that would release
  // it must not expand into a call: the region releases the storage wholesale.
  std::unordered_set<int> region_aliases;
  // Drops that belong to a pool. They keep the ownership record and lower into a
  // release against the pool that handed the slot out.
  std::unordered_set<int> pool_aliases;
  // Sites filled from this activation's caller arena instead of a chunk of its
  // own. The allocation lowering passes the incoming handle to the arena.
  std::unordered_set<int> activation_sites;
};

// Candidates are chosen before the Drop expansion below, because that is the
// point where a release can still be suppressed. Whether the stack analysis
// claims one is decided afterwards, and lowering keeps the stack first: a
// candidate the stack promotes never reaches the arena branch.
// A site the stack promotes keeps the stack: it costs no bookkeeping at all. The
// whole pool group goes when any of its sites is claimed there, because one group
// is one pool and a pool of some sites would not match the layout the group was
// built from; the remaining sites fall back to the arena on their own.
void drop_stack_claimed(
    HybridSelection& selection,
    const std::unordered_map<int, StackAllocationLayout>& stack_allocations) {
  std::vector<RegionSelection> kept;
  for (RegionSelection& region : selection.regions) {
    if (region.pooled) {
      bool claimed = false;
      for (int site : region.sites)
        claimed |= stack_allocations.count(site) != 0;
      if (!claimed) kept.push_back(std::move(region));
      continue;
    }
    // An arena is shared by layout, so the sites the stack did not claim keep
    // theirs rather than losing the region to a neighbour's promotion.
    std::vector<int> sites;
    for (int site : region.sites)
      if (stack_allocations.count(site) == 0) sites.push_back(site);
    if (sites.empty()) continue;
    region.sites = std::move(sites);
    kept.push_back(std::move(region));
  }
  selection.regions = std::move(kept);
}

// `serve_from_caller` is set for a promoted activation whose every caller passes a
// real handle: its arena is the caller's, so it creates none of its own. Its
// arena-eligible sites are recorded in `activation_sites` for the allocation
// lowering, and their aliases still join `region_aliases` so the release the
// binding would emit is suppressed - the arena owns that storage either way.
HybridSelection select_hybrid_strategies(
    const tir::Function& function,
    const std::vector<tir::BoundedCallResult>& promoted_calls,
    bool serve_from_caller) {
  HybridSelection selection;

  for (const tir::Op& op : function.operations())
    if (op.kind == tir::OpKind::Call && op.callee == "memory.use_allocator")
      return selection;

  // An arena this activation owns for storage a callee fills: one frame region per
  // promoted call site, created at entry and destroyed on the way out, exactly
  // like a region the function chose for itself. The call site is registered as a
  // region member so the region survives its own audit, and its aliases suppress
  // the release the binding would otherwise emit - the arena owns the storage now.
  for (const tir::BoundedCallResult& site : promoted_calls) {
    if (serve_from_caller) {
      selection.activation_sites.insert(site.result);
      selection.region_aliases.insert(site.aliases.begin(), site.aliases.end());
      continue;
    }
    RegionSelection region;
    region.pooled = false;
    // The classifier already chose the narrowest scope that holds the call and
    // every use of its storage. A loop-scoped site gets its arena created on the
    // loop's entry edge and released on its exits, so the peak stays one
    // iteration's worth; anything else lives until the frame returns.
    region.scope.loop = site.loop_scoped;
    region.scope.begin = site.scope_begin;
    region.scope.end = site.scope_end;
    region.scope.header_label = site.scope_header;
    region.alignment = 8;
    region.sites.push_back(site.result);
    region.aliases = site.aliases;
    const std::size_t index = selection.regions.size();
    selection.arena_call_regions.emplace(site.result, index);
    selection.regions.push_back(std::move(region));
    selection.region_aliases.insert(site.aliases.begin(), site.aliases.end());
  }

  const tir::LifetimeAnalysis lifetimes =
      tir::LifetimeAnalysis::analyze(function);
  if (lifetimes.empty()) return selection;

  // One region per scope and layout: sites that share a loop header and a layout
  // share one handle, and a site whose storage outlives its loop keeps the
  // frame's. The scope is what decides where the create and the destroy go.
  std::unordered_map<std::string, std::size_t> region_index;
  auto region_for = [&](const tir::AllocationSite& site,
                        const tir::Op& op) -> std::size_t {
    std::string key = site.loop_scoped
                          ? std::to_string(site.scope_begin) + ":" +
                                std::to_string(site.scope_end) + "|"
                          : std::string("frame|");
    key += site.pool_eligible ? "pool:" + site.layout_key
                              : std::string("arena");
    const auto found = region_index.find(key);
    if (found != region_index.end()) return found->second;

    RegionSelection region;
    region.pooled = site.pool_eligible;
    region.layout_key = site.layout_key;
    region.slot_bytes = site.payload_bytes;
    const std::size_t alignment =
        type_alignment(op.result.type.element_type());
    region.alignment = alignment == 0 ? 1 : alignment;
    region.scope.loop = site.loop_scoped;
    region.scope.begin = site.scope_begin;
    region.scope.end = site.scope_end;
    region.scope.header_label = site.scope_header;
    const std::size_t index = selection.regions.size();
    selection.regions.push_back(std::move(region));
    region_index.emplace(key, index);
    return index;
  };

  for (const tir::Op& op : function.operations()) {
    if (!tir::is_allocation_site(op)) continue;
    // Two arguments name an allocator: that hint is the programmer's decision.
    if (op.arguments.size() != 1) continue;
    const tir::AllocationSite* site = lifetimes.site(op.result.id);
    if (site == nullptr || site->lifetime != tir::AllocationLifetime::Bounded)
      continue;
    if (site->manually_released) continue;
    // A site nobody touches is not a local working set; claiming it would buy
    // nothing and would replace a release the programmer can see with a chunk.
    if (!site->used_locally) continue;
    if (!op.result.type.is_pointer()) continue;
    if (!tir::is_arena_eligible_payload(op.result.type.element_type()))
      continue;
    if (serve_from_caller) {
      // Filled from the handle this activation was given, not from a chunk of its
      // own: the caller created the chunk and releases it.
      selection.activation_sites.insert(op.result.id);
      selection.region_aliases.insert(site->aliases.begin(), site->aliases.end());
      continue;
    }

    const std::size_t index = region_for(*site, op);
    selection.regions[index].sites.push_back(op.result.id);
    selection.regions[index].aliases.insert(site->aliases.begin(),
                                            site->aliases.end());
    selection.region_aliases.insert(site->aliases.begin(),
                                    site->aliases.end());
    // Repeated storage: a pool recycles one slot per iteration instead of
    // growing a bump region for the whole scope.
    if (site->pool_eligible)
      selection.pool_aliases.insert(site->aliases.begin(),
                                    site->aliases.end());
  }
  return selection;
}

// The Windows CRT calls `main(int argc, char** argv)`. A Nirqoba entry point
// that declares `string[] args` needs a different signature, so it is emitted as
// `nrq_entry_args` and this wrapper marshals the native arguments into the
// runtime-managed array before calling it.
static bool emit_entry_wrapper(mlir::ModuleOp module, mlir::OpBuilder& builder,
                               const tir::Function& user_main,
                               llvm::StringRef user_symbol,
                               bool callee_needs_region,
                               RuntimeConfiguration configuration,
                               DiagnosticEngine& diagnostics) {
  mlir::MLIRContext* ctx = module.getContext();
  mlir::Location loc = mlir::UnknownLoc::get(ctx);
  mlir::Type i32 = builder.getI32Type();
  mlir::Type ptr = llvm_ptr_type(ctx);

  mlir::Type user_ret;
  if (!user_main.return_type().is_void()) {
    user_ret = tir_type_to_mlir(user_main.return_type(), ctx, builder);
    if (!user_ret) {
      diagnostics.error("NRQ4005", SourceLocation{},
                        "internal: unsupported 'main' return type in entry wrapper");
      return false;
    }
  }

  builder.setInsertionPointToEnd(module.getBody());
  auto wrapper = builder.create<mlir::func::FuncOp>(
      loc, "main", mlir::FunctionType::get(ctx, {i32, ptr}, {i32}));
  mlir::Block* entry = wrapper.addEntryBlock();
  builder.setInsertionPointToStart(entry);

  // This object may only be linked against the runtime library built from the
  // same configuration. Each library defines exactly one of these two symbols
  // and the compiler names the one its own configuration requires, so the wrong
  // library is an unresolved external instead of a program that observes the
  // heap differently from the way it was compiled to.
  auto abi_check = ensure_runtime_fn(
      module, builder, runtime_abi_symbol(configuration),
      mlir::LLVM::LLVMVoidType::get(ctx), {});
  builder.create<mlir::LLVM::CallOp>(loc, abi_check, mlir::ValueRange{});

  auto args_build = ensure_runtime_fn(module, builder, "nrq_args_build", ptr, {i32, ptr});
  mlir::Value array = builder.create<mlir::LLVM::CallOp>(
      loc, args_build,
      mlir::ValueRange{entry->getArgument(0), entry->getArgument(1)}).getResult();

  // `string[] args` is an ordinary dynamic array value, while the runtime
  // descriptor is {length, items}. Marshalling it here is what keeps the
  // language value one representation: length is a field read and indexing walks
  // the item block.
  //
  // The table of pointers is copied into a buffer from the array allocator
  // because the language owns every array it hands to user code: releasing
  // `args` - at main's exit, or in a callee it was passed to - has to return
  // exactly the storage the language allocated. The argument strings stay where
  // the CRT put them; only the table is copied.
  if (!user_main.parameters().empty() &&
      user_main.parameters().front().type.is_dynamic_array()) {
    mlir::Type i64 = builder.getI64Type();
    mlir::Type descriptor_type =
        mlir::LLVM::LLVMStructType::getLiteral(ctx, {i64, ptr});
    mlir::Value zero_index = builder.create<mlir::LLVM::ConstantOp>(
        loc, i32, builder.getI32IntegerAttr(0)).getResult();
    mlir::Value length_pointer = builder.create<mlir::LLVM::GEPOp>(
        loc, ptr, descriptor_type, array,
        mlir::ValueRange{zero_index, zero_index}).getResult();
    mlir::Value length = builder.create<mlir::LLVM::LoadOp>(
        loc, i64, length_pointer).getResult();
    mlir::Value one_index = builder.create<mlir::LLVM::ConstantOp>(
        loc, i32, builder.getI32IntegerAttr(1)).getResult();
    mlir::Value items_pointer = builder.create<mlir::LLVM::GEPOp>(
        loc, ptr, descriptor_type, array,
        mlir::ValueRange{zero_index, one_index}).getResult();
    mlir::Value items = builder.create<mlir::LLVM::LoadOp>(
        loc, ptr, items_pointer).getResult();

    mlir::Value empty_length = builder.create<mlir::LLVM::ConstantOp>(
        loc, i64, builder.getI64IntegerAttr(0)).getResult();
    mlir::Value element_bytes = builder.create<mlir::LLVM::ConstantOp>(
        loc, i64, builder.getI64IntegerAttr(8)).getResult();
    auto create = ensure_runtime_fn(module, builder, "nrq_array_create", ptr,
                                    {i64, i64, i64});
    mlir::Value buffer = builder.create<mlir::LLVM::CallOp>(
        loc, create,
        mlir::ValueRange{length, element_bytes, element_bytes}).getResult();

    // An argument-less program has nothing to copy, and the empty array's buffer
    // is null, so the copy is skipped rather than handed two null pointers. The
    // branch keeps the empty case out of the memory intrinsic entirely.
    mlir::Value no_arguments = builder.create<mlir::LLVM::ICmpOp>(
        loc, mlir::LLVM::ICmpPredicate::eq, length, empty_length).getResult();
    mlir::Block* copy_block = wrapper.addBlock();
    mlir::Block* arguments_ready = wrapper.addBlock();
    builder.create<mlir::LLVM::CondBrOp>(loc, no_arguments, arguments_ready,
                                        copy_block);
    builder.setInsertionPointToStart(copy_block);
    mlir::Value byte_count = builder.create<mlir::LLVM::MulOp>(
        loc, length, element_bytes).getResult();
    auto copy = ensure_runtime_fn(module, builder, "memcpy", ptr,
                                  {ptr, ptr, i64});
    builder.create<mlir::LLVM::CallOp>(
        loc, copy, mlir::ValueRange{buffer, items, byte_count});
    builder.create<mlir::LLVM::BrOp>(loc, arguments_ready);
    builder.setInsertionPointToStart(arguments_ready);

    mlir::Type array_type = tir_type_to_mlir(
        user_main.parameters().front().type, ctx, builder);
    if (!array_type) {
      diagnostics.error("NRQ4005", SourceLocation{},
                        "internal: unsupported 'main' parameter type in entry wrapper");
      return false;
    }
    mlir::Value value = builder.create<mlir::LLVM::UndefOp>(
        loc, array_type).getResult();
    value = builder.create<mlir::LLVM::InsertValueOp>(
        loc, value, buffer, builder.getDenseI64ArrayAttr({0})).getResult();
    value = builder.create<mlir::LLVM::InsertValueOp>(
        loc, value, length, builder.getDenseI64ArrayAttr({1})).getResult();
    value = builder.create<mlir::LLVM::InsertValueOp>(
        loc, value, length, builder.getDenseI64ArrayAttr({2})).getResult();
    array = value;
  }

  // This wrapper is entered by the C runtime, so it is the outermost place a
  // managed program can start. Opening the region here is what lets the callee and
  // everything it calls run inside one region instead of each activation entering
  // and leaving the protocol on its own.
  if (callee_needs_region) {
    auto enter = ensure_runtime_fn(
        module, builder, "nrq_gc_mutator_enter",
        mlir::LLVM::LLVMVoidType::get(ctx), {});
    builder.create<mlir::LLVM::CallOp>(loc, enter, mlir::ValueRange{});
  }

  mlir::Value exit_code;
  if (user_ret) {
    mlir::Value result = builder.create<mlir::func::CallOp>(
        loc, user_symbol, mlir::TypeRange{user_ret}, mlir::ValueRange{array}).getResult(0);

    if (result.getType() == i32) {
      exit_code = result;
    } else if (mlir::isa<mlir::IntegerType>(result.getType())) {
      const unsigned bits = result.getType().getIntOrFloatBitWidth();
      exit_code = bits < 32
          ? builder.create<mlir::LLVM::SExtOp>(loc, i32, result).getResult()
          : builder.create<mlir::LLVM::TruncOp>(loc, i32, result).getResult();
    } else {
      diagnostics.error("NRQ4005", SourceLocation{},
                        "internal: 'main' must return an integer in the entry wrapper");
      return false;
    }
  } else {
    builder.create<mlir::func::CallOp>(
        loc, user_symbol, mlir::TypeRange{}, mlir::ValueRange{array});
    exit_code = builder.create<mlir::LLVM::ConstantOp>(
        loc, i32, builder.getI32IntegerAttr(0)).getResult();
  }

  if (callee_needs_region) {
    // Leaving publishes what the region accumulated, which is exactly the TLAB
    // batching the region exists for.
    auto leave = ensure_runtime_fn(
        module, builder, "nrq_gc_mutator_leave",
        mlir::LLVM::LLVMVoidType::get(ctx), {});
    builder.create<mlir::LLVM::CallOp>(loc, leave, mlir::ValueRange{});
  }

  builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange{exit_code});
  return true;
}

}

GcRootAnalysisStats gc_root_analysis_stats() { return analysis_stats(); }
void reset_gc_root_analysis_stats() { analysis_stats() = GcRootAnalysisStats{}; }

// Storage nobody reads and nothing destroys is not an allocation at all. The
// three facts this needs are the ones strategy selection reads - the pointer stays
// in the activation, nothing dereferences it, and the payload needs no
// destructor - so the decision costs no new analysis.
//
// Eliding here, before any strategy claims the site, is what keeps the effect
// honest: the general allocator is not called, no arena is created for it and no
// release follows it, because there is no site left to select.
void elide_dead_allocations(tir::Function& function) {
  const tir::LifetimeAnalysis lifetimes = tir::LifetimeAnalysis::analyze(function);
  if (lifetimes.empty()) return;

  std::unordered_set<int> aliases;
  for (const tir::Op& op : function.operations()) {
    if (!tir::is_allocation_site(op)) continue;
    const tir::AllocationSite* site = lifetimes.site(op.result.id);
    if (site == nullptr) continue;
    if (site->lifetime != tir::AllocationLifetime::Bounded) continue;
    if (site->used_locally || site->manually_released) continue;
    if (!op.result.type.is_pointer()) continue;
    if (!tir::is_arena_eligible_payload(op.result.type.element_type())) continue;
    aliases.insert(site->aliases.begin(), site->aliases.end());
  }
  if (aliases.empty()) return;

  // Every value naming the dead storage goes with it: the allocation, the store
  // that bound it, the load that read it back and the drop that would have
  // released it. Nothing else can name it, because it never escaped and nothing
  // dereferenced it.
  std::vector<tir::Op> kept;
  kept.reserve(function.operations().size());
  for (tir::Op& op : function.operations()) {
    const bool names_dead =
        (op.result.is_valid() && aliases.count(op.result.id) != 0) ||
        (op.operand.is_valid() && aliases.count(op.operand.id) != 0) ||
        (op.lhs.is_valid() && aliases.count(op.lhs.id) != 0);
    if (names_dead) continue;
    kept.push_back(std::move(op));
  }
  function.operations() = std::move(kept);

  // The obligation described a release that no longer exists; leaving it behind
  // would make the function claim a destruction it does not perform.
  std::vector<tir::DestructionObligation>& obligations = function.obligations();
  obligations.erase(
      std::remove_if(obligations.begin(), obligations.end(),
                     [&](const tir::DestructionObligation& obligation) {
                       return obligation.owner.is_valid() &&
                              aliases.count(obligation.owner.id) != 0;
                     }),
      obligations.end());
}

// Why a site escaped, in the words the report uses. The analysis reason is a
// transport detail; the report states the fact a reader can check in the source.
static std::string_view escape_rationale(tir::EscapeReason reason) {
  switch (reason) {
    case tir::EscapeReason::Returned: return "escaping via return";
    case tir::EscapeReason::PassedToCallee: return "escaping via callee";
    case tir::EscapeReason::StoredOutsideActivation:
      return "escaping into foreign storage";
    case tir::EscapeReason::ContainedInEscapingObject:
      return "escaping inside an escaping object";
    case tir::EscapeReason::CyclicGraph:
      return "escaping through a reference cycle";
    case tir::EscapeReason::None: return "escaping";
  }
  return "escaping";
}

// One line per allocation site: where it is, what it allocates, the strategy
// selected for it and the fact that selected it. Everything here is already
// known to the analyses that just ran, so the report is an observation rather
// than a second opinion.
std::vector<std::string> cdmm_report(
    const tir::Function& function,
    const std::unordered_map<int, StackAllocationLayout>& stack_allocations,
    const HybridSelection& selection,
    bool promoted_callee,
    bool partially_promoted,
    const std::unordered_set<int>& escaping_call_results,
    const std::unordered_set<int>& forwarded_call_results,
    const StructFieldTable* struct_types) {
  std::vector<std::string> lines;
  // A caller with no allocation sites of its own still reports the strategy it
  // chose for a callee's storage, so an empty analysis is not an empty report.
  const tir::LifetimeAnalysis lifetimes =
      tir::LifetimeAnalysis::analyze(function, struct_types);

  // A variable is named where its pointer is stored, not where it is allocated.
  std::unordered_map<int, std::string> slot_names;
  for (const tir::Op& op : function.operations())
    if (op.kind == tir::OpKind::Alloca && op.result.is_valid() &&
        !op.slot_name.empty())
      slot_names.emplace(op.result.id, op.slot_name);
  std::unordered_map<int, std::string> binding_names;
  for (const tir::Op& op : function.operations()) {
    if (op.kind != tir::OpKind::Store) continue;
    if (!op.lhs.is_valid() || !op.operand.is_valid()) continue;
    const auto found = slot_names.find(op.lhs.id);
    if (found != slot_names.end()) binding_names.emplace(op.operand.id, found->second);
  }

  std::unordered_map<int, std::size_t> region_of_site;
  for (std::size_t index = 0; index < selection.regions.size(); ++index)
    for (int site : selection.regions[index].sites)
      region_of_site.emplace(site, index);

  for (const tir::Op& op : function.operations()) {
    if (!tir::is_allocation_site(op)) continue;
    const tir::AllocationSite* site = lifetimes.site(op.result.id);
    if (site == nullptr) continue;

    const auto binding = binding_names.find(op.result.id);
    const std::string name =
        binding != binding_names.end() ? binding->second : std::string("<temporary>");

    std::string_view strategy;
    std::string rationale;
    if (promoted_callee && site->reason == tir::EscapeReason::Returned) {
      // The site escapes its own function, so its home belongs to the call site:
      // the caller's arena when the caller passed one, the general allocator when
      // it passed null. Both branches exist in this body.
      strategy = "arena";
      rationale = partially_promoted
                      ? "returned to bounded and escaping callers, arena handle "
                        "checked at runtime"
                      : "returned to bounded callers, arena handle checked at "
                        "runtime";
    } else if (stack_allocations.count(op.result.id) != 0) {
      strategy = "stack";
      rationale = "bounded to function, promoted to the stack";
      if (site->loop_scoped) rationale += ", loop-scoped";
    } else if (const auto region = region_of_site.find(op.result.id);
               region != region_of_site.end()) {
      if (selection.regions[region->second].pooled) {
        strategy = "pool";
        rationale = "bounded to function, repeated layout";
        if (site->identical_sites > 0)
          rationale += " (" + std::to_string(site->identical_sites) + " sites)";
        if (site->loop_scoped) rationale += ", loop-scoped";
      } else {
        strategy = "arena";
        rationale = "bounded to function";
        if (site->loop_scoped) rationale += ", loop-scoped";
      }
    } else {
      strategy = "gc";
      // A payload whose own type can close a cycle of owning edges is storage no
      // deterministic release can describe: the collector is the strategy the
      // shape selects, so the report names the shape rather than the escape.
      rationale = site->cyclic_payload
                      ? std::string("cyclic object graph detected")
                      : std::string(escape_rationale(site->reason));
      if (site->in_loop) rationale += ", in loop";
    }

    std::string line = "cdmm: ";
    line += op.location.is_valid() ? op.location.to_string() : std::string("<unknown>");
    line += " '" + name + "' " + op.result.type.to_string();
    line += " -> " + std::string(strategy) + " (" + rationale + ")";
    lines.push_back(std::move(line));
  }

  // A call whose callee fills the result into this frame's arena. The strategy is
  // chosen here rather than in the callee, so the decision belongs in this
  // report, and the callee is named because it is where the storage is written.
  for (const auto& entry : selection.arena_call_regions) {
    const tir::Op* call = nullptr;
    for (const tir::Op& op : function.operations())
      if (op.kind == tir::OpKind::Call && op.result.is_valid() &&
          op.result.id == entry.first) {
        call = &op;
        break;
      }
    if (call == nullptr) continue;
    const auto binding = binding_names.find(entry.first);
    const std::string name =
        binding != binding_names.end() ? binding->second : std::string("<temporary>");
    std::string line = "cdmm: ";
    line += call->location.is_valid() ? call->location.to_string()
                                      : std::string("<unknown>");
    line += " '" + name + "' " + call->result.type.to_string();
    // The scope is where the arena is created and released, so it belongs in the
    // line: a loop-scoped site reclaims its storage once per iteration.
    const bool loop_scoped = selection.regions[entry.second].scope.loop;
    line += " -> arena (bounded to the ";
    line += loop_scoped ? "loop" : "caller's frame";
    line += ", filled by " + call->callee + ")";
    lines.push_back(std::move(line));
  }

  // A call site of a promoted callee that passes null. Lowering the storage to the
  // general allocator happens inside the callee, so the call site is the only
  // place this decision is visible: the callee's own lines describe both branches.
  for (const tir::Op& op : function.operations()) {
    if (op.kind != tir::OpKind::Call || !op.result.is_valid()) continue;
    const bool forwarded = forwarded_call_results.count(op.result.id) != 0;
    if (!forwarded && escaping_call_results.count(op.result.id) == 0) continue;
    const auto binding = binding_names.find(op.result.id);
    const std::string name =
        binding != binding_names.end() ? binding->second : std::string("<temporary>");
    std::string line = "cdmm: ";
    line += op.location.is_valid() ? op.location.to_string()
                                   : std::string("<unknown>");
    line += " '" + name + "' " + op.result.type.to_string();
    if (forwarded) {
      // The chain fills this activation's arena, so the site names the handle it
      // handed down rather than claiming the general allocator.
      line += " -> arena (forwarded this activation's handle to " + op.callee +
              ")";
    } else {
      line += " -> gc (escaping call site, null arena handle passed to " +
              op.callee + ")";
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

namespace {

// True when a pointer names storage an arena or a pool owns. The registry destroys
// the fields of such storage, so the aggregate walk must not run a second time.
// The walk goes through the pointer's history: a load names a slot, and the value
// the slot holds is whatever the last store put there.
bool traces_to_registry_storage(const tir::Function& function,
                               const tir::Value& value,
                               const std::unordered_set<int>& served, int depth) {
  if (!value.is_valid() || depth > 8) return false;
  if (served.count(value.id) != 0) return true;
  const tir::Op* origin = nullptr;
  for (auto op = function.operations().rbegin(); op != function.operations().rend();
       ++op)
    if (op->result.is_valid() && op->result.id == value.id) {
      origin = &*op;
      break;
    }
  if (origin == nullptr) return false;
  if (origin->kind == tir::OpKind::GetElementPtr)
    return traces_to_registry_storage(function, origin->lhs, served, depth + 1);
  if (origin->kind != tir::OpKind::Load) return false;
  // Every value the slot received is a candidate, not only the last one: a
  // cleanup nulls a slot after moving out of it, and the allocation it held
  // before that is still the storage the pointer names.
  for (auto op = function.operations().rbegin(); op != function.operations().rend();
       ++op)
    if (op->kind == tir::OpKind::Store && op->lhs.is_valid() &&
        op->lhs.id == origin->lhs.id && op->operand.is_valid() &&
        traces_to_registry_storage(function, op->operand, served, depth + 1))
      return true;
  return false;
}

// Expands one aggregate field walk into a release per managed field at the
// destination's address, and drops the walk where a registry owns the storage.
void expand_aggregate_field_teardowns(tir::Function& function,
                                      const std::unordered_set<int>& served) {
  std::vector<tir::Op> expanded;
  expanded.reserve(function.operations().size());
  for (tir::Op& op : function.operations()) {
    if (op.kind != tir::OpKind::AggregateFieldDestroy || !op.lhs.is_valid()) {
      expanded.push_back(std::move(op));
      continue;
    }
    const tir::Value base = op.lhs;
    if (traces_to_registry_storage(function, base, served, 0)) continue;
    // A pointer destination names the aggregate one level down; an aggregate
    // binding is the aggregate itself.
    const Type& aggregate =
        base.type.is_pointer() ? base.type.element_type() : base.type;
    const std::vector<std::pair<std::string, Type>> declared =
        aggregate.struct_fields();
    for (std::size_t index = 0;
         index < op.release_fields.size() && index < op.release_callees.size();
         ++index) {
      const int field = op.release_fields[index];
      if (field < 0 || static_cast<std::size_t>(field) >= declared.size()) continue;
      const Type& field_type = declared[static_cast<std::size_t>(field)].second;
      tir::Op address;
      address.kind = tir::OpKind::GetElementPtr;
      address.location = op.location;
      address.lhs = base;
      address.field_index = field;
      address.result = function.allocate_value(field_type);
      const tir::Value slot = address.result;
      expanded.push_back(std::move(address));
      tir::Op load;
      load.kind = tir::OpKind::Load;
      load.location = op.location;
      load.lhs = slot;
      load.result = function.allocate_value(field_type);
      const tir::Value stored = load.result;
      expanded.push_back(std::move(load));
      tir::Op release;
      release.kind = tir::OpKind::Call;
      release.location = op.location;
      release.callee = op.release_callees[index];
      release.arguments.push_back(stored);
      expanded.push_back(std::move(release));
    }
  }
  function.operations() = std::move(expanded);
}

}  // namespace

mlir::OwningOpRef<mlir::ModuleOp> translate_tir_to_mlir(
    const tir::Module& tir_module,
    mlir::MLIRContext& context,
    DiagnosticEngine& diagnostics,
    bool fast_math,
    RuntimeConfiguration configuration,
    bool inspect_cdmm,
    const StructFieldTable* struct_types) {

  mlir::OpBuilder builder(&context);
  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::ModuleOp::create(builder.getUnknownLoc());
  builder.setInsertionPointToStart(module->getBody());
  const BorrowedEscapeSummary borrowed_summary =
      summarize_borrowed_parameters(tir_module);
  // Call sites whose returned storage stays in the caller's frame, and the
  // callees that can be served from it. The analysis is module-wide because a
  // callee has one signature for every caller, so its answer cannot depend on the
  // call site it is read at.
  const tir::CallSiteArenaPlan arena_plan =
      tir::analyze_call_site_arenas(tir_module);
  const CollectionSummary collections = summarize_collections(tir_module);
  const std::unordered_set<std::string> mutator_regions =
      summarize_mutator_regions(tir_module);
  // The managed fields of every aggregate a literal packed with a release. An
  // arena allocation of such a payload registers one teardown per field, because
  // the chunk's bulk release cannot visit them: the release frees storage, and a
  // field owns storage of its own.
  std::unordered_map<std::string, PayloadTeardowns> payload_field_releases;
  for (const auto& tir_fn : tir_module.functions())
    for (const tir::Op& op : tir_fn->operations())
      if (op.kind == tir::OpKind::AggregatePack && !op.slot_name.empty()) {
        PayloadTeardowns& entry = payload_field_releases[op.slot_name];
        if (entry.layout.struct_fields().empty()) entry.layout = op.result.type;
        for (std::size_t i = 0;
             i < op.release_fields.size() && i < op.release_callees.size(); ++i) {
          // Two literals of the same payload describe the same field twice; the
          // field is one slot, so it is registered once.
          bool seen = false;
          for (const std::pair<int, std::string>& known : entry.fields)
            if (known.first == op.release_fields[i]) seen = true;
          if (seen) continue;
          entry.fields.emplace_back(op.release_fields[i],
                                    op.release_callees[i]);
        }
      }
  // One table for the whole module: literal globals live at module scope, so the
  // names must not restart for every function the lowering visits.
  StringLiteralTable string_literals;
  // One report for the module: the audit pass prints it in the order the sites
  // were translated, which is source order within a function.
  std::vector<std::string> cdmm_lines;

  for (const auto& tir_fn : tir_module.functions()) {
    mlir::Type ret_type;
    if (tir_fn->return_type().is_void()) {
      ret_type = {};
    } else {
      ret_type = tir_type_to_mlir(tir_fn->return_type(), &context, builder);
      if (!ret_type) {
        diagnostics.error("NRQ4005", SourceLocation{},
                          "internal: unsupported function return type in TIR translation");
        return nullptr;
      }
    }

    std::vector<mlir::Type> param_types;
    param_types.reserve(tir_fn->parameters().size());
    for (const auto& param : tir_fn->parameters()) {
      mlir::Type pty = tir_type_to_mlir(param.type, &context, builder);
      if (!pty) {
        diagnostics.error("NRQ4005", SourceLocation{},
                          "internal: unsupported parameter type in TIR translation");
        return nullptr;
      }
      param_types.push_back(pty);
    }
    // A promoted callee takes one trailing argument: the arena its caller created
    // for the storage it returns. It is part of the signature rather than a side
    // channel because the callee has to read it as a value.
    const bool promoted_callee = arena_plan.promoted(tir_fn->name());
    if (promoted_callee) param_types.push_back(llvm_ptr_type(&context));

    mlir::FunctionType fn_type;
    if (ret_type) {
      fn_type = mlir::FunctionType::get(&context, param_types, {ret_type});
    } else {
      fn_type = mlir::FunctionType::get(&context, param_types, {});
    }

    // An entry point that takes `string[] args` cannot be the C `main`, so it
    // is emitted under an internal name and reached through the wrapper below.
    const bool is_entry_with_args =
        tir_fn->name() == "main" && !tir_fn->parameters().empty();
    const std::string symbol = is_entry_with_args ? "nrq_entry_args" : tir_fn->name();

    auto func_op = builder.create<mlir::func::FuncOp>(
        builder.getUnknownLoc(), symbol, fn_type);
    func_op->setAttr("passthrough",
                     builder.getArrayAttr({builder.getStringAttr("nounwind")}));
    if (tir_fn->return_ownership() != ValueOwnership::Trivial)
      func_op->setAttr("nirqoba.return_ownership",
                       builder.getStringAttr(
                           ownership_spelling(tir_fn->return_ownership())));
    if (!tir_fn->return_resource_domain().empty())
      func_op->setAttr("nirqoba.return_resource_domain",
                       builder.getStringAttr(tir_fn->return_resource_domain()));
    // Phase 1 ownership metadata survives into MLIR as attributes. Consumers
    // that only understand the standard dialects can ignore them, while the
    // verifier and diagnostic passes can still see the relationships.
    if (!tir_fn->lifetimes().empty()) {
      std::string summary;
      for (const tir::LifetimeRecord& lifetime : tir_fn->lifetimes()) {
        if (!summary.empty()) summary += ";";
        switch (lifetime.kind) {
          case tir::LifetimeKind::Lexical: summary += "lexical"; break;
          case tir::LifetimeKind::Parameter: summary += "parameter"; break;
          case tir::LifetimeKind::Return: summary += "return"; break;
          case tir::LifetimeKind::None: summary += "none"; break;
        }
        if (!lifetime.source_parameter.empty())
          summary += ":" + lifetime.source_parameter;
        summary += lifetime.mutable_access ? ":mutable" : ":immutable";
      }
      func_op->setAttr("nirqoba.lifetime_relations",
                       builder.getStringAttr(summary));
    }
    if (!tir_fn->obligations().empty())
      func_op->setAttr(
          "nirqoba.destruction_obligations",
          builder.getI32IntegerAttr(
              static_cast<int32_t>(tir_fn->obligations().size())));
    if (!tir_fn->return_borrow_parameter().empty())
      func_op->setAttr("nirqoba.return_borrow_parameter",
                       builder.getStringAttr(tir_fn->return_borrow_parameter()));
    if (tir_fn->return_type().is_pointer() &&
        tir_fn->return_ownership() == ValueOwnership::Owned) {
      func_op.setResultAttr(0, mlir::LLVM::LLVMDialect::getNoAliasAttrName(),
                            builder.getUnitAttr());
    }
    if (!tir_fn->parameters().empty()) {
      llvm::SmallVector<mlir::Attribute> argument_attrs;
      argument_attrs.reserve(tir_fn->parameters().size());
      for (std::size_t index = 0; index < tir_fn->parameters().size(); ++index) {
        const auto& param = tir_fn->parameters()[index];
        mlir::NamedAttrList attrs;
        if (param.ownership != ValueOwnership::Trivial)
          attrs.set("nirqoba.ownership",
                    builder.getStringAttr(ownership_spelling(param.ownership)));
        if (!param.resource_domain.empty())
          attrs.set("nirqoba.resource_domain",
                    builder.getStringAttr(param.resource_domain));
        if (param.type.is_pointer()) {
          // `noalias` follows from the ownership class: a mutable borrow or an
          // owned pointer is the only access path the language allows to that
          // memory for the duration of the call.
          //
          // `nocapture` and `readonly` are promises about the instructions in
          // this body, so they are only attached when the analyses prove them.
          // Two independent measurements had been missing here: the borrowed
          // escape fixpoint was computed for every function and never consulted
          // for attributes, and nothing checked for a write through the
          // parameter at all.
          const bool non_escaping =
              borrowed_parameter_does_not_escape(*tir_fn, index, borrowed_summary);
          if (param.ownership == ValueOwnership::MutableBorrowed ||
              param.ownership == ValueOwnership::Owned) {
            attrs.set(mlir::LLVM::LLVMDialect::getNoAliasAttrName(),
                      builder.getUnitAttr());
            if (param.ownership == ValueOwnership::MutableBorrowed && non_escaping) {
              attrs.set(mlir::LLVM::LLVMDialect::getNoCaptureAttrName(),
                        builder.getUnitAttr());
            }
          } else if (param.ownership == ValueOwnership::Borrowed) {
            if (parameter_is_never_stored_through(*tir_fn, index))
              attrs.set(mlir::LLVM::LLVMDialect::getReadonlyAttrName(),
                        builder.getUnitAttr());
            if (non_escaping) {
              attrs.set(mlir::LLVM::LLVMDialect::getNoCaptureAttrName(),
                        builder.getUnitAttr());
            }
          }
        }
        argument_attrs.push_back(attrs.getDictionary(&context));
      }
      // The trailing arena handle is not a source parameter, so it has no facts
      // to state, but the array still has to cover every argument of the type.
      if (promoted_callee)
        argument_attrs.push_back(mlir::DictionaryAttr::get(&context));
      func_op.setArgAttrsAttr(builder.getArrayAttr(argument_attrs));
    }

    // Destructive cleanup is represented once, as a Drop, and expanded here.
    // The copy keeps the verified module unchanged for later diagnostics.
    tir::Function lowered_fn = *tir_fn;
    elide_dead_allocations(lowered_fn);
    std::vector<tir::BoundedCallResult> promoted_calls;
    if (const auto sites = arena_plan.call_sites.find(tir_fn->name());
        sites != arena_plan.call_sites.end())
      promoted_calls = sites->second;
    std::unordered_set<int> arena_param_sites;
    std::unordered_set<int> arena_param_aliases;
    if (promoted_callee) {
      const tir::LifetimeAnalysis callee_lifetimes =
          tir::LifetimeAnalysis::analyze(lowered_fn);
      for (const auto& entry : callee_lifetimes.sites())
        if (entry.second.reason == tir::EscapeReason::Returned) {
          arena_param_sites.insert(entry.first);
          arena_param_aliases.insert(entry.second.aliases.begin(),
                                     entry.second.aliases.end());
        }
    }
    // A promoted callee with no escaping call site always receives a handle, so
    // its activation lives inside the caller's arena: it opens no chunk of its
    // own, and its allocation has no null arm to fall back on.
    const bool serves_from_caller =
        promoted_callee && !arena_plan.partial(tir_fn->name());
    HybridSelection selection =
        select_hybrid_strategies(lowered_fn, promoted_calls, serves_from_caller);
    // Phase 10e: a payload whose type can close a cycle through a reference
    // count has no deterministic release to run, so a site the cheaper strategies
    // leave unclaimed is collector storage. The answer is read from the struct
    // catalog the checker published, so Sema's gate, the report and this lowering
    // agree about which sites the collector owns. A site a region, a pool or the
    // stack claims is decided inside the allocation lowering, which asks those
    // strategies first; suppressing its Drop here is what already happens there.
    std::unordered_set<int> collector_sites;
    std::unordered_set<int> collector_owned;
    if (struct_types) {
      const tir::LifetimeAnalysis collector_lifetimes =
          tir::LifetimeAnalysis::analyze(lowered_fn, struct_types);
      for (const auto& entry : collector_lifetimes.sites()) {
        if (!entry.second.cyclic_payload) continue;
        collector_sites.insert(entry.first);
        collector_owned.insert(entry.first);
        collector_owned.insert(entry.second.aliases.begin(),
                               entry.second.aliases.end());
      }
    }
    // Both region kinds own the storage, so neither may also see a per-binding
    // release: the Drop keeps its record and lowers into the region's own path.
    // A collected site is the same case for a stronger reason - the collector
    // reclaims it, so a deterministic free would return its block while the
    // collector still traces it.
    std::unordered_set<int> suppressed = selection.region_aliases;
    suppressed.insert(selection.pool_aliases.begin(), selection.pool_aliases.end());
    suppressed.insert(collector_owned.begin(), collector_owned.end());
    lowered_fn.expand_drop_operations_to_calls(suppressed);
    lowered_fn.remove_dead_slot_clears_before_return();
    // The stack analysis runs on the expanded copy, exactly as it did before
    // automated selection existed: a release is a call there, and its answer must
    // not change because a site was also classified.
    const auto stack_allocations =
        analyze_stack_allocations(lowered_fn, borrowed_summary);
    // The stack is cheaper than either region, so its answer wins. A site it
    // claims leaves the selection here, which is why a function whose allocations
    // all fit its frame emits no region calls at all.
    drop_stack_claimed(selection, stack_allocations);
    // The aggregate walks run last, once every strategy has had its say: a site
    // the stack claimed has left the selection, so a walk into its storage is
    // what frees the fields - exactly once, at the end of the frame. A walk into
    // storage an arena or a pool owns is dropped here instead, because that
    // strategy's registry destroys the fields itself.
    {
      // Only the regions that survived the stack analysis: a site the stack claimed
      // has left them, and its frame runs the walk itself. Every surviving region
      // names its storage by its sites and by the values that alias them.
      std::unordered_set<int> served;
      for (const RegionSelection& region : selection.regions) {
        served.insert(region.sites.begin(), region.sites.end());
        served.insert(region.aliases.begin(), region.aliases.end());
      }
      served.insert(selection.activation_sites.begin(),
                    selection.activation_sites.end());
      served.insert(arena_param_sites.begin(), arena_param_sites.end());
      served.insert(arena_param_aliases.begin(), arena_param_aliases.end());
      expand_aggregate_field_teardowns(lowered_fn, served);
    }
    std::unordered_set<int> escaping_call_results;
    if (const auto sites = arena_plan.escaping_call_results.find(tir_fn->name());
        sites != arena_plan.escaping_call_results.end())
      escaping_call_results = sites->second;
    std::unordered_set<int> forwarded_call_results;
    if (const auto sites = arena_plan.forwarded_call_results.find(tir_fn->name());
        sites != arena_plan.forwarded_call_results.end())
      forwarded_call_results = sites->second;
    if (inspect_cdmm)
      for (std::string& line : cdmm_report(lowered_fn, stack_allocations, selection,
                                           promoted_callee,
                                           arena_plan.partial(tir_fn->name()),
                                           escaping_call_results,
                                           forwarded_call_results,
                                           struct_types))
        cdmm_lines.push_back(std::move(line));
    FunctionTranslator translator(
        *module, lowered_fn, borrowed_summary, collections, func_op, builder,
        &context, diagnostics, fast_math, configuration, string_literals,
        symbol == "main", mutator_regions.count(tir_fn->name()) != 0,
        stack_allocations, std::move(selection.regions),
        promoted_callee ? static_cast<int>(param_types.size()) - 1 : -1,
        std::move(arena_param_sites), std::move(arena_param_aliases),
        std::move(selection.arena_call_regions),
        arena_plan.promoted_callees, std::move(forwarded_call_results),
        std::move(selection.activation_sites), std::move(collector_sites),
        serves_from_caller, payload_field_releases, struct_types);
    if (!translator.translate()) return nullptr;

    builder.setInsertionPointAfter(func_op);

    if (is_entry_with_args &&
        !emit_entry_wrapper(*module, builder, *tir_fn, symbol,
                            mutator_regions.count(tir_fn->name()) != 0,
                            configuration, diagnostics)) {
      return nullptr;
    }
  }

  if (inspect_cdmm) {
    std::vector<mlir::Attribute> report;
    report.push_back(builder.getStringAttr(
        "cdmm: " + std::to_string(cdmm_lines.size()) +
        " allocation site(s) classified"));
    for (const std::string& line : cdmm_lines)
      report.push_back(builder.getStringAttr(line));
    module->getOperation()->setAttr("nirqoba.cdmm_report",
                                    builder.getArrayAttr(report));
  }
  return module;
}

}  // namespace nirq
