#include "test_harness.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <thread>
#include <vector>

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

TEST(FileTest, ConcurrentOperationsAndCloseKeepResourceLifetimeSafe) {
  void* handle = nrq_file_open_handle("NUL", 1);
  ASSERT_NE(handle, nullptr);
  constexpr int kWorkers = 4;
  std::atomic<int> ready = 0;
  std::atomic<bool> start = false;
  std::atomic<int> unexpected_results = 0;
  std::vector<std::thread> workers;
  for (int worker = 0; worker < kWorkers; ++worker) {
    workers.emplace_back([&] {
      ++ready;
      while (!start.load()) std::this_thread::yield();
      for (int attempt = 0; attempt < 500; ++attempt) {
        const int32_t result = nrq_file_write_handle(handle, "x");
        if (result != 0 && result != ERROR_INVALID_HANDLE)
          ++unexpected_results;
      }
    });
  }
  while (ready.load() != kWorkers) std::this_thread::yield();
  start = true;
  const int32_t close_result = nrq_file_close_handle(handle);
  for (std::thread& worker : workers) worker.join();

  ASSERT_EQ(close_result, int32_t{0});
  ASSERT_EQ(unexpected_results.load(), 0);
  ASSERT_EQ(nrq_file_write_handle(handle, "stale"),
            static_cast<int32_t>(ERROR_INVALID_HANDLE));
}
