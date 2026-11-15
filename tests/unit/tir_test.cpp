#include "test_harness.h"

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/Lexer.h"
#include "nirqoba/Frontend/Parser.h"
#include "nirqoba/Frontend/SourceFile.h"
#include "nirqoba/Sema/TypeChecker.h"
#include "nirqoba/TIR/Builder.h"
#include "nirqoba/TIR/Printing.h"
#include "nirqoba/TIR/Verifier.h"

using namespace nirq;
using namespace std;

TEST(TirTest, BuildAndVerifySimpleProgram) {
  DiagnosticEngine diag;
  SourceFile src("tir_simple.nrqba",
                 "int function main() {\n"
                 "  print(\"Hello\");\n"
                 "  return 42;\n"
                 "}");
  Lexer lexer(src, diag);
  Parser parser(lexer.tokenize(), diag);
  auto prog = parser.parse_program();
  ASSERT_FALSE(diag.has_errors());

  TypeChecker checker(diag);
  ASSERT_TRUE(checker.check(*prog));

  TirBuilder builder(diag);
  auto tir_mod = builder.lower(*prog);
  ASSERT_TRUE(tir_mod != nullptr);

  bool ok = verify_tir_module(*tir_mod, diag);
  ASSERT_TRUE(ok);
  ASSERT_FALSE(diag.has_errors());

  std::string printed = print_tir(*tir_mod);
  ASSERT_TRUE(printed.find("func @main() -> i64") != std::string::npos);
  ASSERT_TRUE(printed.find("tir.print_string \"Hello\"") != std::string::npos);
  ASSERT_TRUE(printed.find("tir.return") != std::string::npos);
}

// Counts destructions, not textual calls: automatic cleanup is a Drop whose
// release is expanded during lowering, while a user-written release stays a
// Call in TIR.
static std::size_t count_calls(const tir::Function& function,
                               const std::string& callee) {
  std::size_t count = 0;
  for (const tir::Op& op : function.operations()) {
    if (op.kind == tir::OpKind::Call && op.callee == callee) ++count;
    if (op.kind == tir::OpKind::Drop && op.string_value == callee) ++count;
  }
  return count;
}

static const tir::Function* find_function(const tir::Module& module,
                                          const std::string& name) {
  for (const auto& function : module.functions())
    if (function->name() == name) return function.get();
  return nullptr;
}

TEST(TirTest, ForInitializerCleanupRunsAtLoopExit) {
  DiagnosticEngine diag;
  SourceFile src("tir_for_cleanup.nrqba",
                 "int function main() {\n"
                 "  var i = 0;\n"
                 "  for (var buffer = memory.shared_allocate<byte>(8); i < 0; i += 1) {}\n"
                 "  print(i);\n"
                 "  return 0;\n"
                 "}");
  Lexer lexer(src, diag);
  Parser parser(lexer.tokenize(), diag);
  auto prog = parser.parse_program();
  ASSERT_FALSE(diag.has_errors());
  TypeChecker checker(diag);
  ASSERT_TRUE(checker.check(*prog));
  TirBuilder builder(diag);
  auto module = builder.lower(*prog);
  ASSERT_TRUE(module != nullptr);
  const tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);

  int release_index = -1;
  int print_index = -1;
  for (std::size_t index = 0; index < main_fn->operations().size(); ++index) {
    const tir::Op& op = main_fn->operations()[index];
    if ((op.kind == tir::OpKind::Call && op.callee == "memory.shared_release") ||
        (op.kind == tir::OpKind::Drop && op.string_value == "memory.shared_release"))
      release_index = static_cast<int>(index);
    if (op.kind == tir::OpKind::Print && print_index < 0)
      print_index = static_cast<int>(index);
  }
  ASSERT_TRUE(release_index >= 0);
  ASSERT_TRUE(print_index >= 0);
  ASSERT_TRUE(release_index < print_index);
}

