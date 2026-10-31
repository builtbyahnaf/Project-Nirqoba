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

  // Lowers one module of a multi-file program. `symbol_prefix` is empty for the
  // root module (functions keep their bare names so `main` links) and the module
  // name otherwise, producing `module.function` symbols that calls resolve to.
  void lower_module(const std::string& symbol_prefix, const Program& program,
                    tir::Module& module);

 private:
  struct LoopContext {
    int break_label = -1;
    int continue_label = -1;
    std::size_t deferred_scope_depth = 0;
  };
  struct DeferredCall {
    std::string callee;
    SourceLocation location;
    std::vector<tir::Value> arguments;
  };

  void lower_function(const FunctionDecl& function, const std::string& symbol,
                      tir::Module& module);
  void lower_block(const BlockStmt& block, tir::Function& function);
  void lower_statement(const Stmt& statement, tir::Function& function);
  void emit_deferred_from(std::size_t scope_depth, tir::Function& function);
  tir::Value lower_expression(const Expr& expr, tir::Function& function);
  tir::Value resolve_lvalue_ptr(const Expr& target, tir::Function& function);

  // True when the block currently being built already ends in a terminator
  // (return, branch or conditional branch). Used to avoid emitting unreachable
  // operations after `return`, `break` or `continue`.
  static bool block_terminated(const tir::Function& function);

  void push_scope() { scopes_.emplace_back(); }
  void pop_scope() { scopes_.pop_back(); }
  tir::Value lookup_slot(const std::string& name) const;

  DiagnosticEngine& diagnostics_;
  std::vector<std::unordered_map<std::string, tir::Value>> scopes_;
  std::vector<LoopContext> loop_stack_;
  std::vector<std::vector<DeferredCall>> deferred_expressions_;
};

}  // namespace nirq
