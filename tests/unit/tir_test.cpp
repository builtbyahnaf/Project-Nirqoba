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

static std::size_t count_calls(const tir::Function& function,
                               const std::string& callee) {
  std::size_t count = 0;
  for (const tir::Op& op : function.operations())
    if (op.kind == tir::OpKind::Call && op.callee == callee) ++count;
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
    if (op.kind == tir::OpKind::Call && op.callee == "memory.shared_release")
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
