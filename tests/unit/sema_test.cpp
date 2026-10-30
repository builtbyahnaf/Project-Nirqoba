#include "test_harness.h"

#include "nirqoba/Frontend/Lexer.h"
#include "nirqoba/Frontend/Parser.h"
#include "nirqoba/Frontend/SourceFile.h"
#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Sema/TypeChecker.h"

using namespace nirq;

static std::unique_ptr<Program> parse_string(const std::string& code, DiagnosticEngine& diag) {
  SourceFile src("test.nrqba", code);
  Lexer lexer(src, diag);
  auto tokens = lexer.tokenize();
  Parser parser(std::move(tokens), diag);
  return parser.parse_program();
}

TEST(SemaTest, AcceptsIntAndVoidMain) {
  {
    DiagnosticEngine diag;
    auto prog = parse_string("int function main() { return 0; }", diag);
    ASSERT_FALSE(diag.has_errors());
    TypeChecker checker(diag);
    ASSERT_TRUE(checker.check(*prog));
    ASSERT_FALSE(diag.has_errors());
  }
  {
    DiagnosticEngine diag;
    auto prog = parse_string("void function main() { print(\"Hi\"); return; }", diag);
    ASSERT_FALSE(diag.has_errors());
    TypeChecker checker(diag);
    ASSERT_TRUE(checker.check(*prog));
    ASSERT_FALSE(diag.has_errors());
  }
}

TEST(SemaTest, RejectsMissingMain) {
  DiagnosticEngine diag;
  auto prog = parse_string("int function foo() { return 1; }", diag);
  ASSERT_FALSE(diag.has_errors());
  TypeChecker checker(diag);
  ASSERT_FALSE(checker.check(*prog));
  ASSERT_TRUE(diag.has_errors());
  ASSERT_EQ(diag.diagnostics()[0].code, diag::kMissingMain);
}

TEST(SemaTest, RejectsDuplicateMain) {
  DiagnosticEngine diag;
  auto prog = parse_string("int function main() { return 1; }\nint function main() { return 2; }", diag);
  TypeChecker checker(diag);
  ASSERT_FALSE(checker.check(*prog));
  ASSERT_TRUE(diag.has_errors());
}

TEST(SemaTest, RejectsPrint42) {
  DiagnosticEngine diag;
  auto prog = parse_string("int function main() { print(42); return 0; }", diag);
  TypeChecker checker(diag);
  ASSERT_FALSE(checker.check(*prog));
  ASSERT_TRUE(diag.has_errors());
  ASSERT_EQ(diag.diagnostics()[0].code, diag::kPrintRequiresString);
}

TEST(SemaTest, RejectsUndefinedVariable) {
  DiagnosticEngine diag;
  auto prog = parse_string("int function main() { return x; }", diag);
  TypeChecker checker(diag);
  ASSERT_FALSE(checker.check(*prog));
  ASSERT_TRUE(diag.has_errors());
  ASSERT_EQ(diag.diagnostics()[0].code, diag::kUndefinedName);
}

TEST(SemaTest, RejectsDuplicateDeclarationInSameScope) {
  DiagnosticEngine diag;
  auto prog = parse_string("int function main() { let x = 1; let x = 2; return 0; }", diag);
  TypeChecker checker(diag);
  ASSERT_FALSE(checker.check(*prog));
  ASSERT_TRUE(diag.has_errors());
  ASSERT_EQ(diag.diagnostics()[0].code, diag::kDuplicateName);
}

TEST(SemaTest, RejectsMainParameters) {
  DiagnosticEngine diag;
  auto prog = parse_string("int function main(int argc) { return 0; }", diag);
  TypeChecker checker(diag);
  ASSERT_FALSE(checker.check(*prog));
  ASSERT_TRUE(diag.has_errors());
  ASSERT_EQ(diag.diagnostics()[0].code, diag::kMainParameters);
}
