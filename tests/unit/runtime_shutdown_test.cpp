#include "test_harness.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/file.h"
#include "nirqoba/runtime/io.h"
#include "nirqoba/runtime/sync.h"

using namespace std;

namespace {
class TemporaryFile {
 public:
  explicit TemporaryFile(const string& contents) {
    static unsigned counter = 0;
    path_ = filesystem::temp_directory_path() /
            ("nirqoba-shutdown-test-" + to_string(++counter) + ".txt");
    ofstream out(path_, ios::binary | ios::trunc);
    out << contents;
  }
  ~TemporaryFile() {
    error_code error;
    filesystem::remove(path_, error);
  }
  const filesystem::path& path() const { return path_; }

 private:
  filesystem::path path_;
};
}  // namespace

TEST(RuntimeShutdownTest, LiveFileHandleIsClosedAndInvalidated) {
  TemporaryFile file("shutdown file");
  void* handle = nrq_file_open_handle(file.path().string().c_str(), 0);
  ASSERT_NE(handle, nullptr);

  nrq_runtime_shutdown();

  // The registry was cleared before the OS handle was closed, so the stale
  // token is rejected rather than resolving a half-closed resource.
  ASSERT_EQ(nrq_file_close_handle(handle),
            static_cast<int32_t>(ERROR_INVALID_HANDLE));
}

TEST(RuntimeShutdownTest, ReusedFileSlotDoesNotReactivateAStaleToken) {
  TemporaryFile first("first");
  TemporaryFile second("second");
  void* first_handle = nrq_file_open_handle(first.path().string().c_str(), 0);
  ASSERT_NE(first_handle, nullptr);
  ASSERT_EQ(nrq_file_close_handle(first_handle), int32_t{0});

  // The next open takes the freed slot and a fresh generation.
  void* second_handle = nrq_file_open_handle(second.path().string().c_str(), 0);
  ASSERT_NE(second_handle, nullptr);
  ASSERT_NE(first_handle, second_handle);
  ASSERT_EQ(nrq_file_close_handle(first_handle),
            static_cast<int32_t>(ERROR_INVALID_HANDLE));
  ASSERT_EQ(nrq_file_close_handle(second_handle), int32_t{0});
}

TEST(RuntimeShutdownTest, ShutdownIsIdempotent) {
  TemporaryFile file("idempotent shutdown");
  void* handle = nrq_file_open_handle(file.path().string().c_str(), 0);
  ASSERT_NE(handle, nullptr);

  nrq_runtime_shutdown();
  nrq_runtime_shutdown();

  ASSERT_EQ(nrq_file_close_handle(handle),
            static_cast<int32_t>(ERROR_INVALID_HANDLE));
}

TEST(RuntimeShutdownTest, MutexRegistryIsUsableAfterShutdown) {
  void* first = nrq_mutex_create();
  ASSERT_NE(first, nullptr);
  nrq_mutex_lock(first);
  nrq_mutex_unlock(first);

  nrq_runtime_shutdown();

  // Shutdown is not a one-way process latch for the registry itself: a fresh
  // mutex must still be created and used for the remaining lifetime.
  void* second = nrq_mutex_create();
  ASSERT_NE(second, nullptr);
  nrq_mutex_lock(second);
  nrq_mutex_unlock(second);
  nrq_mutex_destroy(second);
}

TEST(RuntimeShutdownTest, FileReadStringSurvivesEntryShutdown) {
  TemporaryFile file("phase7 string");
  const char* text = nrq_file_read_text(file.path().string().c_str());
  ASSERT_NE(text, nullptr);
  ASSERT_EQ(string(text), string("phase7 string"));

  // Entry shutdown does not free borrowed strings; the OS is the release point.
  nrq_runtime_shutdown();
  ASSERT_EQ(string(text), string("phase7 string"));
}

TEST(RuntimeShutdownTest, ExitCleanupRetiresUncontendedRegistries) {
  TemporaryFile file("exit cleanup");
  void* handle = nrq_file_open_handle(file.path().string().c_str(), 0);
  ASSERT_NE(handle, nullptr);

  // The termination path uses try-lock, but an uncontended registry must still
  // be fully retired rather than being abandoned to process teardown.
  nrq_runtime_shutdown_for_exit();

  ASSERT_EQ(nrq_file_close_handle(handle),
            static_cast<int32_t>(ERROR_INVALID_HANDLE));
}

TEST(RuntimeShutdownTest, LiveMappingIsUnmappedAndRegistryCleared) {
  TemporaryFile file("shutdown mapping");
  void* address = nrq_memory_map_file(file.path().string().c_str(), 0);
  ASSERT_NE(address, nullptr);
  ASSERT_EQ(nrq_memory_mapping_size(address), uint64_t{16});

  nrq_runtime_shutdown();

  ASSERT_EQ(nrq_memory_mapping_size(address), uint64_t{0});
  ASSERT_EQ(nrq_memory_unmap_file(address), int32_t{0});
}
