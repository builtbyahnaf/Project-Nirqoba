#include "test_harness.h"

#include "nirqoba/runtime/args.h"

using namespace nirq;
using namespace std;

TEST(RuntimeArgsTest, EmptyArgumentsUseTheProcessDescriptor) {
  NrqStringArray* array = nrq_args_build(0, nullptr);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(nrq_args_length(array), int64_t{0});
  ASSERT_TRUE(nrq_args_get(array, 0) != nullptr);
  ASSERT_EQ(string(nrq_args_get(array, 0)), string(""));
  ASSERT_EQ(string(nrq_args_get(array, -1)), string(""));
}

TEST(RuntimeArgsTest, ArgumentsExcludeTheExecutablePath) {
  const char* argv[] = {"program.exe", "alpha", "beta"};
  NrqStringArray* array = nrq_args_build(3, argv);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(nrq_args_length(array), int64_t{2});
  ASSERT_EQ(string(nrq_args_get(array, 0)), string("alpha"));
  ASSERT_EQ(string(nrq_args_get(array, 1)), string("beta"));
  ASSERT_EQ(string(nrq_args_get(array, 2)), string(""));
}

TEST(RuntimeArgsTest, RebuildingRefreshesTheSameDescriptor) {
  const char* first[] = {"program.exe", "one"};
  const char* second[] = {"program.exe", "two", "three"};
  NrqStringArray* first_array = nrq_args_build(2, first);
  NrqStringArray* second_array = nrq_args_build(3, second);

  // One process has one argument array; a rebuild must not allocate a second
  // descriptor or retain the previous argv.
  ASSERT_EQ(first_array, second_array);
  ASSERT_EQ(nrq_args_length(second_array), int64_t{2});
  ASSERT_EQ(string(nrq_args_get(second_array, 0)), string("two"));
  ASSERT_EQ(string(nrq_args_get(second_array, 1)), string("three"));
}

TEST(RuntimeArgsTest, StringLengthHandlesNullAndText) {
  ASSERT_EQ(nrq_strlen(nullptr), int64_t{0});
  ASSERT_EQ(nrq_strlen(""), int64_t{0});
  ASSERT_EQ(nrq_strlen("phase 7"), int64_t{7});
}
