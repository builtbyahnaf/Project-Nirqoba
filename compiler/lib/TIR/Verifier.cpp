#include "nirqoba/TIR/Verifier.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace nirq {
using namespace std;
namespace {

constexpr const char* kStructure = "NRQ4003";
constexpr const char* kType = "NRQ4004";
// Phase 1 ownership diagnostics. They are deliberately separate from the
// structural/type codes so a failure names the ownership rule that was broken.
constexpr const char* kOwnership = "NRQ4006";
constexpr const char* kBorrow = "NRQ4007";
constexpr const char* kDestruction = "NRQ4008";

bool is_owning(ValueOwnership ownership) {
  return is_owning_ownership(ownership) ||
         ownership == ValueOwnership::GcManaged;
}

std::string_view transfer_name(tir::TransferKind kind) {
  switch (kind) {
    case tir::TransferKind::Local: return "local";
    case tir::TransferKind::Return: return "return";
    case tir::TransferKind::Argument: return "argument";
    case tir::TransferKind::Field: return "field";
    case tir::TransferKind::Element: return "element";
  }
  return "unknown";
}

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
    case ValueOwnership::ArenaBound:
      // Arena-bound storage is a pointer, array or aggregate owned by an arena.
      return type.is_pointer() || type.is_owned_array() || type.is_soa() ||
             is_aggregate_type(type);
    case ValueOwnership::Weak:
      return type.is_pointer() || type.is_managed_pointer();
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
  std::unordered_map<std::string, const tir::Function*> functions;
  for (const auto& function : module.functions())
    functions.emplace(function->name(), function.get());

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
    // First-class ownership state derived from Move/Borrow/EndBorrow/Drop.
    std::unordered_map<int, std::string> moved;
    std::unordered_map<int, SourceLocation> moved_at;
    std::unordered_map<int, const tir::Op*> borrows;
    std::unordered_set<int> ended_borrows;
    std::unordered_map<int, SourceLocation> ended_at;
    std::unordered_set<int> discharged_obligations;
    std::unordered_map<int, SourceLocation> discharged_at;
    std::unordered_set<int> address_of_results;

