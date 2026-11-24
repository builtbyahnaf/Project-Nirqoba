// Lowers a type-checked AST into TIR.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // A `switch` arm can be left with `break`, but `continue` belongs to the loop
    // around it, so the two kinds share this stack and disagree only about which
    // entry `continue` may target.
    bool is_loop = true;
  };
  struct DeferredCall {
    std::string callee;
    SourceLocation location;
    std::vector<tir::Value> arguments;
    Type result_type;
    bool automatic = false;
    bool active = true;
    tir::Value owner_slot;
    bool clear_owner_slot_after_call = false;
    // The fields of the owner that own storage of their own, with the release
    // each one calls. An owner that has any is destroyed field by field, which is
    // what makes an aggregate a container of owners rather than one opaque value.
    std::vector<std::pair<int, std::string>> field_releases;
  };

  void lower_function(const FunctionDecl& function, const std::string& symbol,
                      tir::Module& module);
  void lower_block(const BlockStmt& block, tir::Function& function);
  void lower_statement(const Stmt& statement, tir::Function& function);
  void emit_deferred_from(std::size_t scope_depth, tir::Function& function);
  // Emits the load/drop/clear sequence for one active automatic cleanup. Used
  // both at scope exit and immediately before an owner slot is overwritten.
  void emit_destruction(const DeferredCall& deferred, tir::Function& function);
  tir::Value lower_expression(const Expr& expr, tir::Function& function);
  // is_ok/is_err/unwrap/unwrap_err over an already-lowered Result box.
  tir::Value lower_result_operation(const CallExpr& call, const tir::Value& box,
                                    tir::Function& function);
  tir::Value resolve_lvalue_ptr(const Expr& target, tir::Function& function);

  // Removes an automatic cleanup once its owner is proven transferred or
  // destroyed on every path reaching it. Conditional regions keep the cleanup:
  // the runtime slot is nulled on the transferring path, so the fallback still
  // destroys a value that remains live on the other path.
  void suppress_automatic_cleanup(const tir::Value& slot);
  // Restores or adds the automatic cleanup for a slot that receives a new owner
  // after a move, explicit release, or an uninitialized declaration.
  void track_assignment_cleanup(const tir::Value& slot, const std::string& callee,
                                const SourceLocation& location);

  struct ConditionalRegion {
    explicit ConditionalRegion(std::size_t& depth) : depth_(depth) { ++depth_; }
    ~ConditionalRegion() { --depth_; }
    ConditionalRegion(const ConditionalRegion&) = delete;
    ConditionalRegion& operator=(const ConditionalRegion&) = delete;

   private:
    std::size_t& depth_;
  };

  // True when the block currently being built already ends in a terminator
  // (return, branch or conditional branch). Used to avoid emitting unreachable
  // operations after `return`, `break` or `continue`.
  static bool block_terminated(const tir::Function& function);

  void push_scope() { scopes_.emplace_back(); }
  void pop_scope() { scopes_.pop_back(); }
  tir::Value lookup_slot(const std::string& name) const;

  DiagnosticEngine& diagnostics_;
  // Function currently being lowered. Cleanup suppression uses it to mark a
  // destruction obligation as transferred rather than discharged.
  tir::Function* current_function_ = nullptr;
  std::vector<std::unordered_map<std::string, tir::Value>> scopes_;
  std::vector<LoopContext> loop_stack_;
  std::vector<std::vector<DeferredCall>> deferred_expressions_;
  // Number of enclosing branches, loop bodies, loop conditions and short-circuit
  // right-hand sides. A transfer at depth zero dominates the owner's cleanup.
  std::size_t conditional_depth_ = 0;
  // Reason recorded on the next Move operation. The lowering context sets this
  // around the expression that transfers ownership, so TIR can distinguish a
  // returned value from an argument or an ordinary local transfer.
  tir::TransferKind transfer_context_ = tir::TransferKind::Local;
  // True while lowering the argument of an explicit release call, where a
  // transfer discharges the obligation instead of handing it to a new owner.
  bool lowering_release_argument_ = false;
  // Borrowed values created in each lexical scope, ended when that scope exits.
  std::vector<std::vector<tir::Value>> borrow_scopes_;
  // Borrow value whose EndBorrow must not be emitted: it is being returned to
  // the caller, so its lifetime is the caller's, not this scope's.
  std::unordered_set<int> escaping_borrows_;
  struct TransferContextGuard {
    TransferContextGuard(tir::TransferKind& slot, tir::TransferKind value)
        : slot_(slot), previous_(slot) {
      slot_ = value;
    }
    ~TransferContextGuard() { slot_ = previous_; }
    TransferContextGuard(const TransferContextGuard&) = delete;
    TransferContextGuard& operator=(const TransferContextGuard&) = delete;

   private:
    tir::TransferKind& slot_;
    tir::TransferKind previous_;
  };
  void emit_move(const tir::Value& source, tir::Function& function,
                 const SourceLocation& location, tir::Value& result);
  void emit_end_borrows(tir::Function& function);
  void emit_all_end_borrows(tir::Function& function);
  void register_obligation(const tir::Value& slot, const std::string& callee,
                           const SourceLocation& location,
                           tir::Function& function);
};

}  // namespace nirq
