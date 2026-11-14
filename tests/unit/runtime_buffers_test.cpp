#include "test_harness.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "nirqoba/runtime/file.h"
#include "nirqoba/runtime/io.h"
#include "runtime_buffers.h"

using namespace nirq;
using namespace std;

namespace {
class TemporaryTextFile {
 public:
  explicit TemporaryTextFile(const string& contents) {
    static unsigned counter = 0;
    path_ = filesystem::temp_directory_path() /
            ("nirqoba-buffer-test-" + to_string(++counter) + ".txt");
    ofstream out(path_, ios::binary | ios::trunc);
    out << contents;
  }
  ~TemporaryTextFile() {
    error_code error;
    filesystem::remove(path_, error);
  }
  const filesystem::path& path() const { return path_; }

 private:
  filesystem::path path_;
};
}  // namespace

TEST(RuntimeBuffersTest, AllocationsAreDistinctWritableAndProcessOwned) {
  runtime::release_process_strings();
  char* first = runtime::allocate_process_string(32);
  char* second = runtime::allocate_process_string(32);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_NE(first, second);
  for (int index = 0; index < 31; ++index) {
    first[index] = 'a';
    second[index] = 'b';
  }
  first[31] = '\0';
  second[31] = '\0';
  ASSERT_EQ(string(first), string(31, 'a'));
  ASSERT_EQ(string(second), string(31, 'b'));
  ASSERT_TRUE(runtime::process_string_bytes() >= 64);
  runtime::release_process_strings();
  ASSERT_EQ(runtime::process_string_bytes(), size_t{0});
}

TEST(RuntimeBuffersTest, LargeAllocationsUseTheirOwnCommittedBlocks) {
  runtime::release_process_strings();
  constexpr size_t kLarge = 200 * 1024;
  char* buffer = runtime::allocate_process_string(kLarge);
  ASSERT_NE(buffer, nullptr);
  buffer[0] = 'L';
  buffer[kLarge - 1] = 'Z';
  ASSERT_EQ(buffer[0], 'L');
  ASSERT_EQ(buffer[kLarge - 1], 'Z');
  ASSERT_TRUE(runtime::process_string_bytes() >= kLarge);
  runtime::release_process_strings();
  ASSERT_EQ(runtime::process_string_bytes(), size_t{0});
}

TEST(RuntimeBuffersTest, ReleaseIsIdempotentAndAllowsReuse) {
  runtime::release_process_strings();
  runtime::release_process_strings();
  ASSERT_EQ(runtime::process_string_bytes(), size_t{0});
  char* buffer = runtime::allocate_process_string(8);
  ASSERT_NE(buffer, nullptr);
  buffer[0] = 'x';
  ASSERT_EQ(buffer[0], 'x');
  runtime::release_process_strings();
  ASSERT_EQ(runtime::process_string_bytes(), size_t{0});
}

TEST(RuntimeBuffersTest, FileReadsAreProcessLifetimeValues) {
  TemporaryTextFile file("phase seven runtime buffer");
  const string path = file.path().string();
  const char* first = nrq_file_read_text(path.c_str());
  const char* second = nrq_file_read_text(path.c_str());
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_NE(first, second);
  ASSERT_EQ(string(first), string("phase seven runtime buffer"));
  ASSERT_EQ(string(second), string("phase seven runtime buffer"));
  ASSERT_TRUE(runtime::process_string_bytes() > 0);

  nrq_runtime_shutdown();
  // Entry shutdown retires native resources, not borrowed strings. The values
  // are still observable and no later language code can outlive the process.
  ASSERT_TRUE(runtime::process_string_bytes() > 0);
  ASSERT_EQ(string(first), string("phase seven runtime buffer"));
  ASSERT_EQ(string(second), string("phase seven runtime buffer"));

  const char* after = nrq_file_read_text(path.c_str());
  ASSERT_NE(after, nullptr);
  ASSERT_EQ(string(after), string("phase seven runtime buffer"));
  runtime::release_process_strings();
  ASSERT_EQ(runtime::process_string_bytes(), size_t{0});
}

TEST(RuntimeBuffersTest, ConcurrentAllocationsRemainValid) {
  runtime::release_process_strings();
  constexpr int kThreads = 4;
  constexpr int kPerThread = 100;
  vector<char*> pointers(kThreads * kPerThread);
  vector<thread> workers;
  atomic<int> failures{0};
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&, thread] {
      for (int index = 0; index < kPerThread; ++index) {
        char* buffer = runtime::allocate_process_string(64);
        if (!buffer) {
          ++failures;
          continue;
        }
        const char marker = static_cast<char>('a' + thread);
        buffer[0] = marker;
        buffer[63] = '\0';
        if (buffer[0] != marker) ++failures;
        pointers[thread * kPerThread + index] = buffer;
      }
    });
  }
  for (thread& worker : workers) worker.join();

  ASSERT_EQ(failures.load(), 0);
  ASSERT_EQ(pointers.size(), size_t{kThreads * kPerThread});
  sort(pointers.begin(), pointers.end());
  ASSERT_TRUE(adjacent_find(pointers.begin(), pointers.end()) == pointers.end());
  runtime::release_process_strings();
  ASSERT_EQ(runtime::process_string_bytes(), size_t{0});
}
