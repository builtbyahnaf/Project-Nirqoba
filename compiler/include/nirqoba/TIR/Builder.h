// Lowers a type-checked AST into TIR.
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
  struct LoopContext {
    int break_label = -1;
    int continue_label = -1;
  };

  void lower_function(const FunctionDecl& function, tir::Module& module);
  void lower_block(const BlockStmt& block, tir::Function& function);
  void lower_statement(const Stmt& statement, tir::Function& function);
  tir::Value lower_expression(const Expr& expr, tir::Function& function);
  tir::Value resolve_lvalue_ptr(const Expr& target, tir::Function& function);

  void push_scope() { scopes_.emplace_back(); }
  void pop_scope() { scopes_.pop_back(); }
  tir::Value lookup_slot(const std::string& name) const;

  DiagnosticEngine& diagnostics_;
  std::vector<std::unordered_map<std::string, tir::Value>> scopes_;
  std::vector<LoopContext> loop_stack_;
};

}  // namespace nirq
