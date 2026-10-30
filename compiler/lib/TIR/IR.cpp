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
    case OpKind::ConstantFloat: return "const_float";
    case OpKind::ConstantString: return "const_string";
    case OpKind::Alloca: return "alloca";
    case OpKind::Store: return "store";
    case OpKind::Load: return "load";
    case OpKind::Unary: return "unary";
    case OpKind::Binary: return "binary";
    case OpKind::Cast: return "cast";
    case OpKind::Call: return "call";
    case OpKind::PrintString: return "print_string";
    case OpKind::Print: return "print";
    case OpKind::Label: return "label";
    case OpKind::Branch: return "branch";
    case OpKind::BranchCond: return "branch_cond";
    case OpKind::Return: return "return";
    case OpKind::ArrayAlloca: return "array_alloca";
    case OpKind::GetElementPtr: return "gep";
    case OpKind::AggregatePack: return "aggregate_pack";
    case OpKind::AggregateExtract: return "aggregate_extract";
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
    case BinaryOp::Exponentiate: return "pow";
    case BinaryOp::BitwiseAnd: return "and";
    case BinaryOp::BitwiseOr: return "or";
    case BinaryOp::BitwiseXor: return "xor";
    case BinaryOp::ShiftLeft: return "shl";
    case BinaryOp::ShiftRight: return "shr";
    case BinaryOp::Equal: return "eq";
    case BinaryOp::NotEqual: return "ne";
    case BinaryOp::Less: return "lt";
    case BinaryOp::LessEqual: return "le";
    case BinaryOp::Greater: return "gt";
    case BinaryOp::GreaterEqual: return "ge";
    case BinaryOp::LogicalAnd: return "logical_and";
    case BinaryOp::LogicalOr: return "logical_or";
  }
  return "unknown";
}

std::string_view unary_op_name(UnaryOp op) {
  switch (op) {
    case UnaryOp::Negate: return "neg";
    case UnaryOp::Plus: return "plus";
    case UnaryOp::LogicalNot: return "not";
    case UnaryOp::BitwiseNot: return "bitnot";
  }
  return "unknown";
}

std::string_view cast_kind_name(CastKind kind) {
  switch (kind) {
    case CastKind::Trunc: return "trunc";
    case CastKind::SExt: return "sext";
    case CastKind::ZExt: return "zext";
    case CastKind::SIToFP: return "sitofp";
    case CastKind::UIToFP: return "uitofp";
    case CastKind::FPToSI: return "fptosi";
    case CastKind::FPToUI: return "fptoui";
    case CastKind::FPExt: return "fpext";
    case CastKind::FPTrunc: return "fptrunc";
    case CastKind::BoolToInt: return "bool_to_int";
    case CastKind::IntToBool: return "int_to_bool";
    case CastKind::CharToInt: return "char_to_int";
    case CastKind::IntToChar: return "int_to_char";
  }
  return "unknown";
}

}  // namespace nirq::tir
