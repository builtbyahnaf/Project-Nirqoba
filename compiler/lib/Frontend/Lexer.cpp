#include "nirqoba/Frontend/Lexer.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>

namespace nirq {
using namespace std;

namespace {

bool is_identifier_start(char c) {
  return isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_identifier_continue(char c) {
  return isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_hex_digit(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_binary_digit(char c) { return c == '0' || c == '1'; }

}  // namespace

vector<Token> Lexer::tokenize() {
  vector<Token> tokens;
  while (true) {
    skip_trivia();
    Token token = scan_token();
    const bool at_end = token.kind == TokenKind::EndOfFile;
    tokens.push_back(move(token));
    if (at_end) break;
  }
  if (tokens.size() == 1) {
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
      while (!at_end() && peek() != '\n') ++offset_;
      continue;
    }
    if (c == '/' && peek(1) == '*') {
const size_t start = offset_;
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

Token Lexer::make_token(TokenKind kind, size_t start_offset) {
  Token token;
  token.kind = kind;
  token.text = string(source_text_.substr(start_offset, offset_ - start_offset));
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

  const size_t start = offset_;
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
  if (c == '\'') {
    Token token = make_token(TokenKind::CharLiteral, start);
    --offset_;
    scan_char(token, start);
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
    case '%':
      if (match('=')) return make_token(TokenKind::PercentEqual, start);
      return make_token(TokenKind::Percent, start);
    case '=':
      if (match('=')) return make_token(TokenKind::EqualEqual, start);
      return make_token(TokenKind::Equal, start);
    case '!':
      if (match('=')) return make_token(TokenKind::BangEqual, start);
      return make_token(TokenKind::Bang, start);
    case '<':
      if (match('<')) {
        if (match('=')) return make_token(TokenKind::LessLessEqual, start);
        return make_token(TokenKind::LessLess, start);
      }
      if (match('=')) return make_token(TokenKind::LessEqual, start);
      return make_token(TokenKind::Less, start);
    case '>':
      if (match('>')) {
        if (match('=')) return make_token(TokenKind::GreaterGreaterEqual, start);
        return make_token(TokenKind::GreaterGreater, start);
      }
      if (match('=')) return make_token(TokenKind::GreaterEqual, start);
      return make_token(TokenKind::Greater, start);
    case '&':
      if (match('&')) return make_token(TokenKind::AmpAmp, start);
      if (match('=')) return make_token(TokenKind::AmpEqual, start);
      return make_token(TokenKind::Amp, start);
    case '|':
      if (match('|')) return make_token(TokenKind::PipePipe, start);
      if (match('=')) return make_token(TokenKind::PipeEqual, start);
      return make_token(TokenKind::Pipe, start);
    case '^':
      if (match('=')) return make_token(TokenKind::CaretEqual, start);
      return make_token(TokenKind::Caret, start);
    case '~': return make_token(TokenKind::Tilde, start);
    default:
      break;
  }

  Token token;
  token.kind = TokenKind::Invalid;
  token.text = string(1, c);
  token.location = source_.location(start, 1);
  diagnostics_.error(diag::kUnexpectedCharacter, token.location,
                     "unexpected character '" + string(1, c) + "' in source file");
  return token;
}

void Lexer::scan_number(Token& token, size_t start_offset) {
  bool is_hex = false;
  bool is_binary = false;
  bool is_float = false;

  // Check prefix 0x / 0b
  if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
    is_hex = true;
    offset_ += 2;
    while (!at_end() && (is_hex_digit(peek()) || peek() == '_')) ++offset_;
  } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
    is_binary = true;
    offset_ += 2;
    while (!at_end() && (is_binary_digit(peek()) || peek() == '_')) ++offset_;
  } else {
    // Decimal or floating point
    while (!at_end() && (is_digit(peek()) || peek() == '_')) ++offset_;

    if (!at_end() && peek() == '.' && is_digit(peek(1))) {
      is_float = true;
      ++offset_;  // consume '.'
      while (!at_end() && (is_digit(peek()) || peek() == '_')) ++offset_;
    }

    if (!at_end() && (peek() == 'e' || peek() == 'E')) {
      is_float = true;
      ++offset_;  // consume 'e'
      if (!at_end() && (peek() == '+' || peek() == '-')) ++offset_;
      while (!at_end() && (is_digit(peek()) || peek() == '_')) ++offset_;
    }
  }

  // Type suffixes: i8, i16, i32, i64, u8, u16, u32, u64, f32, f64
  size_t suffix_start = offset_;
  if (!at_end() && (peek() == 'i' || peek() == 'u' || peek() == 'f')) {
    const size_t rem = source_text_.size() - offset_;
    if (rem >= 3 && (source_text_.substr(offset_, 3) == "i64" ||
                     source_text_.substr(offset_, 3) == "i32" ||
                     source_text_.substr(offset_, 3) == "i16" ||
                     source_text_.substr(offset_, 3) == "u64" ||
                     source_text_.substr(offset_, 3) == "u32" ||
                     source_text_.substr(offset_, 3) == "u16" ||
                     source_text_.substr(offset_, 3) == "f64" ||
                     source_text_.substr(offset_, 3) == "f32")) {
      offset_ += 3;
    } else if (rem >= 2 && (source_text_.substr(offset_, 2) == "i8" ||
                            source_text_.substr(offset_, 2) == "u8")) {
      offset_ += 2;
    }
  }

  const string_view full_text = source_text_.substr(start_offset, offset_ - start_offset);
  string_view num_part = full_text.substr(0, suffix_start - start_offset);
  string suffix(full_text.substr(suffix_start - start_offset));

  if (suffix == "f32" || suffix == "f64") {
    is_float = true;
  }

  token.text = string(full_text);
  token.suffix = move(suffix);
  token.location = source_.location(start_offset, full_text.size());

  // Clean underscores for parsing
  string clean_digits;
  for (char ch : num_part) {
    if (ch != '_') clean_digits.push_back(ch);
  }

  if (is_float) {
    token.kind = TokenKind::FloatLiteral;
    try {
      token.float_value = stod(clean_digits);
    } catch (...) {
      diagnostics_.error("NRQ1010", token.location, "invalid floating-point literal '" + token.text + "'");
      token.float_value = 0.0;
    }
    return;
  }

  token.kind = TokenKind::IntegerLiteral;
  int base = 10;
  string_view parse_digits = clean_digits;
  if (is_hex) {
    base = 16;
    parse_digits = parse_digits.substr(2);
  } else if (is_binary) {
    base = 2;
    parse_digits = parse_digits.substr(2);
  }

  unsigned long long raw_val = 0;
  auto [ptr, ec] = from_chars(parse_digits.data(), parse_digits.data() + parse_digits.size(), raw_val, base);
  if (ec != errc()) {
    diagnostics_.error("NRQ1009", token.location,
                       "integer literal '" + token.text + "' is too large for 64-bit integer");
    token.integer_value = 0;
    return;
  }
  token.integer_value = static_cast<long long>(raw_val);
}

void Lexer::scan_char(Token& token, size_t start_offset) {
  ++offset_;  // consume opening single quote
  if (at_end() || peek() == '\n') {
    diagnostics_.error("NRQ1011", source_.location(start_offset, offset_ - start_offset),
                       "unterminated character literal");
    token.kind = TokenKind::CharLiteral;
    return;
  }

  uint32_t val = 0;
  if (peek() == '\\') {
    ++offset_;
    if (at_end()) return;
    char escaped = peek();
    ++offset_;
    switch (escaped) {
      case 'n': val = '\n'; break;
      case 't': val = '\t'; break;
      case 'r': val = '\r'; break;
      case '\\': val = '\\'; break;
      case '\'': val = '\''; break;
      case '0': val = '\0'; break;
      default:
        diagnostics_.error(diag::kUnknownEscape, source_.location(start_offset, 2),
                           string("unknown escape sequence in character literal '\\") + escaped + "'");
        val = static_cast<uint32_t>(escaped);
        break;
    }
  } else {
    val = static_cast<unsigned char>(peek());
    ++offset_;
  }

  if (at_end() || peek() != '\'') {
    diagnostics_.error("NRQ1011", source_.location(start_offset, offset_ - start_offset),
                       "character literal must be terminated with a single quote");
  } else {
    ++offset_;  // consume closing single quote
  }

  token.kind = TokenKind::CharLiteral;
  token.text = string(source_text_.substr(start_offset, offset_ - start_offset));
  token.char_value = val;
  token.location = source_.location(start_offset, offset_ - start_offset);
}

void Lexer::scan_identifier(Token& token, size_t start_offset) {
  while (!at_end() && is_identifier_continue(peek())) ++offset_;
  const string_view spelling = source_text_.substr(start_offset, offset_ - start_offset);
  token.kind = keyword_kind(spelling);
  token.text = string(spelling);
  token.location = source_.location(start_offset, spelling.size());
}

void Lexer::scan_string(Token& token, size_t start_offset) {
  ++offset_;
  string value;
  bool terminated = false;
  while (!at_end()) {
    const char c = peek();
    if (c == '"') {
      ++offset_;
      terminated = true;
      break;
    }
    if (c == '\n') {
      break;
    }
    if (c != '\\') {
      value.push_back(c);
      ++offset_;
      continue;
    }

    const size_t escape_offset = offset_;
    ++offset_;
    if (at_end()) break;
    const char escaped = peek();
    ++offset_;
    switch (escaped) {
      case 'n': value.push_back('\n'); break;
      case 't': value.push_back('\t'); break;
      case 'r': value.push_back('\r'); break;
      case '\\': value.push_back('\\'); break;
      case '"': value.push_back('"'); break;
      case '0': value.push_back('\0'); break;
      default:
        diagnostics_.error(diag::kUnknownEscape, source_.location(escape_offset, 2),
                           string("unknown escape sequence '\\") + escaped +
                               "'; supported escapes are \\n, \\t, \\r, \\\\, \\\" and \\0");
        value.push_back(escaped);
        break;
    }
  }

  if (!terminated) {
    diagnostics_.error(diag::kUnterminatedString, source_.location(start_offset, offset_ - start_offset),
                       "unterminated string literal");
  }
  token.kind = TokenKind::StringLiteral;
  token.text = string(source_text_.substr(start_offset, offset_ - start_offset));
  token.string_value = move(value);
  token.location = source_.location(start_offset, offset_ - start_offset);
}

}  // namespace nirq
