#include "nirqoba/Driver/TemporaryArtifact.h"

#include <atomic>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace nirq {
using namespace std;
namespace fs = filesystem;

namespace {
atomic<unsigned long long> g_staged_counter{0};

unsigned long process_id() {
#ifdef _WIN32
  return static_cast<unsigned long>(GetCurrentProcessId());
#else
  return 0;
#endif
}
}  // namespace

TemporaryDirectory::TemporaryDirectory(fs::path staged_path,
                                       fs::path final_path)
    : staged_path_(move(staged_path)),
      final_path_(move(final_path)),
      owns_staged_directory_(false) {}

TemporaryDirectory::~TemporaryDirectory() { discard(); }

TemporaryDirectory::TemporaryDirectory(TemporaryDirectory&& other) noexcept
    : staged_path_(move(other.staged_path_)),
      final_path_(move(other.final_path_)),
      owns_staged_directory_(other.owns_staged_directory_) {
  other.release();
}

TemporaryDirectory& TemporaryDirectory::operator=(
    TemporaryDirectory&& other) noexcept {
  if (this == &other) return *this;
  discard();
  staged_path_ = move(other.staged_path_);
  final_path_ = move(other.final_path_);
  owns_staged_directory_ = other.owns_staged_directory_;
  other.release();
  return *this;
}

fs::path TemporaryDirectory::make_staged_path(const fs::path& final_path) {
  fs::path directory = final_path.parent_path();
  string filename = final_path.filename().string();
  if (filename.empty()) filename = "project";

  const unsigned long long counter = ++g_staged_counter;
  const string tag = ".nrq-dir-" + to_string(process_id()) + "-" +
                     to_string(counter);
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    fs::path candidate =
        directory / ("." + filename + tag + "-" + to_string(attempt));
    error_code error;
    if (!fs::exists(candidate, error)) return candidate;
  }
  return directory / ("." + filename + tag + "-fallback");
}

bool TemporaryDirectory::create() {
  error_code error;
  if (!fs::create_directories(staged_path_, error) && error) return false;
  owns_staged_directory_ = true;
  return true;
}

bool TemporaryDirectory::publish() {
  if (!owns_staged_directory_) return false;

  error_code error;
  if (fs::exists(final_path_, error)) {
    // The caller checked for a non-empty target before staging. Recheck here so
    // a concurrently created project cannot be replaced.
    if (!fs::is_empty(final_path_, error) || error) return false;
    if (!fs::remove(final_path_, error) || error) return false;
  }

  fs::rename(staged_path_, final_path_, error);
  if (error) return false;

  release();
  return true;
}

void TemporaryDirectory::discard() noexcept {
  if (!owns_staged_directory_) return;
  error_code error;
  fs::remove_all(staged_path_, error);
  release();
}

void TemporaryDirectory::release() noexcept {
  owns_staged_directory_ = false;
  staged_path_.clear();
}

TemporaryArtifact::TemporaryArtifact(fs::path staged_path, fs::path final_path)
    : staged_path_(move(staged_path)),
      final_path_(move(final_path)),
      owns_staged_file_(!staged_path_.empty()) {}

TemporaryArtifact::~TemporaryArtifact() { discard(); }

TemporaryArtifact::TemporaryArtifact(TemporaryArtifact&& other) noexcept
    : staged_path_(move(other.staged_path_)),
      final_path_(move(other.final_path_)),
      owns_staged_file_(other.owns_staged_file_) {
  other.release();
}

TemporaryArtifact& TemporaryArtifact::operator=(
    TemporaryArtifact&& other) noexcept {
  if (this == &other) return *this;
  discard();
  staged_path_ = move(other.staged_path_);
  final_path_ = move(other.final_path_);
  owns_staged_file_ = other.owns_staged_file_;
  other.release();
  return *this;
}

fs::path TemporaryArtifact::make_staged_path(const fs::path& final_path) {
  fs::path directory = final_path.parent_path();
  string filename = final_path.filename().string();
  if (filename.empty()) filename = "artifact";

  const unsigned long long counter = ++g_staged_counter;
  const string tag = ".nrq-tmp-" + to_string(process_id()) + "-" +
                     to_string(counter);

  // A stale file with our tag is not trusted; advance until the sibling is
  // free. The staging file is created by the producer, not here, so there is no
  // window in which an empty artifact looks complete.
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    fs::path candidate =
        directory / ("." + filename + tag + "-" + to_string(attempt));
    error_code error;
    if (!fs::exists(candidate, error)) return candidate;
  }
  return directory / ("." + filename + tag + "-fallback");
}

bool TemporaryArtifact::publish() {
  if (!owns_staged_file_) return true;

  error_code error;
  if (!fs::exists(staged_path_, error)) return false;

#ifdef _WIN32
  if (!MoveFileExW(staged_path_.c_str(), final_path_.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return false;
  }
#else
  if (fs::exists(final_path_, error)) {
    fs::remove(final_path_, error);
    if (error) return false;
  }
  fs::rename(staged_path_, final_path_, error);
  if (error) return false;
#endif

  release();
  return true;
}

void TemporaryArtifact::discard() noexcept {
  if (!owns_staged_file_) return;
  error_code error;
  fs::remove(staged_path_, error);
  release();
}

void TemporaryArtifact::release() noexcept {
  owns_staged_file_ = false;
  staged_path_.clear();
}

}  // namespace nirq
