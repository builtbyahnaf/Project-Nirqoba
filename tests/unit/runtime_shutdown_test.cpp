#include "test_harness.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/file.h"
#include "nirqoba/runtime/io.h"

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

TEST(RuntimeShutdownTest, LiveMappingIsUnmappedAndRegistryCleared) {
  TemporaryFile file("shutdown mapping");
  void* address = nrq_memory_map_file(file.path().string().c_str(), 0);
  ASSERT_NE(address, nullptr);
  ASSERT_EQ(nrq_memory_mapping_size(address), uint64_t{16});

  nrq_runtime_shutdown();

  ASSERT_EQ(nrq_memory_mapping_size(address), uint64_t{0});
  ASSERT_EQ(nrq_memory_unmap_file(address), int32_t{0});
}
