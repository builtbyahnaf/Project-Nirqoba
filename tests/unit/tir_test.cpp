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
