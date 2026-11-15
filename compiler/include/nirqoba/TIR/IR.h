// Nirqoba typed IR (TIR): the typed, target-independent form Phase 1, 2 & 3 lowers
// from the AST and then translates into the `nrq` and standard MLIR dialects.
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
  ValueOwnership ownership = ValueOwnership::Trivial;

  bool is_valid() const { return id >= 0; }
};

// Why an owned value moved. The reason is metadata, not a runtime action: a
// move is always a change of responsible owner, never an implicit copy.
enum class TransferKind {
  Local,       // initializing or assigning a local binding
  Return,      // returning ownership to the caller
  Argument,    // passing ownership to a callee
  Field,       // moving an owned field out of an aggregate
  Element,     // moving an owned element out of a container
};

// How a value's lifetime is bound. `Lexical` covers a borrow that ends with its
// block; `Parameter` binds a borrow to a callee parameter that outlives the
// call; `Return` binds a returned borrow to a caller-supplied source.
enum class LifetimeKind {
  None,
  Lexical,
  Parameter,
  Return,
};

// A lifetime relationship preserved in TIR. `borrow` is the non-owning value
// and `source` is the owner it must not outlive. Returned borrows name the
// source parameter instead of a local value.
struct LifetimeRecord {
  Value borrow;
  Value source;
  LifetimeKind kind = LifetimeKind::None;
  std::string source_parameter;
  std::string field_path;
  bool mutable_access = false;
  SourceLocation location;
};

// A destruction obligation registered when an owning value is introduced. TIR
// represents the obligation separately from the release call so verification can
// reject a missing, duplicated or wrongly-typed discharge.
struct DestructionObligation {
  int id = -1;
  Value owner;
  Type type;
  std::string release;
  std::string resource_domain;
  SourceLocation location;
  bool discharged = false;
  bool transferred = false;
};

enum class OpKind {
  ConstantInt,       // result = integer_value
  ConstantNull,      // result = null/zero value of result.type
  ConstantFloat,     // result = float_value
  ConstantString,    // result = pointer to string_value
  Alloca,            // result = slot for slot_name of result.type
  Store,             // store operand into slot (lhs)
  Load,              // result = load from slot (lhs)
  Unary,             // result = <op> operand
  Binary,            // result = lhs <op> rhs
  Cast,              // result = cast(operand) with cast_kind
  Call,              // result = callee(arguments...)
  PrintString,       // write string_value to stdout through the runtime
  Print,             // write operand to stdout through the runtime
  Label,             // basic block label: label_id
  Branch,            // unconditional branch to label_id
  BranchCond,        // conditional branch: if operand then true_label else false_label
  Return,            // function exit, optional operand
  // Phase 4 aggregate operations:
  ArrayAlloca,       // alloca for a fixed-size array: result = ptr to array[length] of elem_type
  GetElementPtr,     // result = ptr to element: base_ptr + index (array) or field_index (struct/tuple)
  SoaColumnPtr,      // result = ptr to one column element: lhs = container, operand = row index, field_index = field
  AddressOf,         // result = address of a stack slot: operand names the slot
  AggregatePack,     // pack multiple operands into an aggregate (tuple/struct) slot
  AggregateExtract,  // result = extract field_index from aggregate operand
  // Phase 5 sequence operations:
  Length,            // result = element count of operand (string or dynamic array)
  DynamicIndex,      // result = element lhs[operand] of a dynamic array
  // Phase 1 ownership operations. These carry ownership semantics in TIR and
  // lower to the same machine code as the operations they annotate.
  Move,              // result = tir.move operand; operand is no longer usable
  Borrow,            // operand = borrowed value, lhs = owner; no runtime action
  EndBorrow,         // operand = borrow whose lexical lifetime ended
  Drop,              // operand = owning value whose obligation is discharged
  Unreachable,       // marks a block control never reaches (emitted at a dead merge point)
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
  std::string callee;
  std::vector<Value> arguments;

  int label_id = -1;
  int true_label = -1;
  int false_label = -1;
  int field_index = -1;           // GetElementPtr / AggregateExtract: field/element index
  std::vector<Value> aggregate_elements;  // AggregatePack: values to pack

  // Ownership metadata carried by Move/Borrow/Drop operations.
  TransferKind transfer_kind = TransferKind::Local;
  bool mutable_borrow = false;    // Borrow: exclusive access
  std::string borrow_path;        // Borrow: field path or empty for the whole value
  int obligation_id = -1;         // Drop: the obligation being discharged
};

