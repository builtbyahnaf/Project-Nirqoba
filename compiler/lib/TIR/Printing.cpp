#include "nirqoba/TIR/Printing.h"

#include <sstream>

namespace nirq {
namespace {

std::string escape(const std::string& value) {
  std::string escaped;
  for (const char c : value) {
    switch (c) {
      case '\n': escaped += "\\n"; break;
      case '\t': escaped += "\\t"; break;
      case '\r': escaped += "\\r"; break;
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
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
    out << "  func @" << function->name() << "(";
    for (std::size_t i = 0; i < function->parameters().size(); ++i) {
      if (i > 0) out << ", ";
      out << "%" << function->parameters()[i].value.id << " : "
          << function->parameters()[i].type.mangle();
    }
    out << ") -> " << function->return_type().mangle() << " {\n";

    for (const tir::Op& op : function->operations()) {
      out << "    ";
      if (op.kind == tir::OpKind::Label) {
        out << "^bb" << op.label_id << ":\n";
        continue;
      }
      if (op.result.is_valid() && op.kind != tir::OpKind::Store) {
        out << value_name(op.result) << " = ";
      }
      out << "tir." << tir::op_name(op.kind);

      switch (op.kind) {
        case tir::OpKind::ConstantInt:
          out << " " << op.integer_value << " : " << op.result.type.mangle();
          break;
        case tir::OpKind::ConstantFloat:
          out << " " << op.float_value << " : " << op.result.type.mangle();
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
        case tir::OpKind::Unary:
          out << "." << tir::unary_op_name(op.unary_op) << " " << value_name(op.operand);
          break;
        case tir::OpKind::Binary:
          out << "." << tir::binary_op_name(op.binary_op) << " " << value_name(op.lhs) << ", "
              << value_name(op.rhs);
          break;
        case tir::OpKind::Cast:
          out << "." << tir::cast_kind_name(op.cast_kind) << " " << value_name(op.operand)
              << " : " << op.result.type.mangle();
          break;
        case tir::OpKind::Call:
          out << " @" << op.callee << "(";
          for (std::size_t i = 0; i < op.arguments.size(); ++i) {
            if (i > 0) out << ", ";
            out << value_name(op.arguments[i]);
          }
          out << ")";
          break;
        case tir::OpKind::PrintString:
          out << " \"" << escape(op.string_value) << "\"";
          break;
        case tir::OpKind::Print:
          out << " " << value_name(op.operand) << " : " << op.operand.type.mangle();
          break;
        case tir::OpKind::Branch:
          out << " ^bb" << op.label_id;
          break;
        case tir::OpKind::BranchCond:
          out << " " << value_name(op.operand) << ", ^bb" << op.true_label << ", ^bb"
              << op.false_label;
          break;
        case tir::OpKind::Return:
          if (op.operand.is_valid()) out << " " << value_name(op.operand);
          break;
        case tir::OpKind::Label:
          break;
      }
      out << "\n";
    }
    out << "  }\n";
  }
  return out.str();
}

}  // namespace nirq
