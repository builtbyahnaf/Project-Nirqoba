#include "test_harness.h"

#include "nirqoba/Backend/Optimization.h"
#include "nirqoba/Backend/TargetInfo.h"
#include "nirqoba/Diagnostics/DiagnosticEngine.h"

using namespace nirq;
using namespace std;

namespace {

bool has_message_containing(const DiagnosticEngine& diagnostics,
                            string_view needle) {
  for (const Diagnostic& diagnostic : diagnostics.diagnostics()) {
    if (diagnostic.message.find(needle) != string::npos) return true;
  }
  return false;
}

OptimizationOptions options_for(TargetProfile profile) {
  OptimizationOptions options;
  options.target_profile = profile;
  return options;
}

}  // namespace

TEST(TargetTest, ParsesProfileNames) {
  for (const TargetProfile profile : {TargetProfile::Portable,
                                      TargetProfile::Native,
                                      TargetProfile::Explicit}) {
    TargetProfile parsed = TargetProfile::Explicit;
    ASSERT_TRUE(parse_target_profile(target_profile_name(profile), parsed));
    ASSERT_EQ(parsed, profile);
  }
  TargetProfile parsed = TargetProfile::Portable;
  ASSERT_FALSE(parse_target_profile("fastest", parsed));
  ASSERT_EQ(parsed, TargetProfile::Portable);
}

TEST(TargetTest, PortableUsesPublishedBaseline) {
  OptimizationOptions options;
  DiagnosticEngine diagnostics;
  TargetConfiguration config;
  ASSERT_TRUE(resolve_target_configuration(options, config, diagnostics));
  ASSERT_FALSE(diagnostics.has_errors());
  ASSERT_EQ(config.profile, TargetProfile::Portable);
  ASSERT_EQ(config.triple, default_target_triple());
  ASSERT_EQ(config.cpu, "generic");
  ASSERT_TRUE(config.features.empty());
  ASSERT_FALSE(config.from_host);
  ASSERT_EQ(describe_target_configuration(config),
            "portable (" + default_target_triple() + ", cpu generic)");
}

TEST(TargetTest, PortableAcceptsRedundantDefaultTriple) {
  OptimizationOptions options;
  options.target_triple = default_target_triple();
  DiagnosticEngine diagnostics;
  TargetConfiguration config;
  ASSERT_TRUE(resolve_target_configuration(options, config, diagnostics));
  ASSERT_EQ(config.triple, default_target_triple());
}

TEST(TargetTest, NativeDerivesCpuAndFeaturesFromHost) {
  OptimizationOptions options;
  options.target_profile = TargetProfile::Native;
  DiagnosticEngine diagnostics;
  TargetConfiguration config;
  ASSERT_TRUE(resolve_target_configuration(options, config, diagnostics));
  ASSERT_FALSE(diagnostics.has_errors());
  ASSERT_EQ(config.profile, TargetProfile::Native);
  ASSERT_EQ(config.triple, host_target_triple());
  ASSERT_TRUE(config.from_host);
  ASSERT_FALSE(config.cpu.empty());
  ASSERT_NE(config.cpu, "unknown");
  ASSERT_TRUE(describe_target_configuration(config).find("native") == 0);
}

TEST(TargetTest, ExplicitUsesRequestedCpuTripleAndFeatures) {
  OptimizationOptions options;
  options.target_profile = TargetProfile::Explicit;
  options.target_triple = "x86_64-unknown-linux-gnu";
  options.target_cpu = "znver4";
  options.target_features = "+avx2";
  DiagnosticEngine diagnostics;
  TargetConfiguration config;
  ASSERT_TRUE(resolve_target_configuration(options, config, diagnostics));
  ASSERT_FALSE(diagnostics.has_errors());
  ASSERT_EQ(config.profile, TargetProfile::Explicit);
  ASSERT_EQ(config.triple, "x86_64-unknown-linux-gnu");
  ASSERT_EQ(config.cpu, "znver4");
  ASSERT_EQ(config.features, "+avx2");
  ASSERT_FALSE(config.from_host);
}

