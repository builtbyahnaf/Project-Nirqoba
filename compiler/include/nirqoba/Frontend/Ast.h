// Abstract Syntax Tree for Nirqoba Phase 1, 2, 3 & 4.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nirqoba/Common/Type.h"
#include "nirqoba/Diagnostics/SourceLocation.h"

namespace nirq {

// ----------------------------------------------------------------- expressions

class Expr {
 public:
  enum class Kind {
    IntegerLiteral,
    FloatLiteral,
    CharLiteral,
    StringLiteral,
    BoolLiteral,
    ArrayLiteral,
    TupleLiteral,
    Index,
    MemberAccess,
    StructInit,
    Name,
    Call,
    Unary,
    Binary,
    Cast
  };

  Expr(Kind kind, SourceLocation location) : kind_(kind), location_(std::move(location)) {}
  virtual ~Expr() = default;

  Kind kind() const { return kind_; }
  const SourceLocation& location() const { return location_; }

  Type type() const { return type_; }
  void set_type(Type type) { type_ = type; }

 private:
  Kind kind_;
  SourceLocation location_;
  Type type_;
};

using ExprPtr = std::unique_ptr<Expr>;

class IntegerLiteralExpr : public Expr {
 public:
  IntegerLiteralExpr(SourceLocation location, long long value, std::string suffix = "")
      : Expr(Kind::IntegerLiteral, std::move(location)), value_(value), suffix_(std::move(suffix)) {}
  long long value() const { return value_; }
  const std::string& suffix() const { return suffix_; }

 private:
  long long value_;
  std::string suffix_;
};

class FloatLiteralExpr : public Expr {
 public:
  FloatLiteralExpr(SourceLocation location, double value, std::string suffix = "")
      : Expr(Kind::FloatLiteral, std::move(location)), value_(value), suffix_(std::move(suffix)) {}
  double value() const { return value_; }
  const std::string& suffix() const { return suffix_; }

 private:
  double value_;
  std::string suffix_;
};

class CharLiteralExpr : public Expr {
 public:
  CharLiteralExpr(SourceLocation location, uint32_t value)
      : Expr(Kind::CharLiteral, std::move(location)), value_(value) {}
  uint32_t value() const { return value_; }

 private:
  uint32_t value_;
};

class StringLiteralExpr : public Expr {
 public:
  StringLiteralExpr(SourceLocation location, std::string value)
      : Expr(Kind::StringLiteral, std::move(location)), value_(std::move(value)) {}
  const std::string& value() const { return value_; }

 private:
  std::string value_;
};

class BoolLiteralExpr : public Expr {
 public:
  BoolLiteralExpr(SourceLocation location, bool value)
      : Expr(Kind::BoolLiteral, std::move(location)), value_(value) {}
  bool value() const { return value_; }

 private:
  bool value_;
};

class ArrayLiteralExpr : public Expr {
 public:
  ArrayLiteralExpr(SourceLocation location, std::vector<ExprPtr> elements)
      : Expr(Kind::ArrayLiteral, std::move(location)), elements_(std::move(elements)) {}
  const std::vector<ExprPtr>& elements() const { return elements_; }

 private:
  std::vector<ExprPtr> elements_;
};

class TupleLiteralExpr : public Expr {
 public:
  TupleLiteralExpr(SourceLocation location, std::vector<ExprPtr> elements)
      : Expr(Kind::TupleLiteral, std::move(location)), elements_(std::move(elements)) {}
  const std::vector<ExprPtr>& elements() const { return elements_; }

 private:
  std::vector<ExprPtr> elements_;
};

class IndexExpr : public Expr {
 public:
  IndexExpr(SourceLocation location, ExprPtr base, ExprPtr index)
      : Expr(Kind::Index, std::move(location)), base_(std::move(base)), index_(std::move(index)) {}
  const Expr& base() const { return *base_; }
  const Expr& index() const { return *index_; }

 private:
  ExprPtr base_;
  ExprPtr index_;
};

class MemberAccessExpr : public Expr {
 public:
  MemberAccessExpr(SourceLocation location, ExprPtr base, std::string member, int tuple_index = -1)
      : Expr(Kind::MemberAccess, std::move(location)), base_(std::move(base)),
        member_(std::move(member)), tuple_index_(tuple_index) {}
  const Expr& base() const { return *base_; }
  const std::string& member() const { return member_; }
  bool is_tuple_access() const { return tuple_index_ >= 0; }
  int tuple_index() const { return tuple_index_; }