TEST(TirTest, TransferredLocalEmitsNoAutomaticCleanup) {
  DiagnosticEngine diag;
  SourceFile src("tir_return_cleanup.nrqba",
                 "byte* function make() {\n"
                 "  let buffer = memory.allocate<byte>(8);\n"
                 "  return buffer;\n"
                 "}\n"
                 "int function main() {\n"
                 "  let result = make();\n"
                 "  memory.deallocate(result);\n"
                 "  return 0;\n"
                 "}");
  Lexer lexer(src, diag);
  Parser parser(lexer.tokenize(), diag);
  auto prog = parser.parse_program();
  ASSERT_FALSE(diag.has_errors());
  TypeChecker checker(diag);
  ASSERT_TRUE(checker.check(*prog));
  TirBuilder builder(diag);
  auto module = builder.lower(*prog);
  ASSERT_TRUE(module != nullptr);
  const tir::Function* make_fn = find_function(*module, "make");
  const tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(make_fn != nullptr);
  ASSERT_TRUE(main_fn != nullptr);
  ASSERT_EQ(count_calls(*make_fn, "memory.deallocate"), 0u);
  ASSERT_EQ(count_calls(*main_fn, "memory.deallocate"), 1u);
}

TEST(TirTest, UnconditionalDeferSupersedesAutomaticCleanup) {
  DiagnosticEngine diag;
  SourceFile src("tir_defer_cleanup.nrqba",
                 "int function main() {\n"
                 "  let buffer = memory.shared_allocate<byte>(8);\n"
                 "  defer memory.shared_release(buffer);\n"
                 "  return 0;\n"
                 "}");
  Lexer lexer(src, diag);
  Parser parser(lexer.tokenize(), diag);
  auto prog = parser.parse_program();
  ASSERT_FALSE(diag.has_errors());
  TypeChecker checker(diag);
  ASSERT_TRUE(checker.check(*prog));
  TirBuilder builder(diag);
  auto module = builder.lower(*prog);
  ASSERT_TRUE(module != nullptr);
  const tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  ASSERT_EQ(count_calls(*main_fn, "memory.shared_release"), 1u);
}

TEST(TirTest, ConditionalDeferKeepsAutomaticFallback) {
  DiagnosticEngine diag;
  SourceFile src("tir_conditional_defer.nrqba",
                 "int function main() {\n"
                 "  let buffer = memory.shared_allocate<byte>(8);\n"
                 "  if (false) { defer memory.shared_release(buffer); }\n"
                 "  return 0;\n"
                 "}");
  Lexer lexer(src, diag);
  Parser parser(lexer.tokenize(), diag);
  auto prog = parser.parse_program();
  ASSERT_FALSE(diag.has_errors());
  TypeChecker checker(diag);
  ASSERT_TRUE(checker.check(*prog));
  TirBuilder builder(diag);
  auto module = builder.lower(*prog);
  ASSERT_TRUE(module != nullptr);
  const tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  ASSERT_EQ(count_calls(*main_fn, "memory.shared_release"), 2u);
}

TEST(TirTest, ConditionalMoveKeepsAutomaticFallback) {
  DiagnosticEngine diag;
  SourceFile src("tir_conditional_move.nrqba",
                 "int function main() {\n"
                 "  let buffer = memory.shared_allocate<byte>(8);\n"
                 "  if (false) { let owner = buffer; }\n"
                 "  return 0;\n"
                 "}");
  Lexer lexer(src, diag);
  Parser parser(lexer.tokenize(), diag);
  auto prog = parser.parse_program();
  ASSERT_FALSE(diag.has_errors());
  TypeChecker checker(diag);
  ASSERT_TRUE(checker.check(*prog));
  TirBuilder builder(diag);
  auto module = builder.lower(*prog);
  ASSERT_TRUE(module != nullptr);
  const tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  ASSERT_EQ(count_calls(*main_fn, "memory.shared_release"), 2u);
}

TEST(TirTest, ParameterDeferSupersedesAutomaticCleanup) {
  DiagnosticEngine diag;
  SourceFile src("tir_parameter_defer.nrqba",
                 "void function release(byte* buffer) {\n"
                 "  defer memory.deallocate(buffer);\n"
                 "}\n"
                 "int function main() { return 0; }\n");
  Lexer lexer(src, diag);
  Parser parser(lexer.tokenize(), diag);
  auto prog = parser.parse_program();
  ASSERT_FALSE(diag.has_errors());
  TypeChecker checker(diag);
  ASSERT_TRUE(checker.check(*prog));
  TirBuilder builder(diag);
  auto module = builder.lower(*prog);
  ASSERT_TRUE(module != nullptr);
  const tir::Function* release_fn = find_function(*module, "release");
  ASSERT_TRUE(release_fn != nullptr);
  ASSERT_EQ(count_calls(*release_fn, "memory.deallocate"), 1u);
}

