// DiagnosticRenderer.h — renders collected Diagnostics to stderr.
//
// The renderer formats each diagnostic as:
//
//   file:line:col: severity[NRQxxxx]: message
//   <source line>
//   <caret underline>
//
// It needs the SourceFile to fetch the source line for the underline.
// The engine is kept separate from rendering so unit tests can inspect
// diagnostics as data without performing any I/O.
#pragma once

#include <string>

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/SourceFile.h"

namespace nirq {

// Renders all diagnostics in `engine` to stderr. Call after every compilation
// stage that may have added diagnostics; duplicates are not filtered.
//
// `source` may be nullptr when no source file is available (e.g. for toolchain
// errors before a file is opened); in that case the source-line underline is
// skipped. Locations whose `file` field is empty are rendered without position.
void render_diagnostics(const DiagnosticEngine& engine, const SourceFile* source);

}  // namespace nirq
