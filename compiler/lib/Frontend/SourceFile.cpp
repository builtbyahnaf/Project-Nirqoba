#include "nirqoba/Frontend/SourceFile.h"

#include <algorithm>

namespace nirq {

SourceFile::SourceFile(std::string path, std::string text)
    : path_(std::move(path)), text_(std::move(text)) {
  line_starts_.push_back(0);
  for (std::size_t index = 0; index < text_.size(); ++index) {
    // CRLF input keeps its columns aligned: the '\r' stays on the line it ends
    // and the following '\n' starts the next one.
    if (text_[index] == '\n') line_starts_.push_back(index + 1);
  }
}

SourceLocation SourceFile::location(std::size_t offset, std::size_t length) const {
  // The offset may legitimately equal size() for an EOF token.
  const std::size_t clamped = std::min(offset, text_.size());
  const auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), clamped);
  const std::size_t line_index = static_cast<std::size_t>(it - line_starts_.begin()) - 1;
  SourceLocation location;
  location.file = path_;
  location.offset = clamped;
  location.line = line_index + 1;
  location.column = clamped - line_starts_[line_index] + 1;
  location.length = length;
  return location;
}

std::string_view SourceFile::line_text(std::size_t line) const {
  if (line == 0 || line > line_starts_.size()) return {};
  const std::size_t begin = line_starts_[line - 1];
  std::size_t end = (line < line_starts_.size()) ? line_starts_[line] : text_.size();
  while (end > begin && (text_[end - 1] == '\n' || text_[end - 1] == '\r')) --end;
  return std::string_view(text_).substr(begin, end - begin);
}

}  // namespace nirq
