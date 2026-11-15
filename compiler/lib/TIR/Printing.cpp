#include "nirqoba/TIR/Printing.h"

#include <sstream>

namespace nirq {
using namespace std;
namespace {

string escape(const string& value) {
  string escaped;
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

string value_name(tir::Value value) {
  return value.is_valid() ? "%" + to_string(value.id) : "<none>";
}

string_view transfer_name(tir::TransferKind kind) {
  switch (kind) {
    case tir::TransferKind::Local: return "local";
    case tir::TransferKind::Return: return "return";
    case tir::TransferKind::Argument: return "argument";
    case tir::TransferKind::Field: return "field";
    case tir::TransferKind::Element: return "element";
  }
  return "unknown";
}

}  // namespace

string print_tir(const tir::Module& module) {
  ostringstream out;
  out << "tir.module\n";
  for (const auto& function : module.functions()) {
    out << "  func @" << function->name() << "(";
    for (size_t i = 0; i < function->parameters().size(); ++i) {
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
        case tir::OpKind::ConstantNull:
          out << " : " << op.result.type.mangle();
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
          for (size_t i = 0; i < op.arguments.size(); ++i) {
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
        case tir::OpKind::ArrayAlloca:
          out << " " << op.slot_name << "[" << op.integer_value << "] : " << op.result.type.mangle();
          break;
        case tir::OpKind::GetElementPtr:
          out << " " << value_name(op.lhs) << "[" << value_name(op.operand)
              << "] field=" << op.field_index;
          break;
        case tir::OpKind::SoaColumnPtr:
          out << " " << value_name(op.lhs) << "[" << value_name(op.operand)
              << "] field=" << op.field_index;
          break;
        case tir::OpKind::AddressOf:
          out << " " << value_name(op.operand) << " : " << op.result.type.mangle();
          break;
        case tir::OpKind::AggregatePack:
          out << " (";
          for (size_t i = 0; i < op.aggregate_elements.size(); ++i) {
            if (i > 0) out << ", ";
            out << value_name(op.aggregate_elements[i]);
          }
          out << ") : " << op.result.type.mangle();
          break;
        case tir::OpKind::AggregateExtract:
          out << " " << value_name(op.operand) << "." << op.field_index
              << " : " << op.result.type.mangle();
          break;
        case tir::OpKind::Length:
          out << " " << value_name(op.operand) << " : " << op.operand.type.mangle();
          break;
        case tir::OpKind::DynamicIndex:
          out << " " << value_name(op.lhs) << "[" << value_name(op.operand)
              << "] : " << op.result.type.mangle();
          break;
        case tir::OpKind::Move:
          out << " " << value_name(op.operand) << " -> " << value_name(op.result)
              << " kind=" << transfer_name(op.transfer_kind);
          break;
        case tir::OpKind::Borrow:
          out << " " << value_name(op.operand) << " from " << value_name(op.lhs)
              << (op.mutable_borrow ? " mutable" : " immutable");
          if (!op.borrow_path.empty()) out << " path=\"" << op.borrow_path << "\"";
          break;
        case tir::OpKind::EndBorrow:
          out << " " << value_name(op.operand);
          break;
        case tir::OpKind::Drop:
          out << " " << value_name(op.operand) << " obligation=" << op.obligation_id
              << " release=" << op.string_value;
          break;
        case tir::OpKind::Unreachable:
          break;
      }
      out << "\n";
    }
    out << "  }\n";
  }
  return out.str();
}

}  // namespace nirq
