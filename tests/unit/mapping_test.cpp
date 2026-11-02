#include "test_harness.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <thread>
#include <vector>

#include "nirqoba/runtime/allocator.h"

TEST(MappingTest, ConcurrentSizeQueriesRemainSafeDuringUnmap) {
  wchar_t directory[MAX_PATH]{};
  ASSERT_TRUE(GetTempPathW(MAX_PATH, directory) != 0);
  wchar_t path[MAX_PATH]{};
  ASSERT_TRUE(GetTempFileNameW(directory, L"nrq", 0, path) != 0);
  HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  constexpr char contents[] = "map";
  DWORD written = 0;
  const BOOL write_succeeded = WriteFile(file, contents, sizeof(contents),
                                         &written, nullptr);
  CloseHandle(file);
  ASSERT_TRUE(write_succeeded != 0);
  ASSERT_EQ(written, static_cast<DWORD>(sizeof(contents)));

  const int path_bytes = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0,
                                              nullptr, nullptr);
  ASSERT_TRUE(path_bytes > 1);
  std::vector<char> utf8_path(static_cast<size_t>(path_bytes));
  ASSERT_TRUE(WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8_path.data(),
                                  path_bytes, nullptr, nullptr) != 0);
  void* mapping = nrq_memory_map_file(utf8_path.data(), 0);
  ASSERT_NE(mapping, nullptr);
  ASSERT_EQ(nrq_memory_mapping_size(mapping),
            static_cast<uint64_t>(sizeof(contents)));

  constexpr int kWorkers = 4;
  std::atomic<int> ready = 0;
  std::atomic<bool> start = false;
  std::atomic<int> invalid_sizes = 0;
  std::vector<std::thread> workers;
  for (int worker = 0; worker < kWorkers; ++worker) {
    workers.emplace_back([&] {
      ++ready;
      while (!start.load()) std::this_thread::yield();
      for (int attempt = 0; attempt < 2000; ++attempt) {
        const uint64_t size = nrq_memory_mapping_size(mapping);
        if (size != 0 && size != sizeof(contents)) ++invalid_sizes;
      }
    });
  }
  while (ready.load() != kWorkers) std::this_thread::yield();
  start = true;
  const int32_t unmap_result = nrq_memory_unmap_file(mapping);
  for (std::thread& worker : workers) worker.join();

  ASSERT_EQ(unmap_result, int32_t{1});
  ASSERT_EQ(invalid_sizes.load(), 0);
  ASSERT_EQ(nrq_memory_mapping_size(mapping), uint64_t{0});
  ASSERT_EQ(nrq_memory_unmap_file(mapping), int32_t{0});
  ASSERT_TRUE(DeleteFileW(path) != 0);
}
