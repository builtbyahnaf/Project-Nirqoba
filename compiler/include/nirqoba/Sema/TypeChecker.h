// Name resolution and type checking for Nirqoba Phase 1, 2, 3 & 4.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/Ast.h"
#include "nirqoba/Sema/SymbolTable.h"

namespace nirq {

struct FunctionSignature {
  std::string name;
  Type return_type;
  std::vector<Type> parameter_types;
  SourceLocation location;
};

// Lightweight catalog entry for a registered struct type.
struct StructInfo {
  std::string name;
  std::vector<Parameter> fields;  // ordered field list from StructDecl
  SourceLocation location;
};

class TypeChecker {
 public:
  explicit TypeChecker(DiagnosticEngine& diagnostics) : diagnostics_(diagnostics) {}

  bool check(Program& program);

 private:
  void check_function(FunctionDecl& function);
  void check_statement(Stmt& statement);
  void check_block(BlockStmt& block);
  void check_variable_declaration(VariableDeclarationStmt& declaration);
  void check_destructuring(DestructuringStmt& statement);
  void check_assignment(AssignmentStmt& assignment);
  void check_compound_assignment(CompoundAssignmentStmt& assignment);
  void check_if(IfStmt& statement);
  void check_while(WhileStmt& statement);
  void check_for(ForStmt& statement);
  void check_break(BreakStmt& statement);
  void check_continue(ContinueStmt& statement);
  void check_return(ReturnStmt& statement);

  Type check_expression(Expr& expr);
  Type check_call(CallExpr& call);
  Type check_array_literal(ArrayLiteralExpr& expr);
  Type check_tuple_literal(TupleLiteralExpr& expr);
  Type check_index_expr(IndexExpr& expr);
  Type check_member_access(MemberAccessExpr& expr);
  Type check_struct_init(StructInitExpr& expr);

  void require_type(const Expr& expr, Type expected, const char* context);
  bool block_can_fall_through(const BlockStmt& block) const;

  // Struct catalog lookup helpers.
  const StructInfo* find_struct(const std::string& name) const;

  DiagnosticEngine& diagnostics_;
  SymbolTable symbols_;
  Type current_return_type_;
  int loop_depth_ = 0;
  std::unordered_map<std::string, FunctionSignature> functions_;
  std::unordered_map<std::string, StructInfo> structs_;
};

}  // namespace nirq
