// Driver/TemporaryArtifact.h — deterministic ownership for compiler-created
// files.
//
// The compiler is a native process, so its internal resources use C++ lifetime
// ownership rather than the language's GC. A compiler artifact is written to a
// sibling staging path and only renamed onto its final path when the producing
// stage succeeds. The lease removes the staging file on every other exit,
// including an early return, a diagnostic, or an exception.
#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace nirq {

// A directory lease for a compiler-owned staging tree. Unlike a file lease,
// publication is a directory rename, so readers never observe a partially
// scaffolded project.
class TemporaryDirectory {
 public:
  TemporaryDirectory() = default;
  TemporaryDirectory(std::filesystem::path staged_path,
                     std::filesystem::path final_path);
  ~TemporaryDirectory();

  TemporaryDirectory(TemporaryDirectory&& other) noexcept;
  TemporaryDirectory& operator=(TemporaryDirectory&& other) noexcept;
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  static std::filesystem::path make_staged_path(
      const std::filesystem::path& final_path);

  // Creates the staged directory tree. The destructor removes it on every path
  // that does not publish it.
  bool create();

  const std::filesystem::path& staged_path() const { return staged_path_; }
  const std::filesystem::path& final_path() const { return final_path_; }
  bool owns_staged_directory() const { return owns_staged_directory_; }

  // Renames the staged tree into place. Refuses to replace a non-empty target,
  // and leaves a failed target untouched.
  bool publish();

  void discard() noexcept;

 private:
  void release() noexcept;

  std::filesystem::path staged_path_;
  std::filesystem::path final_path_;
  bool owns_staged_directory_ = false;
};

class TemporaryArtifact {
 public:
  TemporaryArtifact() = default;
  TemporaryArtifact(std::filesystem::path staged_path,
                    std::filesystem::path final_path);
  ~TemporaryArtifact();

  // A lease owns its staging file until it is published or discarded. Moving
  // transfers that responsibility; the moved-from lease owns nothing.
  TemporaryArtifact(TemporaryArtifact&& other) noexcept;
  TemporaryArtifact& operator=(TemporaryArtifact&& other) noexcept;
  TemporaryArtifact(const TemporaryArtifact&) = delete;
  TemporaryArtifact& operator=(const TemporaryArtifact&) = delete;

  // A unique sibling of `final_path`. The name is diagnostic-only: it names the
  // final artifact and carries the process/counter so concurrent compiler
  // processes cannot collide.
  static std::filesystem::path make_staged_path(
      const std::filesystem::path& final_path);

  const std::filesystem::path& staged_path() const { return staged_path_; }
  const std::filesystem::path& final_path() const { return final_path_; }
  bool owns_staged_file() const { return owns_staged_file_; }

  // Atomically replaces `final_path` with the staged file. Returns false and
  // leaves the staged file owned (so the destructor can remove it) when the
  // rename fails. A pre-existing final artifact is left untouched on failure.
  bool publish();

  // Removes the staged file immediately and releases ownership.
  void discard() noexcept;

 private:
  void release() noexcept;

  std::filesystem::path staged_path_;
  std::filesystem::path final_path_;
  bool owns_staged_file_ = false;
};

// Publishes a set of artifacts as one all-or-nothing operation on the normal
// failure paths. Each existing final is first preserved through a hard-link
// backup (copy fallback), then every staged file replaces its final. If any
// rename fails, already-published finals are restored from their backups and
// finals that did not exist before are removed. A crash between renames is not
// a transaction and is documented as a limitation; a returned failure is never
// allowed to leave a new object beside an old executable.
class ArtifactTransaction {
 public:
  ArtifactTransaction() = default;
  ~ArtifactTransaction();

  ArtifactTransaction(ArtifactTransaction&& other) noexcept;
  ArtifactTransaction& operator=(ArtifactTransaction&& other) noexcept;
  ArtifactTransaction(const ArtifactTransaction&) = delete;
  ArtifactTransaction& operator=(const ArtifactTransaction&) = delete;

  // Takes ownership of a staged artifact. Order is publication order.
  void add(TemporaryArtifact artifact);

  bool empty() const { return entries_.empty(); }

  // Publishes every entry, or restores every final touched before returning
  // false. On failure, staged files that were not consumed are still owned by
  // their lease and are removed by the destructor.
  bool publish();

  // Removes every staged file and backup without publishing.
  void discard() noexcept;

 private:
  struct Entry {
    TemporaryArtifact artifact;
    std::filesystem::path backup_path;
    bool had_previous = false;
    bool published = false;
  };

  bool prepare_backup(Entry& entry);
  void remove_backup(Entry& entry) noexcept;
  void rollback() noexcept;

  std::vector<Entry> entries_;
};

}  // namespace nirq
