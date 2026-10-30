// Lowers a type-checked AST into TIR.
//
// Preconditions: the TypeChecker has run and produced no errors, so every
// expression carries a valid type. Lowering therefore never re-reports type
// errors; it may only report internal inconsistencies (NRQ4001).
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/Ast.h"
#include "nirqoba/TIR/IR.h"

namespace nirq {

class TirBuilder {
 public:
  explicit TirBuilder(DiagnosticEngine& diagnostics) : diagnostics_(diagnostics) {}

  std::unique_ptr<tir::Module> lower(const Program& program);

 private:
  void lower_function(const FunctionDecl& function, tir::Module& module);
  void lower_block(const BlockStmt& block, tir::Function& function);
  void lower_statement(const Stmt& statement, tir::Function& function);
  tir::Value lower_expression(const Expr& expr, tir::Function& function);

  void push_scope() { scopes_.emplace_back(); }
  void pop_scope() { scopes_.pop_back(); }
  tir::Value lookup_slot(const std::string& name) const;

  DiagnosticEngine& diagnostics_;
  // Stack of name -> slot handle maps, mirroring the checker's scopes.
  std::vector<std::unordered_map<std::string, tir::Value>> scopes_;
};

}  // namespace nirq
