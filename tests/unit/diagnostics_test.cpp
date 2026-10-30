#include "test_harness.h"

#include "nirqoba/Diagnostics/Diagnostic.h"
#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Diagnostics/DiagnosticRenderer.h"
#include "nirqoba/Frontend/SourceFile.h"

using namespace nirq;

TEST(DiagnosticsTest, CollectsErrorsAndNotes) {
  DiagnosticEngine diag;
  SourceLocation loc;
  loc.file = "test.nrqba";
  loc.line = 10;
  loc.column = 5;
  loc.length = 3;

  diag.error("NRQ3011", loc, "undefined variable 'foo'");
  diag.note("NRQ0001", loc, "did you mean 'bar'?");

  ASSERT_TRUE(diag.has_errors());
  ASSERT_EQ(diag.error_count(), 1u);
  ASSERT_EQ(diag.diagnostics().size(), 2u);

  ASSERT_EQ(diag.diagnostics()[0].severity, Severity::Error);
  ASSERT_EQ(diag.diagnostics()[0].code, "NRQ3011");
  ASSERT_EQ(diag.diagnostics()[1].severity, Severity::Note);
}

TEST(DiagnosticsTest, RenderingWithSourceFile) {
  DiagnosticEngine diag;
  SourceFile src("example.nrqba", "let x = 1;\nlet y = z + 2;\n");
  SourceLocation loc = src.location(19, 1); // points to 'z'

  diag.error("NRQ3011", loc, "use of undeclared variable 'z'");

  // Render to stderr should not throw or crash
  render_diagnostics(diag, &src);
}
