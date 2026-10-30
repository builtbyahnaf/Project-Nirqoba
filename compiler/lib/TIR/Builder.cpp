#include "nirqoba/TIR/Builder.h"

#include <utility>

namespace nirq {
namespace {

constexpr const char* kUnresolvedSymbol = "NRQ4001";
constexpr const char* kUnsupportedExpression = "NRQ4002";

tir::BinaryOp map_binary_op(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add: return tir::BinaryOp::Add;
    case BinaryOp::Subtract: return tir::BinaryOp::Subtract;
    case BinaryOp::Multiply: return tir::BinaryOp::Multiply;
    case BinaryOp::Divide: return tir::BinaryOp::Divide;
    case BinaryOp::Remainder: return tir::BinaryOp::Remainder;
    case BinaryOp::Exponentiate: return tir::BinaryOp::Exponentiate;
    case BinaryOp::BitwiseAnd: return tir::BinaryOp::BitwiseAnd;
    case BinaryOp::BitwiseOr: return tir::BinaryOp::BitwiseOr;
    case BinaryOp::BitwiseXor: return tir::BinaryOp::BitwiseXor;
    case BinaryOp::ShiftLeft: return tir::BinaryOp::ShiftLeft;
    case BinaryOp::ShiftRight: return tir::BinaryOp::ShiftRight;
    case BinaryOp::Equal: return tir::BinaryOp::Equal;
    case BinaryOp::NotEqual: return tir::BinaryOp::NotEqual;
    case BinaryOp::Less: return tir::BinaryOp::Less;
    case BinaryOp::LessEqual: return tir::BinaryOp::LessEqual;
    case BinaryOp::Greater: return tir::BinaryOp::Greater;
    case BinaryOp::GreaterEqual: return tir::BinaryOp::GreaterEqual;
    case BinaryOp::LogicalAnd: return tir::BinaryOp::LogicalAnd;
    case BinaryOp::LogicalOr: return tir::BinaryOp::LogicalOr;
  }
  return tir::BinaryOp::Add;
}

tir::CastKind determine_cast_kind(Type src, Type dst) {
  if (src.is_integer() && dst.is_integer()) {
    if (dst.bit_width() < src.bit_width()) return tir::CastKind::Trunc;
    if (src.is_signed_integer()) return tir::CastKind::SExt;
    return tir::CastKind::ZExt;
  }
  if (src.is_integer() && dst.is_float()) {
    return src.is_signed_integer() ? tir::CastKind::SIToFP : tir::CastKind::UIToFP;
  }
  if (src.is_float() && dst.is_integer()) {
    return dst.is_signed_integer() ? tir::CastKind::FPToSI : tir::CastKind::FPToUI;
  }
  if (src.is_float() && dst.is_float()) {
    return dst.bit_width() > src.bit_width() ? tir::CastKind::FPExt : tir::CastKind::FPTrunc;
  }
  if (src.is_bool() && dst.is_integer()) return tir::CastKind::BoolToInt;
  if (src.is_integer() && dst.is_bool()) return tir::CastKind::IntToBool;
  if (src.is_char() && dst.is_integer()) return tir::CastKind::CharToInt;
  if (src.is_integer() && dst.is_char()) return tir::CastKind::IntToChar;
  return tir::CastKind::Trunc;
}

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
      store.lhs = slot;
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
    case Stmt::Kind::CompoundAssignment: {
      const auto& assignment = static_cast<const CompoundAssignmentStmt&>(statement);
      const tir::Value slot = lookup_slot(assignment.name());
      if (!slot.is_valid()) {
        diagnostics_.error(kUnresolvedSymbol, assignment.name_location(),
                           "internal: compound assignment target '" + assignment.name() +
                               "' has no TIR slot");
        return;
      }
      tir::Value current_val = function.allocate_value(slot.type);
      tir::Op load;
      load.kind = tir::OpKind::Load;
      load.location = assignment.name_location();
      load.lhs = slot;
      load.result = current_val;
      function.add_operation(std::move(load));

      tir::Value rhs_val = lower_expression(assignment.value(), function);
      tir::Value res_val = function.allocate_value(slot.type);
      tir::Op bin;
      bin.kind = tir::OpKind::Binary;
      bin.location = assignment.location();
      bin.binary_op = map_binary_op(assignment.op());
      bin.lhs = current_val;
      bin.rhs = rhs_val;
      bin.result = res_val;
      function.add_operation(std::move(bin));

      tir::Op store;
      store.kind = tir::OpKind::Store;
      store.location = assignment.location();
      store.operand = res_val;
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
    case Expr::Kind::FloatLiteral: {
      const auto& literal = static_cast<const FloatLiteralExpr&>(expr);
      Type lit_type = literal.type().is_valid() ? literal.type() : Type::f64_type();
      tir::Value result = function.allocate_value(lit_type);
      tir::Op op;
      op.kind = tir::OpKind::ConstantFloat;
      op.location = literal.location();
      op.result = result;
      op.float_value = literal.value();
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
    case Expr::Kind::Cast: {
      const auto& cast = static_cast<const CastExpr&>(expr);
      const tir::Value operand = lower_expression(cast.operand(), function);
      tir::CastKind cast_kind = determine_cast_kind(cast.operand().type(), cast.target_type());
      tir::Value result = function.allocate_value(cast.target_type());
      tir::Op op;
      op.kind = tir::OpKind::Cast;
      op.location = cast.location();
      op.result = result;
      op.operand = operand;
      op.cast_kind = cast_kind;
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      const tir::Value operand = lower_expression(unary.operand(), function);
      if (unary.op() == UnaryOp::Plus) {
        return operand;
      }
      tir::UnaryOp unary_op = tir::UnaryOp::Negate;
      switch (unary.op()) {
        case UnaryOp::Negate: unary_op = tir::UnaryOp::Negate; break;
        case UnaryOp::LogicalNot: unary_op = tir::UnaryOp::LogicalNot; break;
        case UnaryOp::BitwiseNot: unary_op = tir::UnaryOp::BitwiseNot; break;
        default: break;
      }
      tir::Value result = function.allocate_value(unary.type().is_valid() ? unary.type() : operand.type);
      tir::Op op;
      op.kind = tir::OpKind::Unary;
      op.location = unary.location();
      op.result = result;
      op.operand = operand;
      op.unary_op = unary_op;
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(expr);

      // Short-circuit evaluation for && and ||
      if (binary.op() == BinaryOp::LogicalAnd || binary.op() == BinaryOp::LogicalOr) {
        tir::Value slot = function.allocate_value(Type::bool_type());
        tir::Op alloca;
        alloca.kind = tir::OpKind::Alloca;
        alloca.location = binary.location();
        alloca.result = slot;
        alloca.slot_name = "$short_circuit";
        function.add_operation(std::move(alloca));

        tir::Value lhs = lower_expression(binary.lhs(), function);
        tir::Op store_lhs;
        store_lhs.kind = tir::OpKind::Store;
        store_lhs.location = binary.location();
        store_lhs.lhs = slot;
        store_lhs.operand = lhs;
        store_lhs.result = slot;
        function.add_operation(std::move(store_lhs));

        int rhs_label = function.allocate_label();
        int merge_label = function.allocate_label();

        tir::Op br_cond;
        br_cond.kind = tir::OpKind::BranchCond;
        br_cond.location = binary.location();
        br_cond.operand = lhs;
        if (binary.op() == BinaryOp::LogicalAnd) {
          br_cond.true_label = rhs_label;
          br_cond.false_label = merge_label;
        } else {
          br_cond.true_label = merge_label;
          br_cond.false_label = rhs_label;
        }
        function.add_operation(std::move(br_cond));

        // Evaluate RHS block
        tir::Op rhs_lbl;
        rhs_lbl.kind = tir::OpKind::Label;
        rhs_lbl.label_id = rhs_label;
        function.add_operation(std::move(rhs_lbl));

        tir::Value rhs = lower_expression(binary.rhs(), function);
        tir::Op store_rhs;
        store_rhs.kind = tir::OpKind::Store;
        store_rhs.location = binary.location();
        store_rhs.lhs = slot;
        store_rhs.operand = rhs;
        store_rhs.result = slot;
        function.add_operation(std::move(store_rhs));

        tir::Op br_merge;
        br_merge.kind = tir::OpKind::Branch;
        br_merge.label_id = merge_label;
        function.add_operation(std::move(br_merge));

        // Merge block
        tir::Op merge_lbl;
        merge_lbl.kind = tir::OpKind::Label;
        merge_lbl.label_id = merge_label;
        function.add_operation(std::move(merge_lbl));

        tir::Value final_val = function.allocate_value(Type::bool_type());
        tir::Op load_res;
        load_res.kind = tir::OpKind::Load;
        load_res.location = binary.location();
        load_res.lhs = slot;
        load_res.result = final_val;
        function.add_operation(std::move(load_res));
        return final_val;
      }

      const tir::Value lhs = lower_expression(binary.lhs(), function);
      const tir::Value rhs = lower_expression(binary.rhs(), function);
      tir::BinaryOp op_kind = map_binary_op(binary.op());

      Type res_type = binary.type().is_valid() ? binary.type() : lhs.type;
      tir::Value result = function.allocate_value(res_type);
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
      if (call.arguments().size() != 1) return tir::Value{};
      const Expr& argument = *call.arguments().front();
      if (argument.kind() == Expr::Kind::StringLiteral) {
        tir::Op op;
        op.kind = tir::OpKind::PrintString;
        op.location = call.location();
        op.string_value = static_cast<const StringLiteralExpr&>(argument).value();
        function.add_operation(std::move(op));
        return tir::Value{};
      }

      const tir::Value arg_val = lower_expression(argument, function);
      tir::Op op;
      op.kind = tir::OpKind::Print;
      op.location = call.location();
      op.operand = arg_val;
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
