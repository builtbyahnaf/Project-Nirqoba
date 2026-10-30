// Nirqoba typed IR (TIR): the typed, target-independent form Phase 1 & 2 lowers
// from the AST and then translates into the `nrq` MLIR dialect.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "nirqoba/Common/Type.h"
#include "nirqoba/Diagnostics/SourceLocation.h"

namespace nirq::tir {

// Handle to a value produced by an operation. `id` indexes into the function's
// value numbering; -1 means "no value" (for example a void call).
struct Value {
  int id = -1;
  Type type;

  bool is_valid() const { return id >= 0; }
};

enum class OpKind {
  ConstantInt,     // result = integer_value
  ConstantFloat,   // result = float_value
  ConstantString,  // result = pointer to string_value
  Alloca,          // result = slot for slot_name of result.type
  Store,           // store operand into slot (lhs)
  Load,            // result = load from slot (lhs)
  Unary,           // result = <op> operand
  Binary,          // result = lhs <op> rhs
  Cast,            // result = cast(operand) with cast_kind
  PrintString,     // write string_value to stdout through the runtime
  Print,           // write operand to stdout through the runtime
  Label,           // basic block label: label_id
  Branch,          // unconditional branch to label_id
  BranchCond,      // conditional branch: if operand then true_label else false_label
  Return,          // function exit, optional operand
};

enum class UnaryOp {
  Negate,
  Plus,
  LogicalNot,
  BitwiseNot,
};

enum class BinaryOp {
  Add,
  Subtract,
  Multiply,
  Divide,
  Remainder,
  Exponentiate,
  BitwiseAnd,
  BitwiseOr,
  BitwiseXor,
  ShiftLeft,
  ShiftRight,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  LogicalAnd,
  LogicalOr,
};

enum class CastKind {
  Trunc,
  SExt,
  ZExt,
  SIToFP,
  UIToFP,
  FPToSI,
  FPToUI,
  FPExt,
  FPTrunc,
  BoolToInt,
  IntToBool,
  CharToInt,
  IntToChar,
};

std::string_view op_name(OpKind kind);
std::string_view binary_op_name(BinaryOp op);
std::string_view unary_op_name(UnaryOp op);
std::string_view cast_kind_name(CastKind kind);

struct Op {
  OpKind kind = OpKind::Return;
  SourceLocation location;

  Value result;
  Value operand;
  Value lhs;
  Value rhs;

  BinaryOp binary_op = BinaryOp::Add;
  UnaryOp unary_op = UnaryOp::Negate;
  CastKind cast_kind = CastKind::Trunc;

  long long integer_value = 0;
  double float_value = 0.0;
  std::string string_value;
  std::string slot_name;

  int label_id = -1;
  int true_label = -1;
  int false_label = -1;
};

class Function {
 public:
  Function(std::string name, Type return_type) : name_(std::move(name)), return_type_(return_type) {}

  const std::string& name() const { return name_; }
  Type return_type() const { return return_type_; }

  std::vector<Op>& operations() { return operations_; }
  const std::vector<Op>& operations() const { return operations_; }

  // Reserves the next value number. Types are recorded so the verifier can
  // check operand types without re-deriving them.
  Value allocate_value(Type type);
  int allocate_label() { return next_label_id_++; }

  void add_operation(Op op) { operations_.push_back(std::move(op)); }

  std::size_t value_count() const { return types_.size(); }
  Type value_type(int id) const { return types_[static_cast<std::size_t>(id)]; }

 private:
  std::string name_;
  Type return_type_;
  std::vector<Op> operations_;
  std::vector<Type> types_;
  int next_label_id_ = 0;
};

class Module {
 public:
  Function* add_function(std::unique_ptr<Function> function) {
    functions_.push_back(std::move(function));
    return functions_.back().get();
  }

  std::vector<std::unique_ptr<Function>>& functions() { return functions_; }
  const std::vector<std::unique_ptr<Function>>& functions() const { return functions_; }

 private:
  std::vector<std::unique_ptr<Function>> functions_;
};

}  // namespace nirq::tir