 private:
  ExprPtr base_;
  std::string member_;
  int tuple_index_;
};

class StructInitExpr : public Expr {
 public:
  StructInitExpr(SourceLocation location, std::string struct_name,
                 std::vector<std::pair<std::string, ExprPtr>> field_inits)
      : Expr(Kind::StructInit, std::move(location)), struct_name_(std::move(struct_name)),
        field_inits_(std::move(field_inits)), is_positional_(false) {}

  StructInitExpr(SourceLocation location, std::string struct_name,
                 std::vector<ExprPtr> positional_inits)
      : Expr(Kind::StructInit, std::move(location)), struct_name_(std::move(struct_name)),
        positional_inits_(std::move(positional_inits)), is_positional_(true) {}

  const std::string& struct_name() const { return struct_name_; }
  bool is_positional() const { return is_positional_; }
  const std::vector<std::pair<std::string, ExprPtr>>& field_inits() const { return field_inits_; }
  const std::vector<ExprPtr>& positional_inits() const { return positional_inits_; }

 private:
  std::string struct_name_;
  std::vector<std::pair<std::string, ExprPtr>> field_inits_;
  std::vector<ExprPtr> positional_inits_;
  bool is_positional_;
};

class NameExpr : public Expr {
 public:
  NameExpr(SourceLocation location, std::string name)
      : Expr(Kind::Name, std::move(location)), name_(std::move(name)) {}
  const std::string& name() const { return name_; }

 private:
  std::string name_;
};

class CallExpr : public Expr {
 public:
  CallExpr(SourceLocation location, std::string callee, std::vector<ExprPtr> arguments)
      : Expr(Kind::Call, std::move(location)), callee_(std::move(callee)),
        arguments_(std::move(arguments)) {}
  const std::string& callee() const { return callee_; }
  const std::vector<ExprPtr>& arguments() const { return arguments_; }

 private:
  std::string callee_;
  std::vector<ExprPtr> arguments_;
};

enum class UnaryOp { Negate, Plus, LogicalNot, BitwiseNot };
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
  LogicalOr
};

std::string_view unary_op_spelling(UnaryOp op);
std::string_view binary_op_spelling(BinaryOp op);

class UnaryExpr : public Expr {
 public:
  UnaryExpr(SourceLocation location, UnaryOp op, ExprPtr operand)
      : Expr(Kind::Unary, std::move(location)), op_(op), operand_(std::move(operand)) {}
  UnaryOp op() const { return op_; }
  const Expr& operand() const { return *operand_; }

 private:
  UnaryOp op_;
  ExprPtr operand_;
};

class BinaryExpr : public Expr {
 public:
  BinaryExpr(SourceLocation location, BinaryOp op, ExprPtr lhs, ExprPtr rhs)
      : Expr(Kind::Binary, std::move(location)), op_(op), lhs_(std::move(lhs)),
        rhs_(std::move(rhs)) {}
  BinaryOp op() const { return op_; }
  const Expr& lhs() const { return *lhs_; }
  const Expr& rhs() const { return *rhs_; }

 private:
  BinaryOp op_;
  ExprPtr lhs_;
  ExprPtr rhs_;
};

class CastExpr : public Expr {
 public:
  CastExpr(SourceLocation location, ExprPtr operand, Type target_type)
      : Expr(Kind::Cast, std::move(location)), operand_(std::move(operand)), target_type_(target_type) {}
  const Expr& operand() const { return *operand_; }
  Type target_type() const { return target_type_; }

 private:
  ExprPtr operand_;
  Type target_type_;
};

// ----------------------------------------------------------------- statements

class Stmt {
 public:
  enum class Kind {
    Block,
    VariableDeclaration,
    Assignment,
    CompoundAssignment,
    Destructuring,
    If,
    While,
    For,
    Break,
    Continue,
    Return,
    Expression
  };

  Stmt(Kind kind, SourceLocation location) : kind_(kind), location_(std::move(location)) {}
  virtual ~Stmt() = default;

  Kind kind() const { return kind_; }
  const SourceLocation& location() const { return location_; }

 private:
  Kind kind_;
  SourceLocation location_;
};

using StmtPtr = std::unique_ptr<Stmt>;

class BlockStmt : public Stmt {
 public:
  explicit BlockStmt(SourceLocation location) : Stmt(Kind::Block, std::move(location)) {}
  std::vector<StmtPtr>& statements() { return statements_; }
  const std::vector<StmtPtr>& statements() const { return statements_; }

