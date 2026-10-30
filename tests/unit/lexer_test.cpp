#include "test_harness.h"

#include "nirqoba/Frontend/Lexer.h"
#include "nirqoba/Frontend/SourceFile.h"
#include "nirqoba/Diagnostics/DiagnosticEngine.h"

using namespace nirq;

TEST(LexerTest, TokenMatrix) {
  DiagnosticEngine diag;
  SourceFile src("test.nrqba", "int function main() { let x = 42; return 0; }");
  Lexer lexer(src, diag);
  auto tokens = lexer.tokenize();

  ASSERT_FALSE(diag.has_errors());
  ASSERT_EQ(tokens[0].kind, TokenKind::KwInt);
  ASSERT_EQ(tokens[1].kind, TokenKind::KwFunction);
  ASSERT_EQ(tokens[2].kind, TokenKind::Identifier);
  ASSERT_EQ(tokens[2].text, "main");
  ASSERT_EQ(tokens[3].kind, TokenKind::LParen);
  ASSERT_EQ(tokens[4].kind, TokenKind::RParen);
  ASSERT_EQ(tokens[5].kind, TokenKind::LBrace);
  ASSERT_EQ(tokens[6].kind, TokenKind::KwLet);
  ASSERT_EQ(tokens[7].kind, TokenKind::Identifier);
  ASSERT_EQ(tokens[7].text, "x");
  ASSERT_EQ(tokens[8].kind, TokenKind::Equal);
  ASSERT_EQ(tokens[9].kind, TokenKind::IntegerLiteral);
  ASSERT_EQ(tokens[9].integer_value, 42);
  ASSERT_EQ(tokens[10].kind, TokenKind::Semicolon);
  ASSERT_EQ(tokens[11].kind, TokenKind::KwReturn);
  ASSERT_EQ(tokens[12].kind, TokenKind::IntegerLiteral);
  ASSERT_EQ(tokens[12].integer_value, 0);
  ASSERT_EQ(tokens[13].kind, TokenKind::Semicolon);
  ASSERT_EQ(tokens[14].kind, TokenKind::RBrace);
  ASSERT_EQ(tokens[15].kind, TokenKind::EndOfFile);
}

TEST(LexerTest, LineColumnTracking) {
  DiagnosticEngine diag;
  SourceFile src("coords.nrqba", "int\n  function\n    main");
  Lexer lexer(src, diag);
  auto tokens = lexer.tokenize();

  ASSERT_EQ(tokens[0].location.line, 1u);
  ASSERT_EQ(tokens[0].location.column, 1u);

  ASSERT_EQ(tokens[1].location.line, 2u);
  ASSERT_EQ(tokens[1].location.column, 3u);

  ASSERT_EQ(tokens[2].location.line, 3u);
  ASSERT_EQ(tokens[2].location.column, 5u);
}

TEST(LexerTest, EscapeHandling) {
  DiagnosticEngine diag;
  SourceFile src("escapes.nrqba", "\"Hello\\n\\t\\r\\\\\\\"World\"");
  Lexer lexer(src, diag);
  auto tokens = lexer.tokenize();

  ASSERT_FALSE(diag.has_errors());
  ASSERT_EQ(tokens[0].kind, TokenKind::StringLiteral);
  ASSERT_EQ(tokens[0].string_value, "Hello\n\t\r\\\"World");
}

TEST(LexerTest, UnterminatedString) {
  DiagnosticEngine diag;
  SourceFile src("unterminated.nrqba", "\"unterminated string");
  Lexer lexer(src, diag);
  auto tokens = lexer.tokenize();

  ASSERT_TRUE(diag.has_errors());
  ASSERT_EQ(diag.diagnostics()[0].code, diag::kUnterminatedString);
}

TEST(LexerTest, UnexpectedCharacter) {
  DiagnosticEngine diag;
  SourceFile src("illegal.nrqba", "int main @");
  Lexer lexer(src, diag);
  auto tokens = lexer.tokenize();

  ASSERT_TRUE(diag.has_errors());
  ASSERT_EQ(diag.diagnostics()[0].code, diag::kUnexpectedCharacter);
}

TEST(LexerTest, Comments) {
  DiagnosticEngine diag;
  SourceFile src("comments.nrqba", "// single line comment\n/* multi\nline\ncomment */ 123");
  Lexer lexer(src, diag);
  auto tokens = lexer.tokenize();

  ASSERT_FALSE(diag.has_errors());
  ASSERT_EQ(tokens[0].kind, TokenKind::IntegerLiteral);
  ASSERT_EQ(tokens[0].integer_value, 123);
}

TEST(LexerTest, Phase2KeywordsAndOperatorsRecognized) {
  DiagnosticEngine diag;
  SourceFile src("future.nrqba", "if else while for struct module + - * / == !=");
  Lexer lexer(src, diag);
  auto tokens = lexer.tokenize();

  ASSERT_FALSE(diag.has_errors());
  ASSERT_EQ(tokens[0].kind, TokenKind::KwIf);
  ASSERT_EQ(tokens[1].kind, TokenKind::KwElse);
  ASSERT_EQ(tokens[2].kind, TokenKind::KwWhile);
  ASSERT_EQ(tokens[3].kind, TokenKind::KwFor);
  ASSERT_EQ(tokens[4].kind, TokenKind::KwStruct);
  ASSERT_EQ(tokens[5].kind, TokenKind::KwModule);
  ASSERT_EQ(tokens[6].kind, TokenKind::Plus);
  ASSERT_EQ(tokens[7].kind, TokenKind::Minus);
  ASSERT_EQ(tokens[8].kind, TokenKind::Star);
  ASSERT_EQ(tokens[9].kind, TokenKind::Slash);
  ASSERT_EQ(tokens[10].kind, TokenKind::EqualEqual);
  ASSERT_EQ(tokens[11].kind, TokenKind::BangEqual);
}
