#include "nirqoba/Diagnostics/DiagnosticRenderer.h"

#include <cstdio>
#include <string>

namespace nirq {

// Writes one rendered diagnostic to stderr. The format matches what other
// LLVM-based compilers produce so IDE integrations parse it correctly:
//
//   path/to/file.nrqba:4:10: error[NRQ3011]: use of undeclared variable 'x'
//   let y = x + 1;
//            ^
//
// Rules:
//   - Severity and code always appear even when location is unknown.
//   - The source-line underline uses '^' for the first byte, '~' for the rest
//     of the token span, matching clang's style.
//   - The caret aligns on the column byte-count, not grapheme count (Phase 4
//     will refine this for Unicode identifiers).
//   - Notes are indented to associate them visually with the preceding error.
static void render_one(const Diagnostic& diag, const SourceFile* source) {
  // "  note: ..." vs "error[NRQxxxx]: ..."
  const bool is_note = (diag.severity == Severity::Note);

  std::string header;
  if (diag.location.is_valid()) {
    header = diag.location.to_string() + ": ";
  }
  header += std::string(severity_name(diag.severity));
  if (!diag.code.empty()) {
    header += "[" + diag.code + "]";
  }
  header += ": " + diag.message;

  if (is_note) {
    // Indent notes so they visually attach to the preceding diagnostic.
    std::fprintf(stderr, "  %s\n", header.c_str());
  } else {
    std::fprintf(stderr, "%s\n", header.c_str());
  }

  // Source-line underline — only when we have a valid location with a source
  // file attached and the location refers to a real line.
  if (source != nullptr && diag.location.is_valid() && diag.location.line > 0) {
    const std::string_view line_text = source->line_text(diag.location.line);
    if (!line_text.empty()) {
      std::fprintf(stderr, "%.*s\n", static_cast<int>(line_text.size()), line_text.data());

      // Column is 1-based; clamp so we never underline past end of line.
      const std::size_t col = diag.location.column > 0 ? diag.location.column - 1 : 0;
      const std::size_t span = diag.location.length > 0 ? diag.location.length : 1;

      std::string underline(col, ' ');
      underline.push_back('^');
      if (span > 1) {
        underline.append(span - 1, '~');
      }
      std::fprintf(stderr, "%s\n", underline.c_str());
    }
  }
}

void render_diagnostics(const DiagnosticEngine& engine, const SourceFile* source) {
  for (const Diagnostic& diag : engine.diagnostics()) {
    render_one(diag, source);
  }
}

}  // namespace nirq
