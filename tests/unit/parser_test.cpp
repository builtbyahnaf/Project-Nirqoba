#include "test_harness.h"

#include "nirqoba/Frontend/Lexer.h"
#include "nirqoba/Frontend/Parser.h"
#include "nirqoba/Frontend/SourceFile.h"
#include "nirqoba/Diagnostics/DiagnosticEngine.h"

using namespace nirq;

TEST(ParserTest, FirstProgramAccepts) {
  DiagnosticEngine diag;
  SourceFile src("first.nrqba",
                 "int function main() {\n"
                 "  print(\"Hello, Nirqoba!\");\n"
                 "  return 0;\n"
                 "}");
  Lexer lexer(src, diag);
  auto tokens = lexer.tokenize();
  Parser parser(std::move(tokens), diag);
  auto program = parser.parse_program();

  ASSERT_FALSE(diag.has_errors());
  ASSERT_TRUE(program != nullptr);
  ASSERT_EQ(program->functions().size(), 1u);
  ASSERT_EQ(program->functions()[0]->name(), "main");
  ASSERT_TRUE(program->functions()[0]->return_type().is_int());
}

TEST(ParserTest, RejectMissingReturnType) {
  DiagnosticEngine diag;
  SourceFile src("no_ret.nrqba", "function main() { return 0; }");
  Lexer lexer(src, diag);
  auto tokens = lexer.tokenize();
  Parser parser(std::move(tokens), diag);
  auto program = parser.parse_program();

  ASSERT_TRUE(diag.has_errors());
}

TEST(ParserTest, RejectPhase2Constructs) {
  DiagnosticEngine diag;
  SourceFile src("phase2.nrqba",
                 "int function main() {\n"
                 "  if (true) { return 1; }\n"
                 "}");
  Lexer lexer(src, diag);
  auto tokens = lexer.tokenize();
  Parser parser(std::move(tokens), diag);
  auto program = parser.parse_program();

  ASSERT_TRUE(diag.has_errors());
  ASSERT_EQ(diag.diagnostics()[0].code, diag::kNotImplementedInPhase1);
}

TEST(ParserTest, RejectUnclosedBlock) {
  DiagnosticEngine diag;
  SourceFile src("unclosed.nrqba", "int function main() { return 0;");
  Lexer lexer(src, diag);
  auto tokens = lexer.tokenize();
  Parser parser(std::move(tokens), diag);
  auto program = parser.parse_program();

  ASSERT_TRUE(diag.has_errors());
  ASSERT_EQ(diag.diagnostics()[0].code, diag::kExpectedToken);
}
