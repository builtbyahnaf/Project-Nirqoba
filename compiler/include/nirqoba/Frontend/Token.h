// Token model for the Nirqoba lexer.
//
// The token set is the *whole* Phase 2 surface, not just Phase 1: the lexer
// recognizes every operator and future keyword now, and the parser rejects the
// ones Phase 1 does not implement with an explicit phase diagnostic. That keeps
// Phase 2 additive instead of a lexer rewrite (ROADMAP 3.3).
#pragma once

#include <cstddef>
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

  // Reserved for later phases: recognized, never parsed in Phase 1.
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

  // Operators (Phase 2 recognition set).
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
  PipeEqual,
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
  // Raw source text of the token. String literals keep their decoded value in
  // `string_value` and the escaped spelling in `text`.
  std::string text;
  // Decoded contents of a string literal (escapes resolved); empty otherwise.
  std::string string_value;
  // Value of an integer literal. Phase 1 accepts decimal only.
  long long integer_value = 0;
  SourceLocation location;

  bool is(TokenKind expected) const { return kind == expected; }
};

// Stable spelling of a token kind, used by diagnostics and the AST printer.
std::string_view token_kind_name(TokenKind kind);

// Keyword lookup for an identifier spelling. Returns Identifier when the
// spelling is not reserved.
TokenKind keyword_kind(std::string_view text);

// True for keywords that belong to a later phase and must be rejected there.
bool is_reserved_future_keyword(TokenKind kind);

// Human-readable name of a token for "... expected" messages: keywords and
// punctuation report their spelling, other tokens their kind name.
std::string describe_token(const Token& token);

}  // namespace nirq
