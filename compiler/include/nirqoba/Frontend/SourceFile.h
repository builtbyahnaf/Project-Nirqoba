// Owns the text of one source file and converts byte offsets to locations.
//
// Line and column indexes are computed once at construction, so every later
// query is O(log n) instead of rescanning the buffer. Columns count bytes
// (documented in docs/architecture/01-architecture.md); grapheme-accurate
// columns are deferred to the Unicode work in Phase 4.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "nirqoba/Diagnostics/SourceLocation.h"

namespace nirq {

class SourceFile {
 public:
  // `path` is used verbatim in diagnostics so error text matches the path the
  // user typed. `text` is moved into the object and must outlive all tokens.
  SourceFile(std::string path, std::string text);

  const std::string& path() const { return path_; }
  const std::string& text() const { return text_; }
  std::string_view view() const { return text_; }
  std::size_t size() const { return text_.size(); }

  // Location for a span starting at `offset` covering `length` bytes.
  SourceLocation location(std::size_t offset, std::size_t length = 1) const;

  // Text of the 1-based `line`, without its terminating newline. Returns an
  // empty string when the line is out of range.
  std::string_view line_text(std::size_t line) const;

  std::size_t line_count() const { return line_starts_.size(); }

 private:
  std::string path_;
  std::string text_;
  std::vector<std::size_t> line_starts_;
};

}  // namespace nirq
