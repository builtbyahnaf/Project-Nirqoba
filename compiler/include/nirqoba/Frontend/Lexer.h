// Maximally-munching scanner over one source file.
//
// The lexer never throws and never prints: every lexical problem becomes a
// diagnostic and an Invalid token, so the parser always sees a token stream.
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/SourceFile.h"
#include "nirqoba/Frontend/Token.h"

namespace nirq {

class Lexer {
 public:
  Lexer(const SourceFile& source, DiagnosticEngine& diagnostics)
      : source_(source), source_text_(source.view()), diagnostics_(diagnostics) {}

  // Scans the whole file. The returned vector always ends with one EndOfFile
  // token, which makes parser loops terminate without an extra guard.
  std::vector<Token> tokenize();

 private:
  Token scan_token();
  Token make_token(TokenKind kind, std::size_t start_offset);
  void scan_number(Token& token, std::size_t start_offset);
  void scan_string(Token& token, std::size_t start_offset);
  void scan_char(Token& token, std::size_t start_offset);
  void scan_identifier(Token& token, std::size_t start_offset);
  void skip_trivia();

  char peek(std::size_t lookahead = 0) const {
    const std::size_t index = offset_ + lookahead;
    return index < source_text_.size() ? source_text_[index] : '\0';
  }

  bool at_end() const { return offset_ >= source_text_.size(); }
  bool match(char expected);

  const SourceFile& source_;
  std::string_view source_text_;
  DiagnosticEngine& diagnostics_;
  std::size_t offset_ = 0;
};

}  // namespace nirq