TEST(TirTest, VerifierRejectsMissingReturn) {
  DiagnosticEngine diag;
  tir::Module mod;
  auto fn = std::make_unique<tir::Function>("broken", Type::int_type());
  // Empty operations
  mod.add_function(std::move(fn));

  bool ok = verify_tir_module(mod, diag);
  ASSERT_FALSE(ok);
  ASSERT_TRUE(diag.has_errors());
}

TEST(TirTest, VerifierRejectsBorrowedScalarParameter) {
  DiagnosticEngine diag;
  tir::Module mod;
  auto fn = std::make_unique<tir::Function>("bad_borrow", Type::void_type());
  tir::Value parameter =
      fn->allocate_value(Type::int_type(), ValueOwnership::Borrowed);
  fn->add_parameter("value", parameter.type, parameter, ValueOwnership::Borrowed);
  tir::Op ret;
  ret.kind = tir::OpKind::Return;
  fn->add_operation(ret);
  mod.add_function(std::move(fn));

  ASSERT_FALSE(verify_tir_module(mod, diag));
  ASSERT_TRUE(diag.has_errors());
}

TEST(TirTest, VerifierRejectsReleasingBorrowedValue) {
  DiagnosticEngine diag;
  tir::Module mod;
  auto fn = std::make_unique<tir::Function>("release_borrow", Type::void_type());
  const Type pointer = Type::pointer_type(Type::int_type());
  tir::Value parameter = fn->allocate_value(pointer, ValueOwnership::Borrowed);
  fn->add_parameter("value", pointer, parameter, ValueOwnership::Borrowed);
  tir::Op release;
  release.kind = tir::OpKind::Call;
  release.callee = "memory.deallocate";
  release.arguments.push_back(parameter);
  fn->add_operation(release);
  tir::Op ret;
  ret.kind = tir::OpKind::Return;
  fn->add_operation(ret);
  mod.add_function(std::move(fn));

  ASSERT_FALSE(verify_tir_module(mod, diag));
  ASSERT_TRUE(diag.has_errors());
}

TEST(TirTest, VerifierRejectsReleaseAfterConsumption) {
  DiagnosticEngine diag;
  tir::Module mod;
  auto fn = std::make_unique<tir::Function>("double_release", Type::void_type());
  const Type pointer = Type::pointer_type(Type::int_type());
  tir::Value parameter = fn->allocate_value(pointer, ValueOwnership::Owned);
  fn->add_parameter("value", pointer, parameter, ValueOwnership::Owned);
  for (int i = 0; i < 2; ++i) {
    tir::Op release;
    release.kind = tir::OpKind::Call;
    release.callee = "memory.deallocate";
    release.arguments.push_back(parameter);
    fn->add_operation(release);
  }
  tir::Op ret;
  ret.kind = tir::OpKind::Return;
  fn->add_operation(ret);
  mod.add_function(std::move(fn));

  ASSERT_FALSE(verify_tir_module(mod, diag));
  ASSERT_TRUE(diag.has_errors());
}

TEST(TirTest, VerifierRejectsWrongReleaseKind) {
  DiagnosticEngine diag;
  tir::Module mod;
  auto fn = std::make_unique<tir::Function>("wrong_release", Type::void_type());
  const Type shared = Type::shared_pointer_type(Type::int_type());
  tir::Value parameter = fn->allocate_value(shared, ValueOwnership::Shared);
  fn->add_parameter("value", shared, parameter, ValueOwnership::Shared);
  tir::Op release;
  release.kind = tir::OpKind::Call;
  release.callee = "array.owned_release";
  release.arguments.push_back(parameter);
  fn->add_operation(release);
  tir::Op ret;
  ret.kind = tir::OpKind::Return;
  fn->add_operation(ret);
  mod.add_function(std::move(fn));

  ASSERT_FALSE(verify_tir_module(mod, diag));
  ASSERT_TRUE(diag.has_errors());
}

