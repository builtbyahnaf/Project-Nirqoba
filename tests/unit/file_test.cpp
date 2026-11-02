#include "test_harness.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nirqoba/runtime/file.h"

TEST(FileTest, ClosedResourceTokensAreNeverReused) {
  void* stale = nrq_file_open_handle("NUL", 1);
  ASSERT_NE(stale, nullptr);
  ASSERT_EQ(nrq_file_close_handle(stale), int32_t{0});

  void* current = nrq_file_open_handle("NUL", 1);
  ASSERT_NE(current, nullptr);
  ASSERT_NE(current, stale);
  ASSERT_EQ(nrq_file_write_handle(stale, "stale"),
            static_cast<int32_t>(ERROR_INVALID_HANDLE));
  ASSERT_EQ(nrq_file_close_handle(stale),
            static_cast<int32_t>(ERROR_INVALID_HANDLE));
  ASSERT_EQ(nrq_file_close_handle(current), int32_t{0});
}