TEST(TargetTest, ExplicitDefaultsToGenericCpuAndPublishedTriple) {
  OptimizationOptions options;
  options.target_profile = TargetProfile::Explicit;
  DiagnosticEngine diagnostics;
  TargetConfiguration config;
  ASSERT_TRUE(resolve_target_configuration(options, config, diagnostics));
  ASSERT_EQ(config.triple, default_target_triple());
  ASSERT_EQ(config.cpu, "generic");
  ASSERT_TRUE(config.features.empty());
}

TEST(TargetTest, PortableRejectsTargetOverrides) {
  {
    OptimizationOptions options;
    options.target_cpu = "znver4";
    DiagnosticEngine diagnostics;
    TargetConfiguration config;
    ASSERT_FALSE(resolve_target_configuration(options, config, diagnostics));
    ASSERT_TRUE(has_message_containing(diagnostics, "portable"));
  }
  {
    OptimizationOptions options;
    options.target_features = "+avx2";
    DiagnosticEngine diagnostics;
    TargetConfiguration config;
    ASSERT_FALSE(resolve_target_configuration(options, config, diagnostics));
    ASSERT_TRUE(has_message_containing(diagnostics, "target features"));
  }
  {
    OptimizationOptions options;
    options.target_triple = "x86_64-unknown-linux-gnu";
    DiagnosticEngine diagnostics;
    TargetConfiguration config;
    ASSERT_FALSE(resolve_target_configuration(options, config, diagnostics));
    ASSERT_TRUE(has_message_containing(diagnostics, "target triple"));
  }
}

TEST(TargetTest, NativeRejectsExplicitTargetOverrides) {
  {
    OptimizationOptions options = options_for(TargetProfile::Native);
    options.target_triple = "x86_64-unknown-linux-gnu";
    DiagnosticEngine diagnostics;
    TargetConfiguration config;
    ASSERT_FALSE(resolve_target_configuration(options, config, diagnostics));
    ASSERT_TRUE(has_message_containing(diagnostics, "derives the triple"));
  }
  {
    OptimizationOptions options = options_for(TargetProfile::Native);
    options.target_cpu = "znver4";
    DiagnosticEngine diagnostics;
    TargetConfiguration config;
    ASSERT_FALSE(resolve_target_configuration(options, config, diagnostics));
    ASSERT_TRUE(has_message_containing(diagnostics, "derives the CPU"));
  }
  {
    OptimizationOptions options = options_for(TargetProfile::Native);
    options.target_features = "+avx2";
    DiagnosticEngine diagnostics;
    TargetConfiguration config;
    ASSERT_FALSE(resolve_target_configuration(options, config, diagnostics));
    ASSERT_TRUE(has_message_containing(diagnostics, "derives features"));
  }
}

TEST(TargetTest, ExplicitRejectsUnknownCpuAndFeatures) {
  {
    OptimizationOptions options = options_for(TargetProfile::Explicit);
    options.target_cpu = "notacpu";
    DiagnosticEngine diagnostics;
    TargetConfiguration config;
    ASSERT_FALSE(resolve_target_configuration(options, config, diagnostics));
    ASSERT_TRUE(has_message_containing(diagnostics, "unknown target CPU"));
  }
  {
    OptimizationOptions options = options_for(TargetProfile::Explicit);
    options.target_features = "+notafeature";
    DiagnosticEngine diagnostics;
    TargetConfiguration config;
    ASSERT_FALSE(resolve_target_configuration(options, config, diagnostics));
    ASSERT_TRUE(has_message_containing(diagnostics, "unknown target feature"));
  }
  {
    OptimizationOptions options = options_for(TargetProfile::Explicit);
    options.target_features = "avx2";
    DiagnosticEngine diagnostics;
    TargetConfiguration config;
    ASSERT_FALSE(resolve_target_configuration(options, config, diagnostics));
    ASSERT_TRUE(has_message_containing(diagnostics, "malformed target feature"));
  }
}

TEST(TargetTest, ExplicitRejectsUnsupportedTriple) {
  OptimizationOptions options;
  options.target_profile = TargetProfile::Explicit;
  options.target_triple = "not-a-real-target";
  DiagnosticEngine diagnostics;
  TargetConfiguration config;
  ASSERT_FALSE(resolve_target_configuration(options, config, diagnostics));
  ASSERT_TRUE(has_message_containing(diagnostics, "unsupported target triple"));
}