TEST(TirTest, VerifierRejectsBorrowedReturnWithoutSource) {
  DiagnosticEngine diag;
  tir::Module mod;
  const Type pointer = Type::pointer_type(Type::int_type());
  auto fn = std::make_unique<tir::Function>("borrow_return", pointer);
  fn->set_return_ownership(ValueOwnership::Borrowed);
  tir::Value value = fn->allocate_value(pointer, ValueOwnership::Borrowed);
  tir::Op ret;
  ret.kind = tir::OpKind::Return;
  ret.operand = value;
  fn->add_operation(ret);
  mod.add_function(std::move(fn));

  ASSERT_FALSE(verify_tir_module(mod, diag));
  ASSERT_TRUE(diag.has_errors());
}

TEST(TirTest, VerifierAcceptsBorrowedReturnWithSource) {
  DiagnosticEngine diag;
  tir::Module mod;
  const Type pointer = Type::pointer_type(Type::int_type());
  auto fn = std::make_unique<tir::Function>("borrow_return", pointer);
  fn->set_return_ownership(ValueOwnership::Borrowed);
  fn->set_return_borrow_parameter("source");
  tir::Value parameter = fn->allocate_value(pointer, ValueOwnership::Borrowed);
  fn->add_parameter("source", pointer, parameter, ValueOwnership::Borrowed);
  tir::Op ret;
  ret.kind = tir::OpKind::Return;
  ret.operand = parameter;
  fn->add_operation(ret);
  mod.add_function(std::move(fn));

  ASSERT_TRUE(verify_tir_module(mod, diag));
  ASSERT_FALSE(diag.has_errors());
}

TEST(TirTest, VerifierRejectsReturnOwnershipMismatch) {
  DiagnosticEngine diag;
  tir::Module mod;
  const Type shared = Type::shared_pointer_type(Type::int_type());
  auto fn = std::make_unique<tir::Function>("bad_return", shared);
  fn->set_return_ownership(ValueOwnership::Shared);
  tir::Value value = fn->allocate_value(shared, ValueOwnership::Owned);
  tir::Op ret;
  ret.kind = tir::OpKind::Return;
  ret.operand = value;
  fn->add_operation(ret);
  mod.add_function(std::move(fn));

  ASSERT_FALSE(verify_tir_module(mod, diag));
  ASSERT_TRUE(diag.has_errors());
}

TEST(TirTest, VerifierRejectsBorrowedArgumentToOwnedParameter) {
  DiagnosticEngine diag;
  tir::Module mod;
  const Type pointer = Type::pointer_type(Type::int_type());

  auto callee = std::make_unique<tir::Function>("take_owned", Type::void_type());
  tir::Value owned = callee->allocate_value(pointer, ValueOwnership::Owned);
  callee->add_parameter("value", pointer, owned, ValueOwnership::Owned);
  tir::Op callee_return;
  callee_return.kind = tir::OpKind::Return;
  callee->add_operation(callee_return);
  mod.add_function(std::move(callee));

  auto caller = std::make_unique<tir::Function>("caller", Type::void_type());
  tir::Value borrowed = caller->allocate_value(pointer, ValueOwnership::Borrowed);
  caller->add_parameter("value", pointer, borrowed, ValueOwnership::Borrowed);
  tir::Op call;
  call.kind = tir::OpKind::Call;
  call.callee = "take_owned";
  call.arguments.push_back(borrowed);
  caller->add_operation(call);
  tir::Op caller_return;
  caller_return.kind = tir::OpKind::Return;
  caller->add_operation(caller_return);
  mod.add_function(std::move(caller));

  ASSERT_FALSE(verify_tir_module(mod, diag));
  ASSERT_TRUE(diag.has_errors());
}