    auto check_value = [&](tir::Value value, const SourceLocation& location,
                           const std::string& role) {
      if (!value.is_valid()) return;
      if (const auto found = moved.find(value.id); found != moved.end()) {
        diagnostics.error(kOwnership, location,
                          function_value(*function, value.id) + role +
                              " used after it was moved by '" + found->second + "'");
        if (const auto at = moved_at.find(value.id); at != moved_at.end())
          diagnostics.note(kOwnership, at->second,
                           "value was transferred to a new owner here");
        ok = false;
      }
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

      if (op.result.is_valid() && value_in_range(*function, op.result) &&
          op.result.ownership != function->value_ownership(op.result.id)) {
        diagnostics.error(kType, op.location,
                          function_value(*function, op.result.id) +
                              "operation result ownership disagrees with the value record");
        ok = false;
      }

      if (op.kind == tir::OpKind::AddressOf && op.result.is_valid())
        address_of_results.insert(op.result.id);

      check_value(op.operand, op.location, "operand");
      check_value(op.lhs, op.location, "lhs");
      check_value(op.rhs, op.location, "rhs");
      for (const tir::Value argument : op.arguments)
        check_value(argument, op.location, "argument");
      for (const tir::Value element : op.aggregate_elements)
        check_value(element, op.location, "aggregate element");

      if (op.kind == tir::OpKind::Call) {
        const auto callee = functions.find(op.callee);
        if (callee != functions.end()) {
          const auto& parameters = callee->second->parameters();
          for (std::size_t index = 0;
               index < op.arguments.size() && index < parameters.size();
               ++index) {
            const tir::Value& argument = op.arguments[index];
            const tir::Function::Parameter& parameter = parameters[index];
            if (!argument.is_valid() || argument.ownership == ValueOwnership::Trivial)
              continue;
            const bool argument_borrows = is_borrowed(argument.ownership);
            const bool parameter_borrows = is_borrowed(parameter.ownership);
            // An owning value may be borrowed implicitly for a borrowed
            // parameter. The reverse is a move out of a borrow and is invalid.
            if (!parameter_borrows && argument_borrows) {
              diagnostics.error(kType, op.location,
                                function_value(*function, argument.id) +
                                    "borrowed value passed to owning parameter '" +
                                    parameter.name + "' of '" + op.callee + "'");
              ok = false;
            }
            if (parameter.ownership == ValueOwnership::MutableBorrowed &&
                argument.ownership == ValueOwnership::Borrowed) {
              diagnostics.error(kType, op.location,
                                function_value(*function, argument.id) +
                                    "immutable borrow passed to mutable parameter '" +
                                    parameter.name + "' of '" + op.callee + "'");
              ok = false;
            }
            if (parameter.ownership == ValueOwnership::Shared &&
                argument.ownership != ValueOwnership::Shared) {
              diagnostics.error(kType, op.location,
                                function_value(*function, argument.id) +
                                    "non-shared value passed to shared parameter '" +
                                    parameter.name + "' of '" + op.callee + "'");
              ok = false;
            }
            if (parameter.ownership == ValueOwnership::Owned &&
                argument.ownership == ValueOwnership::Shared) {
              diagnostics.error(kType, op.location,
                                function_value(*function, argument.id) +
                                    "shared value passed to owned parameter '" +
                                    parameter.name + "' of '" + op.callee + "'");
              ok = false;
            }
          }
        }
      }

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
        case tir::OpKind::Move: {
          if (!is_transferable_ownership(op.operand.ownership)) {
            diagnostics.error(kOwnership, op.location,
                              "internal: move source must be owned or shared, not '" +
                                  std::string(ownership_name(op.operand.ownership)) +
                                  "'");
            ok = false;
          }
          if (op.result.is_valid() && value_in_range(*function, op.result)) {
            if (op.result.id == op.operand.id) {
              diagnostics.error(kOwnership, op.location,
                                "internal: move must produce a distinct value number");
              ok = false;
            }
            if (op.result.type != op.operand.type) {
              diagnostics.error(kOwnership, op.location,
                                "internal: move result type " +
                                    op.result.type.mangle() +
                                    " does not match source type " +
                                    op.operand.type.mangle());
              ok = false;
            }
            if (op.result.ownership != op.operand.ownership) {
              diagnostics.error(kOwnership, op.location,
                                "internal: move result ownership '" +
                                    std::string(ownership_name(op.result.ownership)) +
                                    "' does not match source ownership '" +
                                    std::string(ownership_name(op.operand.ownership)) +
                                    "'");
              ok = false;
            }
          }
          if (op.operand.is_valid()) {
            moved[op.operand.id] = std::string(tir::op_name(op.kind)) + " (" +
                                   std::string(transfer_name(op.transfer_kind)) + ")";
            moved_at[op.operand.id] = op.location;
          }
          break;
        }
        case tir::OpKind::Borrow: {
          const bool borrow_is_mutable =
              op.operand.ownership == ValueOwnership::MutableBorrowed;
          if (!is_borrowing_ownership(op.operand.ownership) ||
              op.operand.ownership == ValueOwnership::Weak) {
            diagnostics.error(kBorrow, op.location,
                              "internal: borrow value must have borrowed ownership, not '" +
                                  std::string(ownership_name(op.operand.ownership)) +
                                  "'");
            ok = false;
          } else if (borrow_is_mutable != op.mutable_borrow) {
            diagnostics.error(kBorrow, op.location,
                              "internal: borrow mutability disagrees with value ownership '" +
                                  std::string(ownership_name(op.operand.ownership)) +
                                  "'");
            ok = false;
          }
          if (is_borrowing_ownership(op.lhs.ownership)) {
            diagnostics.error(kBorrow, op.location,
                              "internal: a borrowed value cannot be the owner of another borrow");
            ok = false;
          }
          if (op.operand.is_valid() &&
              address_of_results.count(op.operand.id) == 0) {
            diagnostics.error(kBorrow, op.location,
                              function_value(*function, op.operand.id) +
                                  "borrow does not originate from an address_of operation");
            ok = false;
          }
          if (borrows.count(op.operand.id) != 0) {
            diagnostics.error(kBorrow, op.location,
                              function_value(*function, op.operand.id) +
                                  "borrowed more than once without an intervening end_borrow");
            ok = false;
          }
          bool has_lifetime = false;
          for (const tir::LifetimeRecord& lifetime : function->lifetimes())
            if (lifetime.borrow.is_valid() && lifetime.borrow.id == op.operand.id &&
                lifetime.source.is_valid() && lifetime.source.id == op.lhs.id &&
                lifetime.kind == tir::LifetimeKind::Lexical)
              has_lifetime = true;
          if (!has_lifetime) {
            diagnostics.error(kBorrow, op.location,
                              function_value(*function, op.operand.id) +
                                  "borrow has no matching lifetime record");
            ok = false;
          }
          borrows[op.operand.id] = &op;
          break;
        }
        case tir::OpKind::EndBorrow: {
          if (!is_borrowing_ownership(op.operand.ownership)) {
            diagnostics.error(kBorrow, op.location,
                              "internal: end_borrow operand is not a borrowed value");
            ok = false;
          }
          if (borrows.count(op.operand.id) == 0) {
            diagnostics.error(kBorrow, op.location,
                              function_value(*function, op.operand.id) +
                                  "ended a borrow that was never created");
            ok = false;
          } else if (ended_borrows.count(op.operand.id) != 0) {
            diagnostics.error(kBorrow, op.location,
                              function_value(*function, op.operand.id) +
                                  "borrow ended more than once");
            if (const auto at = ended_at.find(op.operand.id); at != ended_at.end())
              diagnostics.note(kBorrow, at->second,
                               "borrow was first ended here");
            ok = false;
          } else {
            ended_borrows.insert(op.operand.id);
            ended_at[op.operand.id] = op.location;
          }
          break;
        }
        case tir::OpKind::Drop: {
          if (!is_owning(op.operand.ownership)) {
            diagnostics.error(kDestruction, op.location,
                              "internal: drop operand must be owning, not '" +
                                  std::string(ownership_name(op.operand.ownership)) +
                                  "'");
            ok = false;
          }
          const tir::DestructionObligation* obligation =
              function->obligation(op.obligation_id);
          if (obligation == nullptr) {
            diagnostics.error(kDestruction, op.location,
                              "internal: drop names unknown obligation " +
                                  std::to_string(op.obligation_id));
            ok = false;
          } else {
            if (obligation->transferred) {
              diagnostics.error(kDestruction, op.location,
                                "internal: drop discharges an obligation already transferred to a new owner");
              ok = false;
            }
            if (discharged_obligations.count(obligation->id) != 0) {
              diagnostics.error(kDestruction, op.location,
                                "internal: obligation " +
                                    std::to_string(obligation->id) +
                                    " is discharged more than once");
              if (const auto at = discharged_at.find(obligation->id);
                  at != discharged_at.end())
                diagnostics.note(kDestruction, at->second,
                                 "obligation was first discharged here");
              ok = false;
            } else {
              discharged_obligations.insert(obligation->id);
              discharged_at[obligation->id] = op.location;
            }
            if (obligation->type != op.operand.type) {
              diagnostics.error(kDestruction, op.location,
                                "internal: drop type " + op.operand.type.mangle() +
                                    " does not match obligation type " +
                                    obligation->type.mangle());
              ok = false;
            }
            if (obligation->release != op.string_value) {
              diagnostics.error(kDestruction, op.location,
                                "internal: drop release '" + op.string_value +
                                    "' does not match obligation release '" +
                                    obligation->release + "'");
              ok = false;
            }
            if (!release_matches_type(op.string_value, op.operand.type)) {
              diagnostics.error(kDestruction, op.location,
                                "internal: release '" + op.string_value +
                                    "' does not match drop type " +
                                    op.operand.type.mangle());
              ok = false;
            }
          }
          break;
        }
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

    // Lifetime table validation. The operation stream is authoritative for the
    // borrow itself; the table preserves the source relationship across any
    // transformation that does not understand the new operations.
    for (const tir::LifetimeRecord& lifetime : function->lifetimes()) {
      if (lifetime.kind == tir::LifetimeKind::Lexical) {
        if (!value_in_range(*function, lifetime.borrow) ||
            !value_in_range(*function, lifetime.source)) {
          diagnostics.error(kBorrow, lifetime.location,
                            "internal: lexical lifetime record references an undefined value");
          ok = false;
          continue;
        }
        if (!is_borrowing_ownership(lifetime.borrow.ownership)) {
          diagnostics.error(kBorrow, lifetime.location,
                            "internal: lexical lifetime borrow is not borrowed");
          ok = false;
        }
        if (is_borrowing_ownership(lifetime.source.ownership)) {
          diagnostics.error(kBorrow, lifetime.location,
                            "internal: lexical lifetime source is itself borrowed");
          ok = false;
        }
      } else if (lifetime.kind == tir::LifetimeKind::Parameter) {
        if (!value_in_range(*function, lifetime.borrow)) {
          diagnostics.error(kBorrow, lifetime.location,
                            "internal: parameter lifetime references an undefined value");
          ok = false;
          continue;
        }
        const tir::Function::Parameter* parameter =
            find_parameter(*function, lifetime.source_parameter);
        if (parameter == nullptr) {
          diagnostics.error(kBorrow, lifetime.location,
                            "internal: parameter lifetime names unknown parameter '" +
                                lifetime.source_parameter + "'");
          ok = false;
        } else if (!is_borrowed(parameter->ownership)) {
          diagnostics.error(kBorrow, lifetime.location,
                            "internal: parameter lifetime names non-borrowed parameter '" +
                                lifetime.source_parameter + "'");
          ok = false;
        }
      } else if (lifetime.kind == tir::LifetimeKind::Return) {
        const tir::Function::Parameter* parameter =
            find_parameter(*function, lifetime.source_parameter);
        if (parameter == nullptr || !is_borrowed(parameter->ownership)) {
          diagnostics.error(kBorrow, lifetime.location,
                            "internal: return lifetime must name a borrowed parameter");
          ok = false;
        }
      }
    }

    // Each Drop must name the release call that actually discharges it. A drop
    // whose release is missing or reordered would let the obligation table claim
    // a cleanup that lowering never performs.
    for (std::size_t index = 0; index < operations.size(); ++index) {
      const tir::Op& drop = operations[index];
      if (drop.kind != tir::OpKind::Drop) continue;
      const tir::Op* release =
          index + 1 < operations.size() ? &operations[index + 1] : nullptr;
      if (release == nullptr || release->kind != tir::OpKind::Call ||
          release->callee != drop.string_value || release->arguments.empty() ||
          release->arguments.front().id != drop.operand.id) {
        diagnostics.error(kDestruction, drop.location,
                          "internal: drop for obligation " +
                              std::to_string(drop.obligation_id) +
                              " is not followed by its '" + drop.string_value +
                              "' release call");
        ok = false;
      }
    }

    // Obligation table validation. Completeness is intentionally not required:
    // conditional cleanup and early exits are Phase 2's path-sensitive job.
    for (const tir::DestructionObligation& obligation : function->obligations()) {
      if (!value_in_range(*function, obligation.owner)) {
        diagnostics.error(kDestruction, obligation.location,
                          "internal: destruction obligation names an undefined owner");
        ok = false;
        continue;
      }
      if (obligation.release.empty()) {
        diagnostics.error(kDestruction, obligation.location,
                          "internal: destruction obligation has no release operation");
        ok = false;
      }
      if (!release_matches_type(obligation.release, obligation.type)) {
        diagnostics.error(kDestruction, obligation.location,
                          "internal: obligation release '" + obligation.release +
                              "' does not match owner type " +
                              obligation.type.mangle());
        ok = false;
      }
    }
  }

  // Every obligation id referenced by a Drop is checked above; additionally
  // reject ids that are duplicated in the table, because a duplicate id makes
  // discharge attribution ambiguous.
  for (const auto& function : module.functions()) {
    std::unordered_set<int> ids;
    for (const tir::DestructionObligation& obligation : function->obligations())
      if (!ids.insert(obligation.id).second) {
        diagnostics.error(kDestruction, obligation.location,
                          "internal: duplicate destruction obligation id " +
                              std::to_string(obligation.id));
        ok = false;
      }
  }

  return ok;
}

}  // namespace nirq
