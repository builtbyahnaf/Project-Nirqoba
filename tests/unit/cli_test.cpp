#include "test_harness.h"

#include "nirqoba/CLI/CommandLine.h"
#include "nirqoba/CLI/Driver.h"

using namespace nirq;
using namespace std;

TEST(CliTest, ParseVersionAndHelp) {
  {
    const char* argv[] = {"nirqoba", "version"};
    CommandLineOptions opts;
    std::string err;
    ASSERT_TRUE(parse_command_line(2, argv, opts, err));
    ASSERT_EQ(opts.command, CommandKind::Version);
  }
  {
    const char* argv[] = {"nirqoba", "--help"};
    CommandLineOptions opts;
    std::string err;
    ASSERT_TRUE(parse_command_line(2, argv, opts, err));
    ASSERT_EQ(opts.command, CommandKind::Help);
  }
  {
    const char* argv[] = {"nirqoba", "help", "build"};
    CommandLineOptions opts;
    std::string err;
    ASSERT_TRUE(parse_command_line(3, argv, opts, err));
    ASSERT_EQ(opts.command, CommandKind::Help);
    ASSERT_EQ(opts.help_topic, "build");
  }
}

TEST(CliTest, ParseBuildCommand) {
  const char* argv[] = {"nirqoba", "build", "main.nrqba", "-o", "bin/main.exe", "--emit=obj"};
  CommandLineOptions opts;
  std::string err;
  ASSERT_TRUE(parse_command_line(6, argv, opts, err));
  ASSERT_EQ(opts.command, CommandKind::Build);
  ASSERT_EQ(opts.source_paths.size(), 1u);
  ASSERT_EQ(opts.source_paths[0], "main.nrqba");
  ASSERT_EQ(opts.output_path, "bin/main.exe");
  ASSERT_EQ(opts.emit, EmitKind::Obj);
}

TEST(CliTest, ParseRunCommandWithTrailingArgs) {
  const char* argv[] = {"nirqoba", "run", "prog.nrqba", "--", "arg1", "arg2"};
  CommandLineOptions opts;
  std::string err;
  ASSERT_TRUE(parse_command_line(6, argv, opts, err));
  ASSERT_EQ(opts.command, CommandKind::Run);
  ASSERT_EQ(opts.source_paths.size(), 1u);
  ASSERT_EQ(opts.source_paths[0], "prog.nrqba");
  ASSERT_EQ(opts.trailing_args.size(), 2u);
  ASSERT_EQ(opts.trailing_args[0], "arg1");
  ASSERT_EQ(opts.trailing_args[1], "arg2");
}

TEST(CliTest, CheckRejectsInvalidEmitKind) {
  const char* argv[] = {"nirqoba", "check", "main.nrqba", "--emit=mlir"};
  CommandLineOptions opts;
  std::string err;
  ASSERT_FALSE(parse_command_line(4, argv, opts, err));
}

TEST(CliTest, RejectsUnknownOption) {
  const char* argv[] = {"nirqoba", "build", "main.nrqba", "--unknown-flag"};
  CommandLineOptions opts;
  std::string err;
  ASSERT_FALSE(parse_command_line(4, argv, opts, err));
}

TEST(CliTest, ParsesOptimizationLevels) {
  const char* levels[] = {"-O0", "-O1", "-O2", "-O3", "-Os", "-Oz"};
  const OptimizationLevel expected[] = {
      OptimizationLevel::O0, OptimizationLevel::O1, OptimizationLevel::O2,
      OptimizationLevel::O3, OptimizationLevel::Os, OptimizationLevel::Oz};
  for (size_t index = 0; index < 6; ++index) {
    const char* argv[] = {"nirqoba", "build", "main.nrqba", levels[index]};
    CommandLineOptions opts;
    std::string err;
    ASSERT_TRUE(parse_command_line(4, argv, opts, err));
    ASSERT_EQ(opts.optimization_level, expected[index]);
  }

  {
    const char* argv[] = {"nirqoba", "build", "main.nrqba", "--opt-level=z"};
    CommandLineOptions opts;
    std::string err;
    ASSERT_TRUE(parse_command_line(4, argv, opts, err));
    ASSERT_EQ(opts.optimization_level, OptimizationLevel::Oz);
  }

  {
    const char* argv[] = {"nirqoba", "build", "main.nrqba", "-O9"};
    CommandLineOptions opts;
    std::string err;
    ASSERT_FALSE(parse_command_line(4, argv, opts, err));
    ASSERT_TRUE(err.find("invalid optimization level") != string::npos);
  }
}

TEST(CliTest, ParsesTargetSelection) {
  {
    const char* argv[] = {"nirqoba", "build", "main.nrqba", "--native",
                          "--target-features=+avx2"};
    CommandLineOptions opts;
    std::string err;
    ASSERT_TRUE(parse_command_line(5, argv, opts, err));
    ASSERT_TRUE(opts.host_cpu);
    ASSERT_EQ(opts.target_features, "+avx2");
  }
  {
    const char* argv[] = {"nirqoba", "build", "main.nrqba",
                          "--target-cpu=znver4"};
    CommandLineOptions opts;
    std::string err;
    ASSERT_TRUE(parse_command_line(4, argv, opts, err));
    ASSERT_EQ(opts.target_cpu, "znver4");
  }
}

TEST(CliTest, ParsesLlvmEmitVariants) {
  {
    const char* argv[] = {"nirqoba", "build", "main.nrqba", "--emit=llvm"};
    CommandLineOptions opts;
    std::string err;
    ASSERT_TRUE(parse_command_line(4, argv, opts, err));
    ASSERT_EQ(opts.emit, EmitKind::Llvm);
  }
  {
    const char* argv[] = {"nirqoba", "build", "main.nrqba", "--emit=llvm-raw"};
    CommandLineOptions opts;
    std::string err;
    ASSERT_TRUE(parse_command_line(4, argv, opts, err));
    ASSERT_EQ(opts.emit, EmitKind::LlvmRaw);
  }
}
