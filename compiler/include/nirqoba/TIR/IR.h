// Nirqoba typed IR (TIR): the typed, target-independent form Phase 1 lowers
// from the AST and then translates into the `nrq` MLIR dialect.
//
// Shape: one flat list of operations per function, SSA-style value handles, and
// explicit stack slots for every source variable. That is the smallest form
// that (a) keeps every value's type known, (b) gives the MLIR translation a
// direct mapping, and (c) leaves a place for Phase 2's arith and control-flow
// operations to slot in without redesigning the container.
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
  ConstantInt,   // result = integer_value
  ConstantString,  // result = pointer to string_value
  Alloca,        // result = slot for slot_name of result.type
  Store,         // store operand into slot
  Load,          // result = load from slot
  Binary,        // result = lhs <op> rhs
  PrintString,   // write string_value to stdout through the runtime
  Return,        // function exit, optional operand
};

enum class BinaryOp { Add, Subtract, Multiply, Divide, Remainder };

std::string_view op_name(OpKind kind);
std::string_view binary_op_name(BinaryOp op);

struct Op {
  OpKind kind = OpKind::Return;
  SourceLocation location;

  // Slot handling is spelled with `lhs` so no extra field is needed:
  //   Alloca  -> result is the slot
  //   Store   -> lhs is the target slot, operand is the value
  //   Load    -> lhs is the source slot, result is the loaded value
  Value result;   // ConstantInt, ConstantString, Alloca, Load, Binary
  Value operand;  // Store (value), Return (return value)
  Value lhs;      // Binary left operand, or the slot of a Load/Store
  Value rhs;      // Binary right operand

  BinaryOp binary_op = BinaryOp::Add;
  long long integer_value = 0;
  std::string string_value;  // ConstantString, PrintString (literal text)
  std::string slot_name;     // Alloca
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

  void add_operation(Op op) { operations_.push_back(std::move(op)); }

  std::size_t value_count() const { return types_.size(); }
  Type value_type(int id) const { return types_[static_cast<std::size_t>(id)]; }

 private:
  std::string name_;
  Type return_type_;
  std::vector<Op> operations_;
  std::vector<Type> types_;
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
