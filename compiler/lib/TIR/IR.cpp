#include "nirqoba/TIR/IR.h"

namespace nirq::tir {

Value Function::allocate_value(Type type) {
  Value value;
  value.id = static_cast<int>(types_.size());
  value.type = type;
  types_.push_back(type);
  return value;
}

std::string_view op_name(OpKind kind) {
  switch (kind) {
    case OpKind::ConstantInt: return "const_int";
    case OpKind::ConstantString: return "const_string";
    case OpKind::Alloca: return "alloca";
    case OpKind::Store: return "store";
    case OpKind::Load: return "load";
    case OpKind::Binary: return "binary";
    case OpKind::PrintString: return "print_string";
    case OpKind::Return: return "return";
  }
  return "unknown";
}

std::string_view binary_op_name(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add: return "add";
    case BinaryOp::Subtract: return "sub";
    case BinaryOp::Multiply: return "mul";
    case BinaryOp::Divide: return "div";
    case BinaryOp::Remainder: return "rem";
  }
  return "unknown";
}

}  // namespace nirq::tir