 private:
  std::vector<StmtPtr> statements_;
};

enum class BindingKind { Let, Var, Const };
std::string_view binding_kind_spelling(BindingKind kind);

class VariableDeclarationStmt : public Stmt {
 public:
  VariableDeclarationStmt(SourceLocation location, BindingKind binding, std::string name,
                          bool has_annotation, Type annotation, ExprPtr initializer)
      : Stmt(Kind::VariableDeclaration, std::move(location)), binding_(binding),
        name_(std::move(name)), has_annotation_(has_annotation), annotation_(annotation),
        initializer_(std::move(initializer)) {}
  BindingKind binding() const { return binding_; }
  const std::string& name() const { return name_; }
  bool has_annotation() const { return has_annotation_; }
  Type annotation() const { return annotation_; }
  const Expr& initializer() const { return *initializer_; }

 private:
  BindingKind binding_;
  std::string name_;
  bool has_annotation_;
  Type annotation_;
  ExprPtr initializer_;
};

class DestructuringStmt : public Stmt {
 public:
  DestructuringStmt(SourceLocation location, BindingKind binding, std::vector<std::string> names,
                    ExprPtr value)
      : Stmt(Kind::Destructuring, std::move(location)), binding_(binding),
        names_(std::move(names)), value_(std::move(value)) {}
  BindingKind binding() const { return binding_; }
  const std::vector<std::string>& names() const { return names_; }
  const Expr& value() const { return *value_; }

 private:
  BindingKind binding_;
  std::vector<std::string> names_;
  ExprPtr value_;
};

class AssignmentStmt : public Stmt {
 public:
  AssignmentStmt(SourceLocation location, ExprPtr target, ExprPtr value)
      : Stmt(Kind::Assignment, std::move(location)), target_(std::move(target)),
        value_(std::move(value)) {}

  AssignmentStmt(SourceLocation location, std::string name, ExprPtr value, SourceLocation name_location)
      : Stmt(Kind::Assignment, std::move(location)),
        target_(std::make_unique<NameExpr>(name_location, std::move(name))),
        value_(std::move(value)) {}

  const Expr& target() const { return *target_; }
  Expr& target() { return *target_; }
  const Expr& value() const { return *value_; }
  Expr& value() { return *value_; }

  bool is_simple_name() const { return target_ != nullptr && target_->kind() == Expr::Kind::Name; }
  const std::string& name() const {
    return static_cast<const NameExpr&>(*target_).name();
  }
  const SourceLocation& name_location() const {
    return target_->location();
  }

 private:
  ExprPtr target_;
  ExprPtr value_;
};

class CompoundAssignmentStmt : public Stmt {
 public:
  CompoundAssignmentStmt(SourceLocation location, BinaryOp op, ExprPtr target, ExprPtr value)
      : Stmt(Kind::CompoundAssignment, std::move(location)), op_(op),
        target_(std::move(target)), value_(std::move(value)) {}

  CompoundAssignmentStmt(SourceLocation location, BinaryOp op, std::string name, ExprPtr value, SourceLocation name_location)
      : Stmt(Kind::CompoundAssignment, std::move(location)), op_(op),
        target_(std::make_unique<NameExpr>(name_location, std::move(name))),
        value_(std::move(value)) {}

  BinaryOp op() const { return op_; }
  const Expr& target() const { return *target_; }
  Expr& target() { return *target_; }
  const Expr& value() const { return *value_; }
  Expr& value() { return *value_; }

  bool is_simple_name() const { return target_ != nullptr && target_->kind() == Expr::Kind::Name; }
  const std::string& name() const {
    return static_cast<const NameExpr&>(*target_).name();
  }
  const SourceLocation& name_location() const {
    return target_->location();
  }

 private:
  BinaryOp op_;
  ExprPtr target_;
  ExprPtr value_;
};

class IfStmt : public Stmt {
 public:
  IfStmt(SourceLocation location, ExprPtr condition, std::unique_ptr<BlockStmt> then_branch, StmtPtr else_branch)
      : Stmt(Kind::If, std::move(location)), condition_(std::move(condition)),
        then_branch_(std::move(then_branch)), else_branch_(std::move(else_branch)) {}
  const Expr& condition() const { return *condition_; }
  const BlockStmt& then_branch() const { return *then_branch_; }
  bool has_else() const { return else_branch_ != nullptr; }
  const Stmt* else_branch() const { return else_branch_.get(); }

