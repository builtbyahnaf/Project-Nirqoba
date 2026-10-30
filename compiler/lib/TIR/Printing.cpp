#include "nirqoba/TIR/Printing.h"

#include <sstream>

namespace nirq {
namespace {

std::string escape(const std::string& value) {
  std::string escaped;
  for (const char c : value) {
    switch (c) {
      case '\n': escaped += "\n"; break;
      case '\t': escaped += "\t"; break;
      case '\r': escaped += "\r"; break;
      case '"': escaped += "\\\""; break;
      case '\': escaped += "\\\\"; break;
      default: escaped.push_back(c); break;
    }
  }
  return escaped;
}

std::string value_name(tir::Value value) {
  return value.is_valid() ? "%" + std::to_string(value.id) : "<none>";
}

}  // namespace

std::string print_tir(const tir::Module& module) {
  std::ostringstream out;
  out << "tir.module\n";
  for (const auto& function : module.functions()) {
    out << "  func @" << function->name() << "() -> " << function->return_type().mangle()
        << " {\n";
    for (const tir::Op& op : function->operations()) {
      out << "    ";
      if (op.result.is_valid() && op.kind != tir::OpKind::Store) {
        out << value_name(op.result) << " = ";
      }
      out << "tir." << tir::op_name(op.kind);

      switch (op.kind) {
        case tir::OpKind::ConstantInt:
          out << " " << op.integer_value << " : " << op.result.type.mangle();
          break;
        case tir::OpKind::ConstantString:
          out << " \"" << escape(op.string_value) << "\"";
          break;
        case tir::OpKind::Alloca:
          out << " \"" << op.slot_name << "\" : " << op.result.type.mangle();
          break;
        case tir::OpKind::Store:
          out << " " << value_name(op.lhs) << ", " << value_name(op.operand);
          break;
        case tir::OpKind::Load:
          out << " " << value_name(op.lhs) << " : " << op.result.type.mangle();
          break;
        case tir::OpKind::Binary:
          out << "." << tir::binary_op_name(op.binary_op) << " " << value_name(op.lhs) << ", "
              << value_name(op.rhs);
          break;
        case tir::OpKind::PrintString:
          out << " \"" << escape(op.string_value) << "\"";
          break;
        case tir::OpKind::Return:
          if (op.operand.is_valid()) out << " " << value_name(op.operand);
          break;
      }
      out << "\n";
    }
    out << "  }\n";
  }
  return out.str();
}

}  // namespace nirq
