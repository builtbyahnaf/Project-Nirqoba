#include "test_harness.h"

#include "nirqoba/Backend/Optimization.h"

using namespace nirq;
using namespace std;

TEST(OptimizationTest, ParsesEverySupportedLevel) {
  struct Case {
    const char* text;
    OptimizationLevel level;
    const char* name;
  };
  const Case cases[] = {
      {"0", OptimizationLevel::O0, "-O0"},
      {"1", OptimizationLevel::O1, "-O1"},
      {"2", OptimizationLevel::O2, "-O2"},
      {"3", OptimizationLevel::O3, "-O3"},
      {"s", OptimizationLevel::Os, "-Os"},
      {"S", OptimizationLevel::Os, "-Os"},
      {"z", OptimizationLevel::Oz, "-Oz"},
      {"Z", OptimizationLevel::Oz, "-Oz"},
      {"-O2", OptimizationLevel::O2, "-O2"},
      {"O3", OptimizationLevel::O3, "-O3"},
  };
  for (const Case& test : cases) {
    OptimizationLevel parsed = OptimizationLevel::O0;
    ASSERT_TRUE(parse_optimization_level(test.text, parsed));
    ASSERT_EQ(parsed, test.level);
    ASSERT_EQ(optimization_level_name(parsed), string_view(test.name));
  }
}

TEST(OptimizationTest, RejectsInvalidLevels) {
  OptimizationLevel parsed = OptimizationLevel::O2;
  ASSERT_FALSE(parse_optimization_level("", parsed));
  ASSERT_FALSE(parse_optimization_level("4", parsed));
  ASSERT_FALSE(parse_optimization_level("-O9", parsed));
  ASSERT_FALSE(parse_optimization_level("fast", parsed));
  ASSERT_EQ(parsed, OptimizationLevel::O2);
}

TEST(OptimizationTest, ParsesIpoModes) {
  struct Case {
    const char* text;
    IpoMode mode;
    const char* name;
  };
  const Case cases[] = {
      {"standard", IpoMode::Standard, "standard"},
      {"aggressive", IpoMode::Aggressive, "aggressive"},
      {"minimal", IpoMode::Minimal, "minimal"},
  };
  for (const Case& test : cases) {
    IpoMode parsed = IpoMode::Standard;
    ASSERT_TRUE(parse_ipo_mode(test.text, parsed));
    ASSERT_EQ(parsed, test.mode);
    ASSERT_EQ(ipo_mode_name(parsed), string_view(test.name));
  }

  IpoMode parsed = IpoMode::Standard;
  ASSERT_FALSE(parse_ipo_mode("", parsed));
  ASSERT_FALSE(parse_ipo_mode("full", parsed));
  ASSERT_FALSE(parse_ipo_mode("Standard", parsed));
  ASSERT_EQ(parsed, IpoMode::Standard);
}
