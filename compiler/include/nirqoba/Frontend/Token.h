// Token model for the Nirqoba lexer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "nirqoba/Diagnostics/SourceLocation.h"

namespace nirq {

enum class TokenKind {
  // Trivia and end of input.
  EndOfFile,
  Invalid,

  // Literals.
  IntegerLiteral,
  FloatLiteral,
  CharLiteral,
  StringLiteral,
  Identifier,

  // Phase 1 keywords.
  KwFunction,
  KwLet,
  KwVar,
  KwConst,
  KwReturn,
  KwPrint,
  KwInt,
  KwString,
  KwBool,
  KwVoid,
  KwTrue,
  KwFalse,

  // Phase 2 scalar type keywords & cast operator.
  KwI8,
  KwI16,
  KwI32,
  KwI64,
  KwU8,
  KwU16,
  KwU32,
  KwU64,
  KwByte,
  KwChar,
  KwF32,
  KwF64,
  KwAs,

  // Reserved for later phases: recognized, never parsed in Phase 1 or 2.
  KwIf,
  KwElse,
  KwWhile,
  KwFor,
  KwBreak,
  KwContinue,
  KwStruct,
  KwModule,
  KwImport,
  KwPublic,
  KwPrivate,
  KwProtected,
  KwAsync,
  KwAwait,
  KwFn,

  // Delimiters.
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Comma,
  Semicolon,
  Colon,
  Dot,
  Arrow,

  // Operators.
  Plus,
  Minus,
  Star,
  StarStar,
  Slash,
  Percent,
  Equal,
  PlusEqual,
  MinusEqual,
  StarEqual,
  SlashEqual,
  PercentEqual,
  PipeEqual,
  AmpEqual,
  CaretEqual,
  LessLessEqual,
  GreaterGreaterEqual,
  EqualEqual,
  BangEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  LessLess,
  GreaterGreater,
  AmpAmp,
  PipePipe,
  Bang,
  Amp,
  Pipe,
  Caret,
  Tilde,
};

struct Token {
  TokenKind kind = TokenKind::EndOfFile;
  std::string text;
  std::string string_value;
  long long integer_value = 0;
  double float_value = 0.0;
  uint32_t char_value = 0;
  std::string suffix;
  SourceLocation location;

  bool is(TokenKind expected) const { return kind == expected; }
};

std::string_view token_kind_name(TokenKind kind);
TokenKind keyword_kind(std::string_view text);
bool is_reserved_future_keyword(TokenKind kind);
std::string describe_token(const Token& token);

}  // namespace nirq
