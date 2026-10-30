#include "nirqoba/Frontend/Lexer.h"

#include <cctype>
#include <limits>

namespace nirq {
namespace {

bool is_identifier_start(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_identifier_continue(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;
  while (true) {
    skip_trivia();
    Token token = scan_token();
    const bool at_end = token.kind == TokenKind::EndOfFile;
    tokens.push_back(std::move(token));
    if (at_end) break;
  }
  if (tokens.size() == 1) {
    // A file containing only comments or whitespace has no tokens; report it
    // here so the driver never reaches the parser with an empty program.
    diagnostics_.error(diag::kEmptySource, source_.location(0, 0),
                       "source file contains no tokens");
  }
  return tokens;
}

void Lexer::skip_trivia() {
  while (!at_end()) {
    const char c = peek();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++offset_;
      continue;
    }
    if (c == '/' && peek(1) == '/') {
      // Line comments run to the newline (or end of file) and are not tokens.
      while (!at_end() && peek() != '\n') ++offset_;
      continue;
    }
    if (c == '/' && peek(1) == '*') {
      const std::size_t start = offset_;
      offset_ += 2;
      bool closed = false;
      while (!at_end()) {
        if (peek() == '*' && peek(1) == '/') {
          offset_ += 2;
          closed = true;
          break;
        }
        ++offset_;
      }
      if (!closed) {
        diagnostics_.error(diag::kUnterminatedString, source_.location(start, offset_ - start),
                           "unterminated block comment");
      }
      continue;
    }
    break;
  }
}

bool Lexer::match(char expected) {
  if (peek() != expected) return false;
  ++offset_;
  return true;
}

Token Lexer::make_token(TokenKind kind, std::size_t start_offset) {
  Token token;
  token.kind = kind;
  token.text = std::string(source_text_.substr(start_offset, offset_ - start_offset));
  token.location = source_.location(start_offset, offset_ - start_offset);
  return token;
}

Token Lexer::scan_token() {
  if (at_end()) {
    Token token;
    token.kind = TokenKind::EndOfFile;
    token.location = source_.location(offset_, 0);
    return token;
  }

  const std::size_t start = offset_;
  const char c = source_text_[offset_++];

  if (is_digit(c)) {
    Token token = make_token(TokenKind::IntegerLiteral, start);
    --offset_;
    scan_number(token, start);
    return token;
  }
  if (is_identifier_start(c)) {
    Token token = make_token(TokenKind::Identifier, start);
    --offset_;
    scan_identifier(token, start);
    return token;
  }
  if (c == '"') {
    Token token = make_token(TokenKind::StringLiteral, start);
    --offset_;
    scan_string(token, start);
    return token;
  }

  switch (c) {
    case '(': return make_token(TokenKind::LParen, start);
    case ')': return make_token(TokenKind::RParen, start);
    case '{': return make_token(TokenKind::LBrace, start);
    case '}': return make_token(TokenKind::RBrace, start);
    case '[': return make_token(TokenKind::LBracket, start);
    case ']': return make_token(TokenKind::RBracket, start);
    case ',': return make_token(TokenKind::Comma, start);
    case ';': return make_token(TokenKind::Semicolon, start);
    case ':': return make_token(TokenKind::Colon, start);
    case '.': return make_token(TokenKind::Dot, start);
    case '+':
      if (match('=')) return make_token(TokenKind::PlusEqual, start);
      return make_token(TokenKind::Plus, start);
    case '-':
      if (match('=')) return make_token(TokenKind::MinusEqual, start);
      if (match('>')) return make_token(TokenKind::Arrow, start);
      return make_token(TokenKind::Minus, start);
    case '*':
      if (match('*')) return make_token(TokenKind::StarStar, start);
      if (match('=')) return make_token(TokenKind::StarEqual, start);
      return make_token(TokenKind::Star, start);
    case '/':
      if (match('=')) return make_token(TokenKind::SlashEqual, start);
      return make_token(TokenKind::Slash, start);
    case '%': return make_token(TokenKind::Percent, start);
    case '=':
      if (match('=')) return make_token(TokenKind::EqualEqual, start);
      return make_token(TokenKind::Equal, start);
    case '!':
      if (match('=')) return make_token(TokenKind::BangEqual, start);
      return make_token(TokenKind::Bang, start);
    case '<':
      if (match('<')) return make_token(TokenKind::LessLess, start);
      if (match('=')) return make_token(TokenKind::LessEqual, start);
      return make_token(TokenKind::Less, start);
    case '>':
      if (match('>')) return make_token(TokenKind::GreaterGreater, start);
      if (match('=')) return make_token(TokenKind::GreaterEqual, start);
      return make_token(TokenKind::Greater, start);
    case '&':
      if (match('&')) return make_token(TokenKind::AmpAmp, start);
      return make_token(TokenKind::Amp, start);
    case '|':
      if (match('|')) return make_token(TokenKind::PipePipe, start);
      if (match('=')) return make_token(TokenKind::PipeEqual, start);
      return make_token(TokenKind::Pipe, start);
    case '^': return make_token(TokenKind::Caret, start);
    case '~': return make_token(TokenKind::Tilde, start);
    default:
      break;
  }

  // Unknown byte: report it, emit Invalid and keep scanning so one stray
  // character does not hide the rest of the file's diagnostics.
  Token token;
  token.kind = TokenKind::Invalid;
  token.text = std::string(1, c);
  token.location = source_.location(start, 1);
  diagnostics_.error(diag::kUnexpectedCharacter, token.location,
                     "unexpected character '" + std::string(1, c) + "' in source file");
  return token;
}

void Lexer::scan_number(Token& token, std::size_t start_offset) {
  // decimal_only: Nirqoba Phase 1 accepts decimal integers. Hex/binary
  // literals are part of Phase 2 and are rejected here rather than misread.
  while (!at_end() && is_digit(peek())) ++offset_;
  if (!at_end() && (peek() == 'x' || peek() == 'X' || peek() == 'b' || peek() == 'B')) {
    diagnostics_.error(diag::kNotImplementedInPhase1, source_.location(start_offset, offset_ - start_offset),
                       "non-decimal integer literals are specified for Phase 2");
  }
  const std::string_view digits = source_text_.substr(start_offset, offset_ - start_offset);
  token.kind = TokenKind::IntegerLiteral;
  token.text = std::string(digits);
  token.location = source_.location(start_offset, digits.size());

  long long value = 0;
  for (const char digit : digits) {
    if (value > (std::numeric_limits<long long>::max() - (digit - '0')) / 10) {
      diagnostics_.error("NRQ1009", token.location,
                         "integer literal '" + std::string(digits) + "' is too large for int (64-bit)");
      token.integer_value = 0;
      return;
    }
    value = value * 10 + (digit - '0');
  }
  token.integer_value = value;
}

void Lexer::scan_identifier(Token& token, std::size_t start_offset) {
  while (!at_end() && is_identifier_continue(peek())) ++offset_;
  const std::string_view spelling = source_text_.substr(start_offset, offset_ - start_offset);
  token.kind = keyword_kind(spelling);
  token.text = std::string(spelling);
  token.location = source_.location(start_offset, spelling.size());
}

void Lexer::scan_string(Token& token, std::size_t start_offset) {
  // The opening quote is consumed here; `start_offset` still points at it.
  ++offset_;
  std::string value;
  bool terminated = false;
  while (!at_end()) {
    const char c = peek();
    if (c == '"') {
      ++offset_;
      terminated = true;
      break;
    }
    if (c == '\n') {
      // Phase 1 has no multi-line string literals; stopping at the newline
      // keeps the diagnostic anchored on the line the user actually wrote.
      break;
    }
    if (c != '\') {
      value.push_back(c);
      ++offset_;
      continue;
    }

    const std::size_t escape_offset = offset_;
    ++offset_;
    if (at_end()) break;
    const char escaped = peek();
    ++offset_;
    switch (escaped) {
      case 'n': value.push_back('\n'); break;
      case 't': value.push_back('\t'); break;
      case 'r': value.push_back('\r'); break;
      case '\': value.push_back('\'); break;
      case '"': value.push_back('"'); break;
      case '0': value.push_back('\0'); break;
      default:
        diagnostics_.error(diag::kUnknownEscape, source_.location(escape_offset, 2),
                           std::string("unknown escape sequence '\\") + escaped +
                               "'; supported escapes are \n, \t, \r, \\, \\\" and \0");
        value.push_back(escaped);
        break;
    }
  }

  if (!terminated) {
    diagnostics_.error(diag::kUnterminatedString, source_.location(start_offset, offset_ - start_offset),
                       "unterminated string literal");
  }
  token.kind = TokenKind::StringLiteral;
  token.text = std::string(source_text_.substr(start_offset, offset_ - start_offset));
  token.string_value = std::move(value);
  token.location = source_.location(start_offset, offset_ - start_offset);
}

}  // namespace nirq
