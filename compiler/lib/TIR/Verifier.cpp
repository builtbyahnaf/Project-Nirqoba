#include "nirqoba/TIR/Verifier.h"

#include <string>

namespace nirq {
namespace {

constexpr const char* kStructure = "NRQ4003";
constexpr const char* kType = "NRQ4004";

bool value_in_range(const tir::Function& function, tir::Value value) {
  return value.is_valid() && value.id < static_cast<int>(function.value_count());
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
    if (last.kind != tir::OpKind::Return) {
      diagnostics.error(kStructure, last.location,
                        "internal: function '" + function->name() +
                            "' does not end in a return operation");
      ok = false;
    }

    for (const tir::Op& op : operations) {
      // Operand numbering: every referenced value must exist in this function.
      for (const tir::Value value : {op.result, op.operand, op.lhs, op.rhs}) {
        if (value.is_valid() && !value_in_range(*function, value)) {
          diagnostics.error(kStructure, op.location,
                            "internal: operation references undefined value %" +
                                std::to_string(value.id));
          ok = false;
        }
      }

      switch (op.kind) {
        case tir::OpKind::ConstantInt:
        case tir::OpKind::ConstantString:
        case tir::OpKind::Alloca:
        case tir::OpKind::Store:
          break;
        case tir::OpKind::PrintString:
          // The literal travels as an attribute, so there is no operand to
          // type-check; an empty literal is legal (print writes a newline).
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
        case tir::OpKind::Binary: {
          for (const tir::Value operand : {op.lhs, op.rhs}) {
            if (operand.is_valid() && !function->value_type(operand.id).is_int()) {
              diagnostics.error(kType, op.location,
                                "internal: arithmetic operand is not i64");
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
            }
          } else if (function->return_type().is_int()) {
            diagnostics.error(kType, op.location,
                              "internal: non-void function returns no value");
            ok = false;
          }
          break;
        }
      }
    }
  }

  return ok;
}

}  // namespace nirq
