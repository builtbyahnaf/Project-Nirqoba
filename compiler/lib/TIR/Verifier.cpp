#include "nirqoba/TIR/Verifier.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace nirq {
using namespace std;
namespace {

constexpr const char* kStructure = "NRQ4003";
constexpr const char* kType = "NRQ4004";

bool value_in_range(const tir::Function& function, tir::Value value) {
  return value.is_valid() && value.id < static_cast<int>(function.value_count());
}

bool is_borrowed(ValueOwnership ownership) {
  return ownership == ValueOwnership::Borrowed ||
         ownership == ValueOwnership::MutableBorrowed;
}

bool is_aggregate_type(const Type& type) {
  return type.is_array() || type.is_dynamic_array() || type.is_tuple() ||
         type.is_struct();
}

bool ownership_compatible_with_type(const Type& type, ValueOwnership ownership) {
  if (!type.is_valid() || ownership == ValueOwnership::Trivial) return true;
  switch (ownership) {
    case ValueOwnership::Trivial:
      return true;
    case ValueOwnership::Owned:
      return type.is_pointer() || type.is_owned_array() || type.is_soa() ||
             is_aggregate_type(type);
    case ValueOwnership::Borrowed:
    case ValueOwnership::MutableBorrowed:
      return type.is_pointer() || type.is_array_view() || type.is_string();
    // TIR represents owned, shared, raw and managed pointers with the same
    // pointer shape; the ownership field is what distinguishes them.
    case ValueOwnership::Shared:
      return type.is_pointer() || type.is_soa() || is_aggregate_type(type);
    case ValueOwnership::GcManaged:
      return type.is_managed_pointer() || type.is_pointer() || type.is_soa() ||
             is_aggregate_type(type);
    case ValueOwnership::Raw:
      return type.is_pointer();
  }
  return false;
}

bool is_consuming_release(const std::string& callee) {
  static const std::unordered_set<std::string> releases = {
      "memory.deallocate",    "array.owned_release", "soa.owned_release",
      "aggregate.owned_drop", "memory.shared_release", "memory.secure_release",
      "file.close",           "socket.close",         "mutex.destroy",
      "memory.unmap_file",
  };
  return releases.count(callee) != 0;
}

bool release_matches_type(const std::string& callee, const Type& type) {
  if (!type.is_valid()) return true;
  if (callee == "array.owned_release") return type.is_owned_array();
  if (callee == "soa.owned_release") return type.is_soa();
  if (callee == "aggregate.owned_drop") return is_aggregate_type(type);
  // Resource domains and shared/managed pointer kinds live in Sema and in the
  // TIR ownership/domain fields, not in the MLIR-visible pointer shape. The
  // runtime identity check here is therefore the pointer/array/aggregate shape;
  // the ownership check below distinguishes the release families.
  if (callee == "memory.shared_release" || callee == "memory.secure_release" ||
      callee == "file.close" || callee == "socket.close" ||
      callee == "mutex.destroy" || callee == "memory.unmap_file" ||
      callee == "memory.deallocate")
    return type.is_pointer();
  return true;
}

const tir::Function::Parameter* find_parameter(const tir::Function& function,
                                               const std::string& name) {
  for (const tir::Function::Parameter& parameter : function.parameters())
    if (parameter.name == name) return &parameter;
  return nullptr;
}

std::string function_value(const tir::Function& function, int id) {
  return "internal: function '" + function.name() + "', value %" +
         to_string(id) + ": ";
}

}  // namespace