TEST(TirTest, VerifierAcceptsOwnedArgumentToBorrowedParameter) {
  DiagnosticEngine diag;
  tir::Module mod;
  const Type pointer = Type::pointer_type(Type::int_type());

  auto callee = std::make_unique<tir::Function>("read", Type::void_type());
  tir::Value borrowed = callee->allocate_value(pointer, ValueOwnership::Borrowed);
  callee->add_parameter("value", pointer, borrowed, ValueOwnership::Borrowed);
  tir::Op callee_return;
  callee_return.kind = tir::OpKind::Return;
  callee->add_operation(callee_return);
  mod.add_function(std::move(callee));

  auto caller = std::make_unique<tir::Function>("caller", Type::void_type());
  tir::Value owned = caller->allocate_value(pointer, ValueOwnership::Owned);
  caller->add_parameter("value", pointer, owned, ValueOwnership::Owned);
  tir::Op call;
  call.kind = tir::OpKind::Call;
  call.callee = "read";
  call.arguments.push_back(owned);
  caller->add_operation(call);
  tir::Op caller_return;
  caller_return.kind = tir::OpKind::Return;
  caller->add_operation(caller_return);
  mod.add_function(std::move(caller));

  ASSERT_TRUE(verify_tir_module(mod, diag));
}

TEST(TirTest, VerifierRejectsImmutableBorrowToMutableParameter) {
  DiagnosticEngine diag;
  tir::Module mod;
  const Type pointer = Type::pointer_type(Type::int_type());

  auto callee = std::make_unique<tir::Function>("take_mutable", Type::void_type());
  tir::Value mutable_parameter =
      callee->allocate_value(pointer, ValueOwnership::MutableBorrowed);
  callee->add_parameter("value", pointer, mutable_parameter,
                        ValueOwnership::MutableBorrowed);
  tir::Op callee_return;
  callee_return.kind = tir::OpKind::Return;
  callee->add_operation(callee_return);
  mod.add_function(std::move(callee));

  auto caller = std::make_unique<tir::Function>("caller", Type::void_type());
  tir::Value borrowed = caller->allocate_value(pointer, ValueOwnership::Borrowed);
  caller->add_parameter("value", pointer, borrowed, ValueOwnership::Borrowed);
  tir::Op call;
  call.kind = tir::OpKind::Call;
  call.callee = "take_mutable";
  call.arguments.push_back(borrowed);
  caller->add_operation(call);
  tir::Op caller_return;
  caller_return.kind = tir::OpKind::Return;
  caller->add_operation(caller_return);
  mod.add_function(std::move(caller));

  ASSERT_FALSE(verify_tir_module(mod, diag));
  ASSERT_TRUE(diag.has_errors());
}

TEST(TirTest, VerifierRejectsForgedResultOwnership) {
  DiagnosticEngine diag;
  tir::Module mod;
  auto fn = std::make_unique<tir::Function>("forged", Type::void_type());
  const Type pointer = Type::pointer_type(Type::int_type());
  tir::Value value = fn->allocate_value(pointer, ValueOwnership::Owned);
  value.ownership = ValueOwnership::Shared;
  tir::Op constant;
  constant.kind = tir::OpKind::ConstantInt;
  constant.result = value;
  fn->add_operation(constant);
  tir::Op ret;
  ret.kind = tir::OpKind::Return;
  fn->add_operation(ret);
  mod.add_function(std::move(fn));

  ASSERT_FALSE(verify_tir_module(mod, diag));
  ASSERT_TRUE(diag.has_errors());
}

TEST(TirTest, VerifierAcceptsOwnedRelease) {
  DiagnosticEngine diag;
  tir::Module mod;
  auto fn = std::make_unique<tir::Function>("release_owned", Type::void_type());
  const Type pointer = Type::pointer_type(Type::int_type());
  tir::Value parameter = fn->allocate_value(pointer, ValueOwnership::Owned);
  fn->add_parameter("value", pointer, parameter, ValueOwnership::Owned);
  tir::Op release;
  release.kind = tir::OpKind::Call;
  release.callee = "memory.deallocate";
  release.arguments.push_back(parameter);
  fn->add_operation(release);
  tir::Op ret;
  ret.kind = tir::OpKind::Return;
  fn->add_operation(ret);
  mod.add_function(std::move(fn));

  ASSERT_TRUE(verify_tir_module(mod, diag));
  ASSERT_FALSE(diag.has_errors());
}
