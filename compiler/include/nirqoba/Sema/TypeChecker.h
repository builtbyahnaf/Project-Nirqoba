// Name resolution and type checking for the Phase 1 language.
//
// The checker annotates every expression with its resolved Type (so lowering
// never re-derives types) and reports NRQ3xxx diagnostics. It does not stop at
// the first error: each check records a diagnostic and returns Type::error(),
// which later checks propagate silently instead of cascading.
#pragma once

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/Ast.h"
#include "nirqoba/Sema/SymbolTable.h"

namespace nirq {

class TypeChecker {
 public:
  explicit TypeChecker(DiagnosticEngine& diagnostics) : diagnostics_(diagnostics) {}

  // True when the program is well-formed. Diagnostics are recorded regardless.
  bool check(Program& program);

 private:
  void check_function(FunctionDecl& function);
  void check_statement(Stmt& statement);
  void check_block(BlockStmt& block);
  void check_variable_declaration(VariableDeclarationStmt& declaration);
  void check_assignment(AssignmentStmt& assignment);
  void check_return(ReturnStmt& statement);

  Type check_expression(Expr& expr);
  Type check_call(CallExpr& call);

  // Reports a mismatch unless either side is already the error type.
  void require_type(const Expr& expr, Type expected, const char* context);

  // True when control can reach the end of `block` without a return, ignoring
  // nested function declarations (Phase 1 has none).
  bool block_can_fall_through(const BlockStmt& block) const;

  DiagnosticEngine& diagnostics_;
  SymbolTable symbols_;
  Type current_return_type_;
};

}  // namespace nirq
