// Source locations for every diagnostic and typed-IR operation.
//
// A location is recorded against one owned source file. `line` and `column` are
// 1-based and derived from `offset` by SourceFile; `length` is the number of
// bytes the span covers so diagnostics can underline the whole token.
#pragma once

#include <cstddef>
#include <string>

namespace nirq {

struct SourceLocation {
  std::string file;
  std::size_t offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;
  std::size_t length = 0;

  bool is_valid() const { return !file.empty(); }

  // "file:line:column", the form used at the start of every rendered diagnostic.
  std::string to_string() const {
    return file + ":" + std::to_string(line) + ":" + std::to_string(column);
  }
};

}  // namespace nirq
