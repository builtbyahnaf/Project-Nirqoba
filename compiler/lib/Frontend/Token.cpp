#include "nirqoba/Frontend/Token.h"

namespace nirq {
using namespace std;

string_view token_kind_name(TokenKind kind) {
  switch (kind) {
    case TokenKind::EndOfFile: return "end of file";
    case TokenKind::Invalid: return "invalid token";
    case TokenKind::IntegerLiteral: return "integer literal";
    case TokenKind::FloatLiteral: return "float literal";
    case TokenKind::CharLiteral: return "character literal";
    case TokenKind::StringLiteral: return "string literal";
    case TokenKind::Identifier: return "identifier";
    case TokenKind::KwFunction: return "function";
    case TokenKind::KwLet: return "let";
    case TokenKind::KwVar: return "var";
    case TokenKind::KwConst: return "const";
    case TokenKind::KwReturn: return "return";
    case TokenKind::KwPrint: return "print";
    case TokenKind::KwInt: return "int";
    case TokenKind::KwI8: return "i8";
    case TokenKind::KwI16: return "i16";
    case TokenKind::KwI32: return "i32";
    case TokenKind::KwI64: return "i64";
    case TokenKind::KwU8: return "u8";
    case TokenKind::KwU16: return "u16";
    case TokenKind::KwU32: return "u32";
    case TokenKind::KwU64: return "u64";
    case TokenKind::KwByte: return "byte";
    case TokenKind::KwChar: return "char";
    case TokenKind::KwF32: return "f32";
    case TokenKind::KwF64: return "f64";
    case TokenKind::KwAs: return "as";
    case TokenKind::KwString: return "string";
    case TokenKind::KwBool: return "bool";
    case TokenKind::KwVoid: return "void";
    case TokenKind::KwTrue: return "true";
    case TokenKind::KwFalse: return "false";
    case TokenKind::KwIf: return "if";
    case TokenKind::KwElse: return "else";
    case TokenKind::KwWhile: return "while";
    case TokenKind::KwFor: return "for";
    case TokenKind::KwBreak: return "break";
    case TokenKind::KwContinue: return "continue";
    case TokenKind::KwStruct: return "struct";
    case TokenKind::KwModule: return "module";
    case TokenKind::KwImport: return "import";
    case TokenKind::KwPublic: return "public";
    case TokenKind::KwPrivate: return "private";
    case TokenKind::KwProtected: return "protected";
    case TokenKind::KwAsync: return "async";
    case TokenKind::KwAwait: return "await";
    case TokenKind::KwFn: return "fn";
    case TokenKind::KwDefer: return "defer";
    case TokenKind::LParen: return "(";
    case TokenKind::RParen: return ")";
    case TokenKind::LBrace: return "{";
    case TokenKind::RBrace: return "}";
    case TokenKind::LBracket: return "[";
    case TokenKind::RBracket: return "]";
    case TokenKind::Comma: return ",";
    case TokenKind::Semicolon: return ";";
    case TokenKind::Colon: return ":";
    case TokenKind::Dot: return ".";
    case TokenKind::Arrow: return "->";
    case TokenKind::Plus: return "+";
    case TokenKind::Minus: return "-";
    case TokenKind::Star: return "*";
    case TokenKind::StarStar: return "**";
    case TokenKind::Slash: return "/";
    case TokenKind::Percent: return "%";
    case TokenKind::Equal: return "=";
    case TokenKind::PlusEqual: return "+=";
    case TokenKind::MinusEqual: return "-=";
    case TokenKind::StarEqual: return "*=";
    case TokenKind::SlashEqual: return "/=";
    case TokenKind::PercentEqual: return "%=";
    case TokenKind::PipeEqual: return "|=";
    case TokenKind::AmpEqual: return "&=";
    case TokenKind::CaretEqual: return "^=";
    case TokenKind::LessLessEqual: return "<<=";
    case TokenKind::GreaterGreaterEqual: return ">>=";
    case TokenKind::EqualEqual: return "==";
    case TokenKind::BangEqual: return "!=";
    case TokenKind::Less: return "<";
    case TokenKind::LessEqual: return "<=";
    case TokenKind::Greater: return ">";
    case TokenKind::GreaterEqual: return ">=";
    case TokenKind::LessLess: return "<<";
    case TokenKind::GreaterGreater: return ">>";
    case TokenKind::AmpAmp: return "&&";
    case TokenKind::PipePipe: return "||";
    case TokenKind::Bang: return "!";
    case TokenKind::Amp: return "&";
    case TokenKind::Pipe: return "|";
    case TokenKind::Caret: return "^";
    case TokenKind::Tilde: return "~";
  }
  return "unknown token";
}

TokenKind keyword_kind(string_view text) {
  struct Entry { string_view spelling; TokenKind kind; };
  static constexpr Entry kKeywords[] = {
      {"function", TokenKind::KwFunction}, {"let", TokenKind::KwLet},
      {"var", TokenKind::KwVar},           {"const", TokenKind::KwConst},
      {"return", TokenKind::KwReturn},     {"print", TokenKind::KwPrint},
      {"int", TokenKind::KwInt},           {"string", TokenKind::KwString},
      {"bool", TokenKind::KwBool},         {"void", TokenKind::KwVoid},
      {"true", TokenKind::KwTrue},         {"false", TokenKind::KwFalse},
      {"i8", TokenKind::KwI8},             {"i16", TokenKind::KwI16},
      {"i32", TokenKind::KwI32},           {"i64", TokenKind::KwI64},
      {"u8", TokenKind::KwU8},             {"u16", TokenKind::KwU16},
      {"u32", TokenKind::KwU32},           {"u64", TokenKind::KwU64},
      {"byte", TokenKind::KwByte},         {"char", TokenKind::KwChar},
      {"f32", TokenKind::KwF32},           {"f64", TokenKind::KwF64},
      {"as", TokenKind::KwAs},
      {"if", TokenKind::KwIf},             {"else", TokenKind::KwElse},
      {"while", TokenKind::KwWhile},       {"for", TokenKind::KwFor},
      {"break", TokenKind::KwBreak},       {"continue", TokenKind::KwContinue},
      {"struct", TokenKind::KwStruct},     {"module", TokenKind::KwModule},
      {"import", TokenKind::KwImport},     {"public", TokenKind::KwPublic},
      {"private", TokenKind::KwPrivate},   {"protected", TokenKind::KwProtected},
      {"async", TokenKind::KwAsync},       {"await", TokenKind::KwAwait},
      {"fn", TokenKind::KwFn},
      {"defer", TokenKind::KwDefer},
  };
  for (const Entry& entry : kKeywords) {
    if (entry.spelling == text) return entry.kind;
  }
  return TokenKind::Identifier;
}

bool is_reserved_future_keyword(TokenKind kind) {
  switch (kind) {
    case TokenKind::KwIf:
    case TokenKind::KwElse:
    case TokenKind::KwWhile:
    case TokenKind::KwFor:
    case TokenKind::KwBreak:
    case TokenKind::KwContinue:
    case TokenKind::KwStruct:
    case TokenKind::KwModule:
    case TokenKind::KwImport:
    case TokenKind::KwPublic:
    case TokenKind::KwPrivate:
    case TokenKind::KwProtected:
    case TokenKind::KwAsync:
    case TokenKind::KwAwait:
    case TokenKind::KwFn:
      return true;
    default:
      return false;
  }
}

string describe_token(const Token& token) {
  if (token.kind == TokenKind::IntegerLiteral) return "integer literal '" + token.text + "'";
  if (token.kind == TokenKind::FloatLiteral) return "float literal '" + token.text + "'";
  if (token.kind == TokenKind::CharLiteral) return "character literal '" + token.text + "'";
  if (token.kind == TokenKind::StringLiteral) return "string literal";
  if (token.kind == TokenKind::Identifier) return "identifier '" + token.text + "'";
  return "'" + string(token_kind_name(token.kind)) + "'";
}

}  // namespace nirq
