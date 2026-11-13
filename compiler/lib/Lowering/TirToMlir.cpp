// TirToMlir.cpp — translates a TIR module into MLIR.

#include "nirqoba/Lowering/TirToMlir.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
  // A dynamic array is a pointer to a runtime-owned descriptor; the runtime
  // owns the element storage, so the compiler only ever handles the handle.
  if (type.is_dynamic_array()) return mlir::LLVM::LLVMPointerType::get(ctx);
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
      "file.exists",     "file.close",       "socket.send",
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
          if ((op.callee == "memory.deallocate" ||
               op.callee == "nrq_memory_deallocate") &&
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
        (op.callee == "memory.allocate" || op.callee == "memory.reallocate") &&
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
      "nrq_memory_allocate", "memory.deallocate", "nrq_memory_deallocate",
      "memory.reallocate", "memory.shared_allocate", "memory.shared_retain",
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
      "aggregate.owned_drop", "file.open", "file.close", "file.write",
      "file.read", "file.read_text", "file.write_text", "file.append_text",
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
                                       const CollectionSummary& summary) {
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
    if (op.kind == tir::OpKind::Call && op.callee == "array.allocate" &&
        op.result.is_valid() && op.result.type.is_managed_pointer())
      plan.has_rooted_allocation = true;
    // Stack-placed owned storage is registered inside the activation frame,
    // even when the function has no managed local of its own.
    if (op.kind == tir::OpKind::Call && op.result.is_valid() &&
        op.result.type.is_pointer() &&
        (op.callee == "memory.allocate" || op.callee == "memory.reallocate") &&
        collect_has_managed_reference(op.result.type.element_type()))
      has_owned_storage_roots = true;
  }
  for (const tir::Function::Parameter& parameter : function.parameters())
    if (collect_has_managed_reference(parameter.type)) has_managed_parameters = true;

  if (!has_managed_slots && !has_managed_parameters && !has_managed_call_results) {
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
  switch (ownership) {
    case ValueOwnership::Owned: return "owned";
    case ValueOwnership::Borrowed: return "borrowed";
    case ValueOwnership::MutableBorrowed: return "mutable_borrowed";
    case ValueOwnership::Shared: return "shared";
    case ValueOwnership::GcManaged: return "gc_managed";
    case ValueOwnership::Raw: return "raw";
    case ValueOwnership::Trivial: return "trivial";
  }
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
                     bool needs_mutator_region)
      : module_(module), tir_fn_(tir_fn), func_op_(func_op),
        builder_(builder), ctx_(ctx), diag_(diag), fast_math_(fast_math),
        configuration_(configuration),
        string_literals_(string_literals),
        emit_abi_reference_(emit_abi_reference),
        stack_allocations_(
            analyze_stack_allocations(tir_fn, borrowed_summary)),
        gc_roots_(analyze_gc_root_plan(tir_fn, collections)),
        uses_gc_mutator_(needs_mutator_region),
        needs_gc_root_frame_(uses_gc_mutator_ && gc_roots_.needs_root_frame),
        promoted_slots_(analyze_promoted_slots(tir_fn)) {
    const std::vector<tir::Op>& operations = tir_fn_.operations();
    for (std::size_t index = 0; index < operations.size(); ++index)
      if (operations[index].kind == tir::OpKind::Label)
        label_positions_.emplace(operations[index].label_id, index);
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
    const std::vector<tir::Op>& operations = tir_fn_.operations();
    for (std::size_t index = 0; index < operations.size(); ++index) {
      current_operation_index_ = index;
      const tir::Op& op = operations[index];
      // Forwarded values are only valid until the next call (which may publish,
      // move or collect what the slot holds) or the next control-flow join (whose
      // incoming edges this single-block map does not describe).
      if (op.kind == tir::OpKind::Call || op.kind == tir::OpKind::Label ||
          op.kind == tir::OpKind::Branch ||
          op.kind == tir::OpKind::BranchCond) {
        forwarded_root_values_.clear();
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
    if (!bounds_fail_block_) return;
    func_op_.getBody().push_back(bounds_fail_block_);
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

  // Root registration is likewise a per-activation action: the same slot must
  // be registered once, not once per loop iteration. Registrations are queued
  // until all managed slots are known, then emitted after the frame begins and
  // before any body operation can trigger collection.
  void register_gc_root(mlir::Value slot, const Type& value_type,
                        mlir::Location loc,
                        mlir::Value initial_value = {}) {
    std::vector<uint64_t> offsets;
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
    if (pending_gc_roots_.empty() || entry_block_ == nullptr) return;
    if (!gc_root_frame_marker_) return;
    mlir::OpBuilder::InsertionGuard guard(builder_);
    mlir::Operation* frame_begin = gc_root_frame_marker_.getDefiningOp();
    if (!frame_begin) return;
    builder_.setInsertionPointAfter(frame_begin);
    // A slot must never hold a stale pointer when it becomes visible to a
    // collection, so every slot is cleared before any of them is published.
    for (const PendingGcRoot& root : pending_gc_roots_) {
      mlir::Type value_type = tir_type_to_mlir(root.type, ctx_, builder_);
      if (!value_type) continue;
      mlir::Value zero = builder_.create<mlir::LLVM::ZeroOp>(
          root.loc, value_type).getResult();
      builder_.create<mlir::LLVM::StoreOp>(root.loc, zero, root.slot);
    }
    // One call publishes the activation's whole root set: the descriptor is a
    // read-only table describing the layouts, the argument array carries the
    // slot addresses a single activation computes. A lone root keeps the
    // direct call, where the batched form's array would cost more than it saves.
    if (!emit_gc_frame_root_registration(pending_gc_roots_)) {
      for (const PendingGcRoot& root : pending_gc_roots_)
        emit_gc_root_registration(root.slot, root.type, root.loc);
    }
    for (const PendingGcRoot& root : pending_gc_roots_) {
      if (root.initial_value) {
        mlir::Type value_type = tir_type_to_mlir(root.type, ctx_, builder_);
        if (!value_type) continue;
        // A parameter that is a single managed reference is published as that
        // reference: the barrier takes it directly, so the activation needs no
        // staging slot and no byte copy to initialise its root.
        if (root.type.is_managed_pointer()) {
          auto store_fn = ensure_runtime_fn(
              module_, builder_, "nrq_gc_root_store",
              mlir::LLVM::LLVMVoidType::get(ctx_),
              {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(
              root.loc, store_fn,
              mlir::ValueRange{root.slot, root.initial_value});
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
    }
    pending_gc_roots_.clear();
  }

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
    const bool direct_pointer_root = keep_root && type.is_managed_pointer() &&
                                    offsets.size() == 1 && offsets[0] == 0;
    mlir::Value slot;
    if (direct_pointer_root) {
      slot = alloca_at_entry(value_type, loc);
      register_gc_root(slot, type, loc);
      auto store_fn = ensure_runtime_fn(
          module_, builder_, "nrq_gc_root_store",
          mlir::LLVM::LLVMVoidType::get(ctx_),
          {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_)});
      builder_.create<mlir::LLVM::CallOp>(loc, store_fn,
                                          mlir::ValueRange{slot, value});
    } else {
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
    auto unpin = ensure_runtime_fn(
        module_, builder_, "nrq_gc_unpin",
        mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
    if (direct_pointer_root) {
      // The root holds exactly the value that was just published into it, so the
      // release needs no reload of the slot it was stored into.
      builder_.create<mlir::LLVM::CallOp>(loc, unpin, mlir::ValueRange{value});
      return;
    }
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
        if (current != nullptr && (current->empty() || !current->back().hasTrait<mlir::OpTrait::IsTerminator>())) {
          builder_.create<mlir::LLVM::BrOp>(loc, target_block);
        }
        builder_.setInsertionPointToStart(target_block);
        return true;
      }

      case tir::OpKind::Branch: {
        mlir::Block* target_block = get_or_create_block(op.label_id);
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
        mlir::Value slot = alloca_at_entry(elem_type, loc);
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
            gc_roots_.unobserved_addresses.count(op.lhs.id) != 0;
        if (op.operand.type.is_managed_pointer()) {
          if (unobserved) {
            apply_access_alignment(
                builder_.create<mlir::LLVM::StoreOp>(loc, val, ptr),
                op.lhs, op.operand.type);
            forwarded_root_values_.erase(op.lhs.id);
            return true;
          }
          auto store_fn = ensure_runtime_fn(module_, builder_, "nrq_gc_root_store",
                                            mlir::LLVM::LLVMVoidType::get(ctx_),
                                            {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, store_fn,
                                              mlir::ValueRange{ptr, val});
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
          register_gc_root(root_slot, op.result.type, loc);
          auto fn = ensure_runtime_fn(module_, builder_,
                                      "nrq_gc_allocate_array_typed_rooted",
                                      llvm_ptr_type(ctx_),
                                      {llvm_ptr_type(ctx_), builder_.getI64Type(),
                                       builder_.getI64Type(),
                                       llvm_ptr_type(ctx_), builder_.getI64Type()});
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
            result = builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{root_slot, count, size, offsets_pointer,
                                          ref_count}).getResult();
          } else {
            mlir::Value pairs_slot = reference_layout_descriptor(column_pairs, loc);
            if (!pairs_slot) {
              diag_.error("NRQ4005", op.location,
                          "internal: no descriptor for the reference columns");
              return false;
            }
            auto soa_fn = ensure_runtime_fn(
                module_, builder_, "nrq_gc_allocate_soa_rooted",
                llvm_ptr_type(ctx_),
                {llvm_ptr_type(ctx_), builder_.getI64Type(), builder_.getI64Type(),
                 llvm_ptr_type(ctx_), builder_.getI64Type()});
            mlir::Value pair_count = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI64Type(),
                builder_.getI64IntegerAttr(
                    static_cast<int64_t>(column_pairs.size()))).getResult();
            result = builder_.create<mlir::LLVM::CallOp>(
                loc, soa_fn, mlir::ValueRange{root_slot, count, size, pairs_slot,
                                              pair_count}).getResult();
          }
          set(op.result, result);
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
          auto unregister = ensure_runtime_fn(
              module_, builder_, "nrq_gc_root_unregister",
              mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(
              loc, unregister, mlir::ValueRange{data});
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

        if (op.callee == "aggregate.owned_drop" && args.size() == 1) {
          std::function<void(const Type&, mlir::Value, std::vector<int64_t>&)> drop_fields;
          drop_fields = [&](const Type& type, mlir::Value aggregate,
                            std::vector<int64_t>& path) {
            if (type.is_shared_pointer()) {
              mlir::Value data = builder_.create<mlir::LLVM::ExtractValueOp>(
                  loc, aggregate, builder_.getDenseI64ArrayAttr(path)).getResult();
              auto release = ensure_runtime_fn(
                  module_, builder_, "nrq_memory_shared_release",
                  builder_.getI32Type(), {llvm_ptr_type(ctx_)});
              builder_.create<mlir::LLVM::CallOp>(loc, release,
                                                   mlir::ValueRange{data});
              return;
            }
            if (type.is_owned_pointer()) {
              mlir::Value data = builder_.create<mlir::LLVM::ExtractValueOp>(
                  loc, aggregate, builder_.getDenseI64ArrayAttr(path)).getResult();
              if (type.is_resource_pointer()) {
                // A domain handle is not collector storage: release it with its
                // runtime destructor. Every one of these accepts null, which is
                // what an uninitialized or moved-out field holds.
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
                  builder_.create<mlir::LLVM::CallOp>(
                      loc, release, mlir::ValueRange{data});
                  return;
                }
              }
              auto unregister = ensure_runtime_fn(
                  module_, builder_, "nrq_gc_root_unregister",
                  mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
              builder_.create<mlir::LLVM::CallOp>(loc, unregister,
                                                   mlir::ValueRange{data});
              auto release = ensure_runtime_fn(
                  module_, builder_, "nrq_memory_deallocate_checked",
                  mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
              builder_.create<mlir::LLVM::CallOp>(loc, release,
                                                   mlir::ValueRange{data});
              return;
            }
            if (type.is_owned_array()) {
              mlir::Value descriptor = builder_.create<mlir::LLVM::ExtractValueOp>(
                  loc, aggregate, builder_.getDenseI64ArrayAttr(path)).getResult();
              mlir::Value data = builder_.create<mlir::LLVM::ExtractValueOp>(
                  loc, descriptor, builder_.getDenseI64ArrayAttr({0})).getResult();
              auto unregister = ensure_runtime_fn(
                  module_, builder_, "nrq_gc_root_unregister",
                  mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
              builder_.create<mlir::LLVM::CallOp>(
                  loc, unregister, mlir::ValueRange{data});
              auto release = ensure_runtime_fn(
                  module_, builder_, "nrq_memory_deallocate_checked",
                  mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
              builder_.create<mlir::LLVM::CallOp>(loc, release,
                                                   mlir::ValueRange{data});
              return;
            }
            auto visit = [&](const Type& field_type, int64_t index) {
              path.push_back(index);
              if (field_type.is_owned_array() || field_type.is_owned_pointer() ||
                  field_type.is_shared_pointer() || field_type.is_array() ||
                  field_type.is_struct() || field_type.is_tuple())
                drop_fields(field_type, aggregate, path);
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
          };
          std::vector<int64_t> path;
          drop_fields(op.arguments[0].type, args[0], path);
          return true;
        }

        // Copying an aggregate retains every shared pointer field it carries.
        if (op.callee == "aggregate.shared_retain" && args.size() == 1) {
          std::function<void(const Type&, mlir::Value, std::vector<int64_t>&)> retain_fields;
          retain_fields = [&](const Type& type, mlir::Value aggregate,
                              std::vector<int64_t>& path) {
            if (type.is_shared_pointer()) {
              mlir::Value data = builder_.create<mlir::LLVM::ExtractValueOp>(
                  loc, aggregate, builder_.getDenseI64ArrayAttr(path)).getResult();
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
                retain_fields(field_type, aggregate, path);
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
          };
          std::vector<int64_t> path;
          retain_fields(op.arguments[0].type, args[0], path);
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
          mlir::Value root_slot = alloca_at_entry(
              llvm_ptr_type(ctx_), loc);
          if (!root_slot) {
            diag_.error("NRQ4005", op.location,
                        "internal: no entry block for a managed allocation root");
            return false;
          }
          register_gc_root(root_slot, op.result.type, loc);
          auto fn = ensure_runtime_fn(module_, builder_,
                                     "nrq_gc_allocate_array_typed_rooted",
                                     llvm_ptr_type(ctx_),
                                     {llvm_ptr_type(ctx_), builder_.getI64Type(),
                                      builder_.getI64Type(),
                                      llvm_ptr_type(ctx_), builder_.getI64Type()});
          mlir::Value size = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(element_size)).getResult();
          mlir::Value ref_count = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(static_cast<int64_t>(reference_offsets.size()))).getResult();
          mlir::Value result = builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{root_slot, count, size, offsets_pointer,
                                        ref_count}).getResult();
          set(op.result, result);
          if (is_development(configuration_) && op.location.is_valid()) {
            const uint64_t site = gc_site_id(op.location);
            emit_gc_site_declaration(site, op.location, loc);
            emit_gc_allocation_tag(result, site, loc);
          }
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
            // Publish its typed root inside this activation's root frame so the
            // frame removes it when the function exits.
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
          mlir::Value alignment = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(type_alignment(op.result.type.element_type()))).getResult();
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
            result = builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{bytes, alignment}).getResult();
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
              {"memory.arena_allocator", 2}, {"memory.region", 2},
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
        if (op.callee == "memory.deallocate") {
          if (!op.arguments.empty() &&
              stack_allocations_.find(op.arguments[0].id) !=
                  stack_allocations_.end()) {
            return true;
          }
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_deallocate_checked",
                                     mlir::LLVM::LLVMVoidType::get(ctx_),
                                     {llvm_ptr_type(ctx_)});
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
        if (op.callee == "nrq_memory_allocate" || op.callee == "memory.allocate") {
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
        if (op.callee == "nrq_memory_deallocate") {
          if (!op.arguments.empty() &&
              stack_allocations_.find(op.arguments[0].id) !=
                  stack_allocations_.end()) {
            return true;
          }
          if (args.size() >= 3) {
            mlir::Value pointer = args[0];
            if (!mlir::isa<mlir::LLVM::LLVMPointerType>(pointer.getType())) {
              pointer = builder_.create<mlir::LLVM::IntToPtrOp>(
                  loc, llvm_ptr_type(ctx_), pointer).getResult();
            }
            auto coerce_i64 = [&](mlir::Value val) -> mlir::Value {
              if (val.getType() == builder_.getI64Type()) return val;
              const unsigned bits = val.getType().getIntOrFloatBitWidth();
              return bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), val).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), val).getResult();
            };
            mlir::Value bytes = coerce_i64(args[1]);
            mlir::Value align = coerce_i64(args[2]);
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_deallocate",
                                       mlir::LLVM::LLVMVoidType::get(ctx_),
                                       {llvm_ptr_type(ctx_), builder_.getI64Type(),
                                        builder_.getI64Type()});
            builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{pointer, bytes, align});
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
        if (op.callee == "nrq_gc_init" || op.callee == "nrq_gc_enable" ||
            op.callee == "gc.enable" || op.callee == "nrq_gc_shutdown" ||
            op.callee == "nrq_gc_disable" || op.callee == "gc.disable" ||
            op.callee == "nrq_gc_collect" || op.callee == "gc.collect") {
          const char* runtime_sym =
              (op.callee == "nrq_gc_init" || op.callee == "nrq_gc_enable" ||
               op.callee == "gc.enable")
                  ? "nrq_gc_enable"
                  : (op.callee == "nrq_gc_shutdown" || op.callee == "nrq_gc_disable" ||
                     op.callee == "gc.disable")
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
        // `element_count` sits at offset 32 of the object header and the payload
        // starts at 48, so the count is the word at payload - 16. It is the same
        // word the collector sizes the payload with, so it cannot disagree with the
        // block that was actually allocated.
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
        } else if (op.operand.type.is_dynamic_array()) {
          runtime_sym = "nrq_args_length";
        } else if (op.operand.type.is_owned_array() ||
                   op.operand.type.is_array_view()) {
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

        if (op.lhs.type.is_owned_array() || op.lhs.type.is_array_view()) {
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
        if (!op.lhs.type.is_dynamic_array()) {
          diag_.error("NRQ4005", op.location,
                      "internal: dynamic indexing of non-dynamic array type '" +
                          op.lhs.type.to_string() + "'");
          return false;
        }

        // The runtime ABI takes a 64-bit index.
        mlir::Type i64 = builder_.getI64Type();
        if (index.getType() != i64) {
          if (!mlir::isa<mlir::IntegerType>(index.getType())) {
            diag_.error("NRQ4005", op.location,
                        "internal: dynamic array index is not an integer");
            return false;
          }
          const unsigned bits = index.getType().getIntOrFloatBitWidth();
          if (bits < 64) {
            index = op.operand.type.is_signed_integer()
                        ? builder_.create<mlir::LLVM::SExtOp>(loc, i64, index).getResult()
                        : builder_.create<mlir::LLVM::ZExtOp>(loc, i64, index).getResult();
          } else {
            index = builder_.create<mlir::LLVM::TruncOp>(loc, i64, index).getResult();
          }
        }

        auto fn = ensure_runtime_fn(module_, builder_, "nrq_args_get",
                                    llvm_ptr_type(ctx_),
                                    {llvm_ptr_type(ctx_), i64});
        mlir::Value element = builder_.create<mlir::LLVM::CallOp>(
            loc, fn, mlir::ValueRange{array, index}).getResult();
        set(op.result, element);
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
  std::unordered_map<int, uint64_t> pointer_alignment_;
  GcRootPlan gc_roots_;
  bool uses_gc_mutator_ = false;
  bool needs_gc_root_frame_ = false;
  std::unordered_map<int, std::size_t> label_positions_;
  mlir::Block* entry_block_;
  std::vector<PendingGcRoot> pending_gc_roots_;
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
  mlir::Block* bounds_fail_block_ = nullptr;
  mlir::Value safepoint_counter_;
  SlotPromotion promoted_slots_;
};

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

mlir::OwningOpRef<mlir::ModuleOp> translate_tir_to_mlir(
    const tir::Module& tir_module,
    mlir::MLIRContext& context,
    DiagnosticEngine& diagnostics,
    bool fast_math,
    RuntimeConfiguration configuration) {

  mlir::OpBuilder builder(&context);
  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::ModuleOp::create(builder.getUnknownLoc());
  builder.setInsertionPointToStart(module->getBody());
  const BorrowedEscapeSummary borrowed_summary =
      summarize_borrowed_parameters(tir_module);
  const CollectionSummary collections = summarize_collections(tir_module);
  const std::unordered_set<std::string> mutator_regions =
      summarize_mutator_regions(tir_module);
  // One table for the whole module: literal globals live at module scope, so the
  // names must not restart for every function the lowering visits.
  StringLiteralTable string_literals;

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
      func_op.setArgAttrsAttr(builder.getArrayAttr(argument_attrs));
    }

    FunctionTranslator translator(
        *module, *tir_fn, borrowed_summary, collections, func_op, builder,
        &context, diagnostics, fast_math, configuration, string_literals,
        symbol == "main", mutator_regions.count(tir_fn->name()) != 0);
    if (!translator.translate()) return nullptr;

    builder.setInsertionPointAfter(func_op);

    if (is_entry_with_args &&
        !emit_entry_wrapper(*module, builder, *tir_fn, symbol,
                            mutator_regions.count(tir_fn->name()) != 0,
                            configuration, diagnostics)) {
      return nullptr;
    }
  }

  return module;
}

}  // namespace nirq
