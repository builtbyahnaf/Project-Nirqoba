// Chi-1b, runtime half: owned text buffers.
//
// The language binding is not wired yet (see the phase report), but the runtime
// contract it will depend on is testable on its own: every successful read hands
// out exactly one buffer, a failed read still hands out an owned empty one so the
// destructor never has a null to special-case, and release returns the live count
// to where it started.

#include "test_harness.h"

#include <cstdint>
#include <cstring>

#include "nirqoba/runtime/file.h"

namespace {

constexpr const char* kMissingPath =
    "tests/integration/positive/no-such-file-for-owned-text.txt";

}  // namespace

TEST(FileOwnedTextTest, AMissingFileStillYieldsAnOwnedEmptyString) {
  const int32_t before = nrq_file_text_live_count();
  char* text = nrq_file_read_text_owned(kMissingPath);
  ASSERT_NE(text, nullptr);
  ASSERT_EQ(text[0], '\0');
  ASSERT_EQ(nrq_file_text_live_count(), before + 1);

  nrq_file_text_release(text);
  ASSERT_EQ(nrq_file_text_live_count(), before);
}

TEST(FileOwnedTextTest, AnEmptyPathAllocatesNothingExtra) {
  const int32_t before = nrq_file_text_live_count();
  char* text = nrq_file_read_text_owned("");
  ASSERT_NE(text, nullptr);
  ASSERT_EQ(std::strlen(text), static_cast<size_t>(0));
  ASSERT_EQ(nrq_file_text_live_count(), before + 1);
  nrq_file_text_release(text);
  ASSERT_EQ(nrq_file_text_live_count(), before);
}

TEST(FileOwnedTextTest, ARepeatedReadReleaseCycleDoesNotGrowTheLiveCount) {
  const int32_t before = nrq_file_text_live_count();
  for (int index = 0; index < 2000; ++index) {
    char* text = nrq_file_read_text_owned(kMissingPath);
    ASSERT_NE(text, nullptr);
    nrq_file_text_release(text);
  }
  // The whole point of the owned reader: two thousand reads cost no more live
  // buffers than zero reads did.
  ASSERT_EQ(nrq_file_text_live_count(), before);
}