bool verify_tir_module(const tir::Module& module, DiagnosticEngine& diagnostics) {
  bool ok = true;

  for (const auto& function : module.functions()) {
    const auto& operations = function->operations();
    if (operations.empty()) {
      diagnostics.error(kStructure, SourceLocation{},
                        "internal: function '" + function->name() + "' has no operations");
      ok = false;
      continue;
    }

    const tir::Op& last = operations.back();
    if (last.kind != tir::OpKind::Return && last.kind != tir::OpKind::Branch &&
        last.kind != tir::OpKind::BranchCond && last.kind != tir::OpKind::Unreachable) {
      diagnostics.error(kStructure, last.location,
                        "internal: function '" + function->name() +
                            "' does not end in a return or branch operation");
      ok = false;
    }

    // Ownership is part of TIR's contract, not a Sema-only annotation. The
    // checks below are deliberately structural: they reject states lowering
    // could not execute correctly, while leaving lexical lifetime paths to the
    // existing Sema borrow analysis.
    for (const tir::Function::Parameter& parameter : function->parameters()) {
      if (!ownership_compatible_with_type(parameter.type, parameter.ownership)) {
        diagnostics.error(kType, SourceLocation{},
                          function_value(*function, parameter.value.id) +
                              "parameter '" + parameter.name +
                              "' has ownership incompatible with type " +
                              parameter.type.mangle());
        ok = false;
      }
    }

    if (!ownership_compatible_with_type(function->return_type(),
                                        function->return_ownership())) {
      diagnostics.error(kType, SourceLocation{},
                        "internal: function '" + function->name() +
                            "' has return ownership incompatible with type " +
                            function->return_type().mangle());
      ok = false;
    }

    const std::string& borrow_source = function->return_borrow_parameter();
    if (!borrow_source.empty()) {
      const tir::Function::Parameter* source =
          find_parameter(*function, borrow_source);
      if (source == nullptr) {
        diagnostics.error(kType, SourceLocation{},
                          "internal: function '" + function->name() +
                              "' returns a borrow of unknown parameter '" +
                              borrow_source + "'");
        ok = false;
      } else {
        if (!is_borrowed(source->ownership)) {
          diagnostics.error(kType, SourceLocation{},
                            "internal: function '" + function->name() +
                                "' returns a borrow of non-borrowed parameter '" +
                                borrow_source + "'");
          ok = false;
        }
        if (!is_borrowed(function->return_ownership())) {
          diagnostics.error(kType, SourceLocation{},
                            "internal: function '" + function->name() +
                                "' names a borrow source without borrowed return ownership");
          ok = false;
        }
        if (source->type != function->return_type()) {
          diagnostics.error(kType, SourceLocation{},
                            "internal: function '" + function->name() +
                                "' borrow source and return types differ");
          ok = false;
        }
      }
    } else if (is_borrowed(function->return_ownership())) {
      diagnostics.error(kType, SourceLocation{},
                        "internal: function '" + function->name() +
                            "' has borrowed return ownership without a source parameter");
      ok = false;
    }

    std::unordered_map<int, std::string> consumed;

    auto check_value = [&](tir::Value value, const SourceLocation& location,
                           const std::string& role) {
      if (!value.is_valid()) return;
      if (const auto found = consumed.find(value.id); found != consumed.end()) {
        diagnostics.error(kType, location,
                          function_value(*function, value.id) + role +
                              " used after it was consumed by '" + found->second +
                              "'");
        ok = false;
      }
      if (value_in_range(*function, value) &&
          !ownership_compatible_with_type(value.type, value.ownership)) {
        diagnostics.error(kType, location,
                          function_value(*function, value.id) + role +
                              " has ownership incompatible with type " +
                              value.type.mangle());
        ok = false;
      }
    };

    for (const tir::Op& op : operations) {
      // Operand numbering: every referenced value must exist in this function.
      for (const tir::Value value : {op.result, op.operand, op.lhs, op.rhs}) {
        if (value.is_valid() && !value_in_range(*function, value)) {
          diagnostics.error(kStructure, op.location,
                            "internal: operation references undefined value %" +
                                to_string(value.id));
          ok = false;
        }
      }
      for (const tir::Value arg : op.arguments) {
        if (arg.is_valid() && !value_in_range(*function, arg)) {
          diagnostics.error(kStructure, op.location,
                            "internal: call operation references undefined argument value %" +
                                to_string(arg.id));
          ok = false;
        }
      }

      check_value(op.operand, op.location, "operand");
      check_value(op.lhs, op.location, "lhs");
      check_value(op.rhs, op.location, "rhs");
      for (const tir::Value argument : op.arguments)
        check_value(argument, op.location, "argument");
      for (const tir::Value element : op.aggregate_elements)
        check_value(element, op.location, "aggregate element");

      if (op.kind == tir::OpKind::Call && is_consuming_release(op.callee)) {
        for (const tir::Value argument : op.arguments) {
          if (!argument.is_valid()) continue;
          if (is_borrowed(argument.ownership)) {
            diagnostics.error(kType, op.location,
                              function_value(*function, argument.id) +
                                  "borrowed value passed to releasing call '" +
                                  op.callee + "'");
            ok = false;
          }
          if (!release_matches_type(op.callee, argument.type)) {
            diagnostics.error(kType, op.location,
                              function_value(*function, argument.id) +
                                  "release '" + op.callee +
                                  "' does not match value type " +
                                  argument.type.mangle());
            ok = false;
          }
          if (op.callee == "memory.shared_release" &&
              argument.ownership != ValueOwnership::Trivial &&
              argument.ownership != ValueOwnership::Shared) {
            diagnostics.error(kType, op.location,
                              function_value(*function, argument.id) +
                                  "shared release requires a shared value");
            ok = false;
          }
          if (op.callee == "memory.deallocate" &&
              argument.ownership == ValueOwnership::Shared) {
            diagnostics.error(kType, op.location,
                              function_value(*function, argument.id) +
                                  "shared value passed to memory.deallocate");
            ok = false;
          }
        }
      }

      switch (op.kind) {
        case tir::OpKind::ConstantInt:
        case tir::OpKind::ConstantNull:
        case tir::OpKind::ConstantFloat:
        case tir::OpKind::ConstantString:
        case tir::OpKind::Alloca:
        case tir::OpKind::Store:
        case tir::OpKind::PrintString:
        case tir::OpKind::Print:
        case tir::OpKind::Label:
        case tir::OpKind::Branch:
        case tir::OpKind::BranchCond:
        case tir::OpKind::Call:
        case tir::OpKind::ArrayAlloca:
        case tir::OpKind::GetElementPtr:
        case tir::OpKind::AddressOf:
        case tir::OpKind::SoaColumnPtr:
        case tir::OpKind::AggregatePack:
        case tir::OpKind::AggregateExtract:
        case tir::OpKind::Length:
        case tir::OpKind::DynamicIndex:
        case tir::OpKind::Unreachable:
          break;
        case tir::OpKind::Load: {
          if (op.lhs.is_valid() && function->value_type(op.lhs.id) != op.result.type) {
            diagnostics.error(kType, op.location,
                              "internal: load type " + op.result.type.mangle() +
                                  " does not match slot type " +
                                  function->value_type(op.lhs.id).mangle());
            ok = false;
          }
          break;
        }
        case tir::OpKind::Unary: {
          if (op.operand.is_valid() && !value_in_range(*function, op.operand)) {
            diagnostics.error(kType, op.location, "internal: unary operand invalid");
            ok = false;
          }
          break;
        }
        case tir::OpKind::Cast: {
          if (op.operand.is_valid() && !value_in_range(*function, op.operand)) {
            diagnostics.error(kType, op.location, "internal: cast operand invalid");
            ok = false;
          }
          break;
        }
        case tir::OpKind::Binary: {
          for (const tir::Value operand : {op.lhs, op.rhs}) {
            if (operand.is_valid() && !value_in_range(*function, operand)) {
              diagnostics.error(kType, op.location, "internal: binary operand invalid");
              ok = false;
            }
          }
          break;
        }
        case tir::OpKind::Return: {
          if (op.operand.is_valid()) {
            if (function->return_type().is_void()) {
              diagnostics.error(kType, op.location,
                                "internal: void function returns a value");
              ok = false;
            } else if (function->return_type() != function->value_type(op.operand.id)) {
              diagnostics.error(kType, op.location,
                                "internal: return type does not match function type");
              ok = false;
            } else if (function->return_ownership() != ValueOwnership::Trivial &&
                       op.operand.ownership != ValueOwnership::Trivial &&
                       function->return_ownership() != op.operand.ownership) {
              diagnostics.error(kType, op.location,
                                "internal: return ownership does not match operand ownership");
              ok = false;
            }
          } else if (!function->return_type().is_void()) {
            diagnostics.error(kType, op.location,
                                "internal: non-void function returns no value");
            ok = false;
          }
          break;
        }
      }

      if (op.kind == tir::OpKind::Call && is_consuming_release(op.callee)) {
        for (const tir::Value argument : op.arguments)
          if (argument.is_valid()) consumed.emplace(argument.id, op.callee);
      }
    }
  }

  return ok;
}

}  // namespace nirq
