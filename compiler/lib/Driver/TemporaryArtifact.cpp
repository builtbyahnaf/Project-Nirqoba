#include "nirqoba/Driver/TemporaryArtifact.h"

#include <atomic>
#include <system_error>
#include <utility>
#include <vector>

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
  bool removed_empty_target = false;
  if (fs::exists(final_path_, error)) {
    // The caller checked for a non-empty target before staging. Recheck here so
    // a concurrently created project cannot be replaced.
    if (!fs::is_empty(final_path_, error) || error) return false;
    if (!fs::remove(final_path_, error) || error) return false;
    removed_empty_target = true;
  }

  fs::rename(staged_path_, final_path_, error);
  if (error) {
    // Windows cannot rename onto an existing directory. Recreate the empty
    // target if the commit failed after removing it, so a failed publish does
    // not erase what the caller already had.
    if (removed_empty_target) {
      error_code restore;
      fs::create_directories(final_path_, restore);
    }
    return false;
  }

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
  // MOVEFILE_WRITE_THROUGH would flush the new directory entry to disk before
  // returning. Publication is an in-process atomicity boundary, not a crash
  // durability promise, and the synchronous flush cost several milliseconds on
  // every build, so only replacement is requested.
  if (!MoveFileExW(staged_path_.c_str(), final_path_.c_str(),
                   MOVEFILE_REPLACE_EXISTING)) {
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

ArtifactTransaction::~ArtifactTransaction() { discard(); }

ArtifactTransaction::ArtifactTransaction(ArtifactTransaction&& other) noexcept
    : entries_(std::move(other.entries_)) {
  other.entries_.clear();
}

ArtifactTransaction& ArtifactTransaction::operator=(
    ArtifactTransaction&& other) noexcept {
  if (this == &other) return *this;
  discard();
  entries_ = std::move(other.entries_);
  other.entries_.clear();
  return *this;
}

void ArtifactTransaction::add(TemporaryArtifact artifact) {
  Entry entry;
  entry.artifact = std::move(artifact);
  entries_.push_back(std::move(entry));
}

bool ArtifactTransaction::prepare_backup(Entry& entry) {
  error_code error;
  if (!fs::exists(entry.artifact.final_path(), error)) return true;
  if (error) return false;

  const fs::path& final_path = entry.artifact.final_path();
  fs::path directory = final_path.parent_path();
  string filename = final_path.filename().string();
  if (filename.empty()) filename = "artifact";
  const string tag = ".nrq-backup-" + to_string(process_id()) + "-" +
                     to_string(++g_staged_counter);
  entry.backup_path = directory / ("." + filename + tag);

#ifdef _WIN32
  // A hard link preserves the previous final without moving it, so a failure
  // before the first replacement leaves the output directory untouched. The
  // copy fallback covers filesystems that reject hard links.
  if (CreateHardLinkW(entry.backup_path.c_str(), final_path.c_str(), nullptr)) {
    entry.had_previous = true;
    return true;
  }
#endif
  if (!fs::copy_file(final_path, entry.backup_path,
                     fs::copy_options::overwrite_existing, error) ||
      error) {
    return false;
  }
  entry.had_previous = true;
  return true;
}

void ArtifactTransaction::remove_backup(Entry& entry) noexcept {
  if (entry.backup_path.empty()) return;
  error_code error;
  fs::remove(entry.backup_path, error);
  entry.backup_path.clear();
  entry.had_previous = false;
}

void ArtifactTransaction::rollback() noexcept {
  // Restore in reverse publication order. A final with a previous version is
  // replaced by its hard-link backup; a final that did not exist before is
  // removed. Failures here leave the backup in place rather than deleting the
  // only surviving copy.
  for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
    if (entry->published) {
      error_code error;
      if (entry->had_previous && fs::exists(entry->backup_path, error)) {
#ifdef _WIN32
        if (MoveFileExW(entry->backup_path.c_str(),
                        entry->artifact.final_path().c_str(),
                        MOVEFILE_REPLACE_EXISTING)) {
          entry->backup_path.clear();
          entry->had_previous = false;
          continue;
        }
#else
        fs::remove(entry->artifact.final_path(), error);
        error.clear();
        fs::rename(entry->backup_path, entry->artifact.final_path(), error);
        if (!error) {
          entry->backup_path.clear();
          entry->had_previous = false;
          continue;
        }
#endif
      } else {
        fs::remove(entry->artifact.final_path(), error);
      }
    }
    remove_backup(*entry);
  }
}

bool ArtifactTransaction::publish() {
  if (entries_.empty()) return true;

  for (Entry& entry : entries_) {
    if (!prepare_backup(entry)) {
      for (Entry& prepared : entries_) remove_backup(prepared);
      return false;
    }
  }

  for (Entry& entry : entries_) {
    if (!entry.artifact.publish()) {
      rollback();
      return false;
    }
    entry.published = true;
  }

  for (Entry& entry : entries_) remove_backup(entry);
  entries_.clear();
  return true;
}

void ArtifactTransaction::discard() noexcept {
  for (Entry& entry : entries_) {
    remove_backup(entry);
    entry.artifact.discard();
  }
  entries_.clear();
}

}  // namespace nirq
