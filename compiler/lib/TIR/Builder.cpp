#include "nirqoba/TIR/Builder.h"

#include <utility>

namespace nirq {
namespace {

// Internal inconsistency codes: these are compiler bugs, not user errors.
constexpr const char* kUnresolvedSymbol = "NRQ4001";
constexpr const char* kUnsupportedExpression = "NRQ4002";

}  // namespace

std::unique_ptr<tir::Module> TirBuilder::lower(const Program& program) {
  auto module = std::make_unique<tir::Module>();
  for (const auto& function : program.functions()) {
    lower_function(*function, *module);
  }
  return module;
}

void TirBuilder::lower_function(const FunctionDecl& function, tir::Module& module) {
  auto body = std::make_unique<tir::Function>(function.name(), function.return_type());
  tir::Function* function_ir = module.add_function(std::move(body));

  scopes_.clear();
  push_scope();

  // Parameters get a slot but no incoming value: Phase 1 cannot call user
  // functions, so no value is ever bound here. Phase 3 fills this in.
  for (const Parameter& parameter : function.parameters()) {
    tir::Value slot = function_ir->allocate_value(parameter.type);
    tir::Op alloca;
    alloca.kind = tir::OpKind::Alloca;
    alloca.location = parameter.location;
    alloca.result = slot;
    alloca.slot_name = parameter.name;
    function_ir->add_operation(std::move(alloca));
    scopes_.back().emplace(parameter.name, slot);
  }

  lower_block(function.body(), *function_ir);
  pop_scope();
}

void TirBuilder::lower_block(const BlockStmt& block, tir::Function& function) {
  push_scope();
  for (const StmtPtr& statement : block.statements()) {
    lower_statement(*statement, function);
  }
  pop_scope();
}

void TirBuilder::lower_statement(const Stmt& statement, tir::Function& function) {
  switch (statement.kind()) {
    case Stmt::Kind::Block:
      lower_block(static_cast<const BlockStmt&>(statement), function);
      return;
    case Stmt::Kind::VariableDeclaration: {
      const auto& declaration = static_cast<const VariableDeclarationStmt&>(statement);
      const tir::Value initial = lower_expression(declaration.initializer(), function);
      const Type slot_type = declaration.has_annotation() ? declaration.annotation() : initial.type;

      tir::Value slot = function.allocate_value(slot_type);
      tir::Op alloca;
      alloca.kind = tir::OpKind::Alloca;
      alloca.location = declaration.location();
      alloca.result = slot;
      alloca.slot_name = declaration.name();
      function.add_operation(std::move(alloca));

      tir::Op store;
      store.kind = tir::OpKind::Store;
      store.location = declaration.location();
      store.operand = initial;
      store.lhs = slot;  // slot target
      store.result = slot;
      function.add_operation(std::move(store));

      scopes_.back().emplace(declaration.name(), slot);
      return;
    }
    case Stmt::Kind::Assignment: {
      const auto& assignment = static_cast<const AssignmentStmt&>(statement);
      const tir::Value value = lower_expression(assignment.value(), function);
      const tir::Value slot = lookup_slot(assignment.name());
      if (!slot.is_valid()) {
        diagnostics_.error(kUnresolvedSymbol, assignment.name_location(),
                           "internal: assignment target '" + assignment.name() +
                               "' has no TIR slot");
        return;
      }
      tir::Op store;
      store.kind = tir::OpKind::Store;
      store.location = assignment.location();
      store.operand = value;
      store.lhs = slot;
      store.result = slot;
      function.add_operation(std::move(store));
      return;
    }
    case Stmt::Kind::Return: {
      const auto& return_statement = static_cast<const ReturnStmt&>(statement);
      tir::Op op;
      op.kind = tir::OpKind::Return;
      op.location = return_statement.location();
      if (return_statement.has_value()) {
        op.operand = lower_expression(*return_statement.value(), function);
      }
      function.add_operation(std::move(op));
      return;
    }
    case Stmt::Kind::Expression: {
      const auto& expression_statement = static_cast<const ExprStmt&>(statement);
      lower_expression(expression_statement.expr(), function);
      return;
    }
  }
}

tir::Value TirBuilder::lower_expression(const Expr& expr, tir::Function& function) {
  switch (expr.kind()) {
    case Expr::Kind::IntegerLiteral: {
      const auto& literal = static_cast<const IntegerLiteralExpr&>(expr);
      Type lit_type = literal.type().is_valid() ? literal.type() : Type::int_type();
      tir::Value result = function.allocate_value(lit_type);
      tir::Op op;
      op.kind = tir::OpKind::ConstantInt;
      op.location = literal.location();
      op.result = result;
      op.integer_value = literal.value();
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::CharLiteral: {
      const auto& literal = static_cast<const CharLiteralExpr&>(expr);
      tir::Value result = function.allocate_value(Type::char_type());
      tir::Op op;
      op.kind = tir::OpKind::ConstantInt;
      op.location = literal.location();
      op.result = result;
      op.integer_value = literal.value();
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::BoolLiteral: {
      const auto& literal = static_cast<const BoolLiteralExpr&>(expr);
      tir::Value result = function.allocate_value(Type::bool_type());
      tir::Op op;
      op.kind = tir::OpKind::ConstantInt;
      op.location = literal.location();
      op.result = result;
      op.integer_value = literal.value() ? 1 : 0;
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::StringLiteral: {
      const auto& literal = static_cast<const StringLiteralExpr&>(expr);
      tir::Value result = function.allocate_value(Type::string_type());
      tir::Op op;
      op.kind = tir::OpKind::ConstantString;
      op.location = literal.location();
      op.result = result;
      op.string_value = literal.value();
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::Name: {
      const auto& name = static_cast<const NameExpr&>(expr);
      const tir::Value slot = lookup_slot(name.name());
      if (!slot.is_valid()) {
        diagnostics_.error(kUnresolvedSymbol, name.location(),
                           "internal: variable '" + name.name() + "' has no TIR slot");
        return tir::Value{};
      }
      tir::Value result = function.allocate_value(slot.type);
      tir::Op op;
      op.kind = tir::OpKind::Load;
      op.location = name.location();
      op.result = result;
      op.lhs = slot;
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      const tir::Value operand = lower_expression(unary.operand(), function);
      // Negation lowers to 0 - x rather than a dedicated op: Phase 1 has no
      // negative constant folding, and sub is the operation LLVM already has.
      tir::Value zero = function.allocate_value(Type::int_type());
      tir::Op constant;
      constant.kind = tir::OpKind::ConstantInt;
      constant.location = unary.location();
      constant.result = zero;
      constant.integer_value = 0;
      function.add_operation(std::move(constant));

      tir::Value result = function.allocate_value(Type::int_type());
      tir::Op op;
      op.kind = tir::OpKind::Binary;
      op.location = unary.location();
      op.result = result;
      op.lhs = zero;
      op.rhs = operand;
      op.binary_op = tir::BinaryOp::Subtract;
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(expr);
      const tir::Value lhs = lower_expression(binary.lhs(), function);
      const tir::Value rhs = lower_expression(binary.rhs(), function);
      tir::BinaryOp op_kind = tir::BinaryOp::Add;
      switch (binary.op()) {
        case BinaryOp::Add: op_kind = tir::BinaryOp::Add; break;
        case BinaryOp::Subtract: op_kind = tir::BinaryOp::Subtract; break;
        case BinaryOp::Multiply: op_kind = tir::BinaryOp::Multiply; break;
        case BinaryOp::Divide: op_kind = tir::BinaryOp::Divide; break;
        case BinaryOp::Remainder: op_kind = tir::BinaryOp::Remainder; break;
      }
      tir::Value result = function.allocate_value(Type::int_type());
      tir::Op op;
      op.kind = tir::OpKind::Binary;
      op.location = binary.location();
      op.result = result;
      op.lhs = lhs;
      op.rhs = rhs;
      op.binary_op = op_kind;
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::Call: {
      const auto& call = static_cast<const CallExpr&>(expr);
      if (call.callee() != "print") {
        diagnostics_.error(kUnsupportedExpression, call.location(),
                           "internal: TIR lowering received an unlowered call to '" +
                               call.callee() + "'");
        return tir::Value{};
      }
      if (call.arguments().size() != 1) return tir::Value{};  // sema already errored
      const Expr& argument = *call.arguments().front();
      if (argument.kind() != Expr::Kind::StringLiteral) {
        // A string variable is a value, not an attribute: printing one needs the
        // formatting machinery of Phase 2. Say so instead of mis-lowering it.
        diagnostics_.error(diag::kNotImplementedInPhase1, argument.location(),
                           "'print' of a string value is specified for Phase 2; Phase 1 "
                           "prints string literals only");
        return tir::Value{};
      }
      tir::Op op;
      op.kind = tir::OpKind::PrintString;
      op.location = call.location();
      op.string_value = static_cast<const StringLiteralExpr&>(argument).value();
      function.add_operation(std::move(op));
      return tir::Value{};
    }
  }
  return tir::Value{};
}

tir::Value TirBuilder::lookup_slot(const std::string& name) const {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    const auto found = scope->find(name);
    if (found != scope->end()) return found->second;
  }
  return tir::Value{};
}

}  // namespace nirq