class Function {
 public:
  struct Parameter {
    std::string name;
    Type type;
    Value value;
    ValueOwnership ownership = ValueOwnership::Trivial;
    std::string resource_domain;
  };

  Function(std::string name, Type return_type) : name_(std::move(name)), return_type_(return_type) {}

  const std::string& name() const { return name_; }
  Type return_type() const { return return_type_; }
  ValueOwnership return_ownership() const { return return_ownership_; }
  void set_return_ownership(ValueOwnership ownership) { return_ownership_ = ownership; }
  const std::string& return_resource_domain() const { return return_resource_domain_; }
  void set_return_resource_domain(std::string domain) {
    return_resource_domain_ = std::move(domain);
  }
  const std::string& return_borrow_parameter() const {
    return return_borrow_parameter_;
  }
  void set_return_borrow_parameter(std::string parameter) {
    return_borrow_parameter_ = std::move(parameter);
  }

  // Lifetime relationships discovered while lowering. A borrow is always
  // recorded here as well as in the Borrow operation, so transformations can
  // query the relationship without scanning the operation stream.
  std::vector<LifetimeRecord>& lifetimes() { return lifetimes_; }
  const std::vector<LifetimeRecord>& lifetimes() const { return lifetimes_; }
  void add_lifetime(LifetimeRecord record) {
    lifetimes_.push_back(std::move(record));
  }

  // Destruction obligations. The id is stable for the function and is what a
  // Drop operation names; ids are never reused after a discharge.
  std::vector<DestructionObligation>& obligations() { return obligations_; }
  const std::vector<DestructionObligation>& obligations() const {
    return obligations_;
  }
  int add_obligation(Value owner, Type type, std::string release,
                     std::string resource_domain, SourceLocation location) {
    DestructionObligation obligation;
    obligation.id = next_obligation_id_++;
    obligation.owner = owner;
    obligation.type = std::move(type);
    obligation.release = std::move(release);
    obligation.resource_domain = std::move(resource_domain);
    obligation.location = location;
    obligations_.push_back(std::move(obligation));
    return obligations_.back().id;
  }
  DestructionObligation* obligation(int id) {
    for (DestructionObligation& obligation : obligations_)
      if (obligation.id == id) return &obligation;
    return nullptr;
  }
  const DestructionObligation* obligation(int id) const {
    for (const DestructionObligation& obligation : obligations_)
      if (obligation.id == id) return &obligation;
    return nullptr;
  }
  // First obligation for an owner slot that has not been discharged. Used when
  // a cleanup is emitted: the builder does not need to thread ids through the
  // deferred-call stack, only the slot.
  DestructionObligation* pending_obligation_for_owner(Value owner) {
    for (DestructionObligation& obligation : obligations_)
      if (!obligation.discharged && !obligation.transferred &&
          obligation.owner.id == owner.id)
        return &obligation;
    return nullptr;
  }

  std::vector<Op>& operations() { return operations_; }
  const std::vector<Op>& operations() const { return operations_; }

  const std::vector<Parameter>& parameters() const { return parameters_; }
  void add_parameter(std::string name, Type type, Value value,
                     ValueOwnership ownership = ValueOwnership::Trivial,
                     std::string resource_domain = {}) {
    parameters_.push_back(Parameter{std::move(name), type, value, ownership,
                                    std::move(resource_domain)});
  }

  // Reserves the next value number. Types are recorded so the verifier can
  // check operand types without re-deriving them.
  Value allocate_value(Type type,
                       ValueOwnership ownership = ValueOwnership::Trivial);
  int allocate_label() { return next_label_id_++; }

  void add_operation(Op op) { operations_.push_back(std::move(op)); }

  std::size_t value_count() const { return types_.size(); }
  Type value_type(int id) const { return types_[static_cast<std::size_t>(id)]; }
  ValueOwnership value_ownership(int id) const {
    return ownerships_[static_cast<std::size_t>(id)];
  }

 private:
  std::string name_;
  Type return_type_;
  ValueOwnership return_ownership_ = ValueOwnership::Trivial;
  std::string return_resource_domain_;
  std::string return_borrow_parameter_;
  std::vector<Parameter> parameters_;
  std::vector<Op> operations_;
  std::vector<Type> types_;
  std::vector<ValueOwnership> ownerships_;
  std::vector<LifetimeRecord> lifetimes_;
  std::vector<DestructionObligation> obligations_;
  int next_label_id_ = 0;
  int next_obligation_id_ = 0;
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