 private:
  ExprPtr condition_;
  std::unique_ptr<BlockStmt> then_branch_;
  StmtPtr else_branch_;
};

class WhileStmt : public Stmt {
 public:
  WhileStmt(SourceLocation location, ExprPtr condition, std::unique_ptr<BlockStmt> body)
      : Stmt(Kind::While, std::move(location)), condition_(std::move(condition)), body_(std::move(body)) {}
  const Expr& condition() const { return *condition_; }
  const BlockStmt& body() const { return *body_; }

 private:
  ExprPtr condition_;
  std::unique_ptr<BlockStmt> body_;
};

class ForStmt : public Stmt {
 public:
  ForStmt(SourceLocation location, StmtPtr initializer, ExprPtr condition, StmtPtr step, std::unique_ptr<BlockStmt> body)
      : Stmt(Kind::For, std::move(location)), initializer_(std::move(initializer)),
        condition_(std::move(condition)), step_(std::move(step)), body_(std::move(body)) {}
  bool has_initializer() const { return initializer_ != nullptr; }
  const Stmt* initializer() const { return initializer_.get(); }
  bool has_condition() const { return condition_ != nullptr; }
  const Expr* condition() const { return condition_.get(); }
  bool has_step() const { return step_ != nullptr; }
  const Stmt* step() const { return step_.get(); }
  const BlockStmt& body() const { return *body_; }

 private:
  StmtPtr initializer_;
  ExprPtr condition_;
  StmtPtr step_;
  std::unique_ptr<BlockStmt> body_;
};

class BreakStmt : public Stmt {
 public:
  explicit BreakStmt(SourceLocation location) : Stmt(Kind::Break, std::move(location)) {}
};

class ContinueStmt : public Stmt {
 public:
  explicit ContinueStmt(SourceLocation location) : Stmt(Kind::Continue, std::move(location)) {}
};

class ReturnStmt : public Stmt {
 public:
  ReturnStmt(SourceLocation location, ExprPtr value)
      : Stmt(Kind::Return, std::move(location)), value_(std::move(value)) {}
  bool has_value() const { return value_ != nullptr; }
  const Expr* value() const { return value_.get(); }

 private:
  ExprPtr value_;
};

class ExprStmt : public Stmt {
 public:
  ExprStmt(SourceLocation location, ExprPtr expr)
      : Stmt(Kind::Expression, std::move(location)), expr_(std::move(expr)) {}
  const Expr& expr() const { return *expr_; }

 private:
  ExprPtr expr_;
};

// ---------------------------------------------------------------- declarations

struct Parameter {
  std::string name;
  Type type;
  SourceLocation location;
};

class StructDecl {
 public:
  StructDecl(std::string name, std::vector<Parameter> fields, SourceLocation location)
      : name_(std::move(name)), fields_(std::move(fields)), location_(std::move(location)) {}

  const std::string& name() const { return name_; }
  const std::vector<Parameter>& fields() const { return fields_; }
  const SourceLocation& location() const { return location_; }

 private:
  std::string name_;
  std::vector<Parameter> fields_;
  SourceLocation location_;
};

class FunctionDecl {
 public:
  FunctionDecl(Type return_type, std::string name, std::vector<Parameter> parameters,
               std::unique_ptr<BlockStmt> body, SourceLocation location)
      : return_type_(return_type), name_(std::move(name)),
        parameters_(std::move(parameters)), body_(std::move(body)),
        location_(std::move(location)) {}

  Type return_type() const { return return_type_; }
  const std::string& name() const { return name_; }
  const std::vector<Parameter>& parameters() const { return parameters_; }
  const BlockStmt& body() const { return *body_; }
  const SourceLocation& location() const { return location_; }

 private:
  Type return_type_;
  std::string name_;
  std::vector<Parameter> parameters_;
  std::unique_ptr<BlockStmt> body_;
  SourceLocation location_;
};

class Program {
 public:
  std::vector<std::unique_ptr<StructDecl>>& structs() { return structs_; }
  const std::vector<std::unique_ptr<StructDecl>>& structs() const { return structs_; }

  std::vector<std::unique_ptr<FunctionDecl>>& functions() { return functions_; }
  const std::vector<std::unique_ptr<FunctionDecl>>& functions() const { return functions_; }

 private:
  std::vector<std::unique_ptr<StructDecl>> structs_;
  std::vector<std::unique_ptr<FunctionDecl>> functions_;
};

}  // namespace nirq
