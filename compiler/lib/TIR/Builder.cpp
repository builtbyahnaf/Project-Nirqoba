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
  lower_module("", program, *module);
  return module;
}

void TirBuilder::lower_module(const std::string& symbol_prefix, const Program& program,
                              tir::Module& module) {
  for (const auto& function : program.functions()) {
    // The entry point is emitted as `main` and must not be prefixed, otherwise
    // the MSVC CRT would not find it.
    const std::string symbol =
        symbol_prefix.empty() || function->name() == "main"
            ? function->name()
            : symbol_prefix + "." + function->name();
    lower_function(*function, symbol, module);
  }
}

void TirBuilder::lower_function(const FunctionDecl& function, const std::string& symbol,
                                tir::Module& module) {
  auto body = std::make_unique<tir::Function>(symbol, function.return_type());
  tir::Function* function_ir = module.add_function(std::move(body));

  scopes_.clear();
  loop_stack_.clear();
  push_scope();

  for (const Parameter& parameter : function.parameters()) {
    tir::Value param_val = function_ir->allocate_value(parameter.type);
    function_ir->add_parameter(parameter.name, parameter.type, param_val);

    tir::Value slot = function_ir->allocate_value(parameter.type);
    tir::Op alloca;
    alloca.kind = tir::OpKind::Alloca;
    alloca.location = parameter.location;
    alloca.result = slot;
    alloca.slot_name = parameter.name;
    function_ir->add_operation(std::move(alloca));

    tir::Op store;
    store.kind = tir::OpKind::Store;
    store.location = parameter.location;
    store.lhs = slot;
    store.operand = param_val;
    store.result = slot;
    function_ir->add_operation(std::move(store));

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
    case Stmt::Kind::Destructuring: {
      const auto& dest = static_cast<const DestructuringStmt&>(statement);
      const tir::Value agg_val = lower_expression(dest.value(), function);
      const Type& agg_type = agg_val.type;
      const std::vector<std::string>& names = dest.names();
      const std::size_t count = names.size();

      for (std::size_t i = 0; i < count; ++i) {
        Type elem_type;
        if (agg_type.is_tuple() && i < agg_type.tuple_elements().size()) {
          elem_type = agg_type.tuple_elements()[i];
        } else if (agg_type.is_array()) {
          elem_type = agg_type.element_type();
        } else {
          continue;
        }

        // Extract element i from the aggregate.
        tir::Value extracted = function.allocate_value(elem_type);
        {
          tir::Op ext_op;
          ext_op.kind = tir::OpKind::AggregateExtract;
          ext_op.location = dest.location();
          ext_op.operand = agg_val;
          ext_op.field_index = static_cast<int>(i);
          ext_op.result = extracted;
          function.add_operation(std::move(ext_op));
        }

        // Alloca for the bound name.
        tir::Value slot = function.allocate_value(elem_type);
        {
          tir::Op alloca;
          alloca.kind = tir::OpKind::Alloca;
          alloca.location = dest.location();
          alloca.result = slot;
          alloca.slot_name = names[i];
          function.add_operation(std::move(alloca));
        }

        // Store extracted value into slot.
        {
          tir::Op store;
          store.kind = tir::OpKind::Store;
          store.location = dest.location();
          store.lhs = slot;
          store.operand = extracted;
          store.result = slot;
          function.add_operation(std::move(store));
        }

        scopes_.back().emplace(names[i], slot);
      }
      return;
    }
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
      const tir::Value slot = resolve_lvalue_ptr(assignment.target(), function);
      if (!slot.is_valid()) {
        diagnostics_.error(kUnresolvedSymbol, assignment.target().location(),
                           "internal: assignment target has no TIR slot");
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
      const tir::Value slot = resolve_lvalue_ptr(assignment.target(), function);
      if (!slot.is_valid()) {
        diagnostics_.error(kUnresolvedSymbol, assignment.target().location(),
                           "internal: compound assignment target has no TIR slot");
        return;
      }
      tir::Value current_val = function.allocate_value(slot.type);
      tir::Op load;
      load.kind = tir::OpKind::Load;
      load.location = assignment.target().location();
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
    case Stmt::Kind::If: {
      const auto& if_stmt = static_cast<const IfStmt&>(statement);
      tir::Value cond = lower_expression(if_stmt.condition(), function);
      int then_label = function.allocate_label();
      int else_label = if_stmt.has_else() ? function.allocate_label() : -1;
      int merge_label = function.allocate_label();

      tir::Op br_cond;
      br_cond.kind = tir::OpKind::BranchCond;
      br_cond.location = if_stmt.location();
      br_cond.operand = cond;
      br_cond.true_label = then_label;
      br_cond.false_label = if_stmt.has_else() ? else_label : merge_label;
      function.add_operation(std::move(br_cond));

      // Then block
      tir::Op then_lbl;
      then_lbl.kind = tir::OpKind::Label;
      then_lbl.label_id = then_label;
      function.add_operation(std::move(then_lbl));
      lower_block(if_stmt.then_branch(), function);
      tir::Op br_then;
      br_then.kind = tir::OpKind::Branch;
      br_then.label_id = merge_label;
      function.add_operation(std::move(br_then));

      // Else block
      if (if_stmt.has_else()) {
        tir::Op else_lbl;
        else_lbl.kind = tir::OpKind::Label;
        else_lbl.label_id = else_label;
        function.add_operation(std::move(else_lbl));
        lower_statement(*if_stmt.else_branch(), function);
        tir::Op br_else;
        br_else.kind = tir::OpKind::Branch;
        br_else.label_id = merge_label;
        function.add_operation(std::move(br_else));
      }

      // Merge block
      tir::Op merge_lbl;
      merge_lbl.kind = tir::OpKind::Label;
      merge_lbl.label_id = merge_label;
      function.add_operation(std::move(merge_lbl));
      return;
    }
    case Stmt::Kind::While: {
      const auto& while_stmt = static_cast<const WhileStmt&>(statement);
      int header_label = function.allocate_label();
      int body_label = function.allocate_label();
      int exit_label = function.allocate_label();

      loop_stack_.push_back(LoopContext{exit_label, header_label});

      tir::Op br_head;
      br_head.kind = tir::OpKind::Branch;
      br_head.label_id = header_label;
      function.add_operation(std::move(br_head));

      // Header block
      tir::Op head_lbl;
      head_lbl.kind = tir::OpKind::Label;
      head_lbl.label_id = header_label;
      function.add_operation(std::move(head_lbl));
      tir::Value cond = lower_expression(while_stmt.condition(), function);
      tir::Op br_cond;
      br_cond.kind = tir::OpKind::BranchCond;
      br_cond.location = while_stmt.location();
      br_cond.operand = cond;
      br_cond.true_label = body_label;
      br_cond.false_label = exit_label;
      function.add_operation(std::move(br_cond));

      // Body block
      tir::Op body_lbl;
      body_lbl.kind = tir::OpKind::Label;
      body_lbl.label_id = body_label;
      function.add_operation(std::move(body_lbl));
      lower_block(while_stmt.body(), function);
      tir::Op br_loop;
      br_loop.kind = tir::OpKind::Branch;
      br_loop.label_id = header_label;
      function.add_operation(std::move(br_loop));

      // Exit block
      tir::Op exit_lbl;
      exit_lbl.kind = tir::OpKind::Label;
      exit_lbl.label_id = exit_label;
      function.add_operation(std::move(exit_lbl));

      loop_stack_.pop_back();
      return;
    }
    case Stmt::Kind::For: {
      const auto& for_stmt = static_cast<const ForStmt&>(statement);
      push_scope();
      if (for_stmt.has_initializer()) {
        lower_statement(*for_stmt.initializer(), function);
      }

      int header_label = function.allocate_label();
      int body_label = function.allocate_label();
      int step_label = function.allocate_label();
      int exit_label = function.allocate_label();

      loop_stack_.push_back(LoopContext{exit_label, step_label});

      tir::Op br_head;
      br_head.kind = tir::OpKind::Branch;
      br_head.label_id = header_label;
      function.add_operation(std::move(br_head));

      // Header block
      tir::Op head_lbl;
      head_lbl.kind = tir::OpKind::Label;
      head_lbl.label_id = header_label;
      function.add_operation(std::move(head_lbl));
      if (for_stmt.has_condition()) {
        tir::Value cond = lower_expression(*for_stmt.condition(), function);
        tir::Op br_cond;
        br_cond.kind = tir::OpKind::BranchCond;
        br_cond.location = for_stmt.location();
        br_cond.operand = cond;
        br_cond.true_label = body_label;
        br_cond.false_label = exit_label;
        function.add_operation(std::move(br_cond));
      } else {
        tir::Op br_body;
        br_body.kind = tir::OpKind::Branch;
        br_body.label_id = body_label;
        function.add_operation(std::move(br_body));
      }

      // Body block
      tir::Op body_lbl;
      body_lbl.kind = tir::OpKind::Label;
      body_lbl.label_id = body_label;
      function.add_operation(std::move(body_lbl));
      lower_block(for_stmt.body(), function);
      tir::Op br_step;
      br_step.kind = tir::OpKind::Branch;
      br_step.label_id = step_label;
      function.add_operation(std::move(br_step));

      // Step block
      tir::Op step_lbl;
      step_lbl.kind = tir::OpKind::Label;
      step_lbl.label_id = step_label;
      function.add_operation(std::move(step_lbl));
      if (for_stmt.has_step()) {
        lower_statement(*for_stmt.step(), function);
      }
      tir::Op br_next;
      br_next.kind = tir::OpKind::Branch;
      br_next.label_id = header_label;
      function.add_operation(std::move(br_next));

      // Exit block
      tir::Op exit_lbl;
      exit_lbl.kind = tir::OpKind::Label;
      exit_lbl.label_id = exit_label;
      function.add_operation(std::move(exit_lbl));

      loop_stack_.pop_back();
      pop_scope();
      return;
    }
    case Stmt::Kind::Break: {
      tir::Op br;
      br.kind = tir::OpKind::Branch;
      br.location = statement.location();
      br.label_id = loop_stack_.back().break_label;
      function.add_operation(std::move(br));
      return;
    }
    case Stmt::Kind::Continue: {
      tir::Op br;
      br.kind = tir::OpKind::Branch;
      br.location = statement.location();
      br.label_id = loop_stack_.back().continue_label;
      function.add_operation(std::move(br));
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
      if (call.callee() == "print") {
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

      std::vector<tir::Value> arg_vals;
      for (const auto& argument : call.arguments()) {
        arg_vals.push_back(lower_expression(*argument, function));
      }

      tir::Value result;
      if (call.type().is_valid() && !call.type().is_void()) {
        result = function.allocate_value(call.type());
      }

      tir::Op op;
      op.kind = tir::OpKind::Call;
      op.location = call.location();
      // Prefer the symbol semantic analysis resolved; builtins keep their
      // spelling because they lower to runtime entry points directly.
      op.callee = call.has_resolved_symbol() ? call.resolved_symbol() : call.callee();
      op.arguments = std::move(arg_vals);
      op.result = result;
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::ArrayLiteral: {
      const auto& arr = static_cast<const ArrayLiteralExpr&>(expr);
      const Type arr_type = expr.type().is_valid() ? expr.type()
          : (arr.elements().empty() ? Type::error()
             : Type::array_type(arr.elements()[0]->type(), arr.elements().size()));

      // Alloca for the entire array on the stack.
      tir::Value arr_slot = function.allocate_value(arr_type);
      {
        tir::Op alloca;
        alloca.kind = tir::OpKind::ArrayAlloca;
        alloca.location = arr.location();
        alloca.result = arr_slot;
        alloca.slot_name = "$array_lit";
        alloca.integer_value = static_cast<long long>(arr.elements().size());
        function.add_operation(std::move(alloca));
      }

      // For each element, compute a GEP and store.
      for (std::size_t i = 0; i < arr.elements().size(); ++i) {
        const tir::Value elem_val = lower_expression(*arr.elements()[i], function);

        // Index constant for GEP.
        tir::Value idx_val = function.allocate_value(Type::int_type());
        {
          tir::Op idx_op;
          idx_op.kind = tir::OpKind::ConstantInt;
          idx_op.location = arr.location();
          idx_op.result = idx_val;
          idx_op.integer_value = static_cast<long long>(i);
          function.add_operation(std::move(idx_op));
        }

        // GEP to element pointer.
        const Type elem_type = arr_type.is_array() ? arr_type.element_type() : Type::error();
        tir::Value elem_ptr = function.allocate_value(elem_type);
        {
          tir::Op gep;
          gep.kind = tir::OpKind::GetElementPtr;
          gep.location = arr.location();
          gep.lhs = arr_slot;
          gep.operand = idx_val;
          gep.field_index = static_cast<int>(i);
          gep.result = elem_ptr;
          function.add_operation(std::move(gep));
        }

        // Store element.
        {
          tir::Op store;
          store.kind = tir::OpKind::Store;
          store.location = arr.location();
          store.lhs = elem_ptr;
          store.operand = elem_val;
          store.result = elem_ptr;
          function.add_operation(std::move(store));
        }
      }

      // Load the whole array value from the slot.
      tir::Value loaded = function.allocate_value(arr_type);
      {
        tir::Op load;
        load.kind = tir::OpKind::Load;
        load.location = arr.location();
        load.lhs = arr_slot;
        load.result = loaded;
        function.add_operation(std::move(load));
      }
      return loaded;
    }
    case Expr::Kind::TupleLiteral: {
      const auto& tup = static_cast<const TupleLiteralExpr&>(expr);
      const Type tup_type = expr.type().is_valid() ? expr.type() : Type::error();

      // Lower each element and pack into AggregatePack.
      std::vector<tir::Value> elem_vals;
      elem_vals.reserve(tup.elements().size());
      for (const ExprPtr& elem : tup.elements()) {
        elem_vals.push_back(lower_expression(*elem, function));
      }

      tir::Value result = function.allocate_value(tup_type);
      tir::Op pack;
      pack.kind = tir::OpKind::AggregatePack;
      pack.location = tup.location();
      pack.aggregate_elements = std::move(elem_vals);
      pack.result = result;
      function.add_operation(std::move(pack));
      return result;
    }
    case Expr::Kind::Index: {
      const auto& idx_expr = static_cast<const IndexExpr&>(expr);
      const tir::Value base_val = lower_expression(idx_expr.base(), function);
      const tir::Value index_val = lower_expression(idx_expr.index(), function);

      const Type result_type = expr.type().is_valid() ? expr.type() : Type::error();

      // GEP to the element pointer.
      tir::Value elem_ptr = function.allocate_value(result_type);
      {
        tir::Op gep;
        gep.kind = tir::OpKind::GetElementPtr;
        gep.location = idx_expr.location();
        gep.lhs = base_val;
        gep.operand = index_val;
        gep.field_index = -1;  // dynamic index
        gep.result = elem_ptr;
        function.add_operation(std::move(gep));
      }

      // Load value from pointer.
      tir::Value loaded = function.allocate_value(result_type);
      {
        tir::Op load;
        load.kind = tir::OpKind::Load;
        load.location = idx_expr.location();
        load.lhs = elem_ptr;
        load.result = loaded;
        function.add_operation(std::move(load));
      }
      return loaded;
    }
    case Expr::Kind::MemberAccess: {
      const auto& mem = static_cast<const MemberAccessExpr&>(expr);
      const tir::Value base_val = lower_expression(mem.base(), function);
      const Type result_type = expr.type().is_valid() ? expr.type() : Type::error();
      const int field_idx = mem.is_tuple_access() ? mem.tuple_index() : -1;

      // For struct member: we need to look up the field index.
      int computed_field_idx = field_idx;
      if (!mem.is_tuple_access() && base_val.type.is_struct()) {
        // We can't look up the struct catalog from Builder, so we embed -1
        // and let MLIR lowering resolve by field name. For now use field_index
        // from the string_value (we'll use slot_name to carry the field name).
      }

      tir::Value result = function.allocate_value(result_type);
      tir::Op extract;
      extract.kind = tir::OpKind::AggregateExtract;
      extract.location = mem.location();
      extract.operand = base_val;
      extract.field_index = computed_field_idx;
      extract.string_value = mem.member();  // carry field name for struct lowering
      extract.result = result;
      function.add_operation(std::move(extract));
      return result;
    }
    case Expr::Kind::StructInit: {
      const auto& si = static_cast<const StructInitExpr&>(expr);
      const Type struct_type = expr.type().is_valid() ? expr.type()
                                : Type::struct_type(si.struct_name());

      std::vector<tir::Value> field_vals;
      if (si.is_positional()) {
        field_vals.reserve(si.positional_inits().size());
        for (const ExprPtr& e : si.positional_inits()) {
          field_vals.push_back(lower_expression(*e, function));
        }
      } else {
        field_vals.reserve(si.field_inits().size());
        for (const auto& kv : si.field_inits()) {
          field_vals.push_back(lower_expression(*kv.second, function));
        }
      }

      tir::Value result = function.allocate_value(struct_type);
      tir::Op pack;
      pack.kind = tir::OpKind::AggregatePack;
      pack.location = si.location();
      pack.aggregate_elements = std::move(field_vals);
      pack.slot_name = si.struct_name();
      pack.result = result;
      function.add_operation(std::move(pack));
      return result;
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

tir::Value TirBuilder::resolve_lvalue_ptr(const Expr& target, tir::Function& function) {
  if (target.kind() == Expr::Kind::Name) {
    const auto& name_expr = static_cast<const NameExpr&>(target);
    return lookup_slot(name_expr.name());
  }
  if (target.kind() == Expr::Kind::Index) {
    const auto& idx_expr = static_cast<const IndexExpr&>(target);
    tir::Value base_ptr = resolve_lvalue_ptr(idx_expr.base(), function);
    if (!base_ptr.is_valid()) return tir::Value{};
    tir::Value idx_val = lower_expression(idx_expr.index(), function);
    Type elem_type = idx_expr.type().is_valid() ? idx_expr.type() : base_ptr.type.element_type();
    tir::Value elem_ptr = function.allocate_value(elem_type);
    tir::Op gep;
    gep.kind = tir::OpKind::GetElementPtr;
    gep.location = idx_expr.location();
    gep.lhs = base_ptr;
    gep.operand = idx_val;
    gep.result = elem_ptr;
    function.add_operation(std::move(gep));
    return elem_ptr;
  }
  if (target.kind() == Expr::Kind::MemberAccess) {
    const auto& mem_expr = static_cast<const MemberAccessExpr&>(target);
    tir::Value base_ptr = resolve_lvalue_ptr(mem_expr.base(), function);
    if (!base_ptr.is_valid()) return tir::Value{};
    Type field_type = mem_expr.type().is_valid() ? mem_expr.type() : Type::error();
    tir::Value field_ptr = function.allocate_value(field_type);
    tir::Op gep;
    gep.kind = tir::OpKind::GetElementPtr;
    gep.location = mem_expr.location();
    gep.lhs = base_ptr;
    gep.field_index = mem_expr.is_tuple_access() ? mem_expr.tuple_index() : -1;
    gep.string_value = mem_expr.member();
    gep.result = field_ptr;
    function.add_operation(std::move(gep));
    return field_ptr;
  }
  return tir::Value{};
}

}  // namespace nirq
