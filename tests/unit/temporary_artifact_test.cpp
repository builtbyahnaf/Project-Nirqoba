#include "test_harness.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "nirqoba/Driver/TemporaryArtifact.h"

using namespace nirq;
namespace fs = std::filesystem;

namespace {
class TestDirectory {
 public:
  TestDirectory() {
    static unsigned counter = 0;
    path_ = fs::temp_directory_path() /
            ("nirqoba-artifact-test-" + std::to_string(++counter));
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  ~TestDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

void write_text(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}
}  // namespace

TEST(TemporaryArtifactTest, DestructionRemovesOnlyTheStagedFile) {
  TestDirectory directory;
  const fs::path final_path = directory.path() / "program.exe";
  write_text(final_path, "old");

  const fs::path staged =
      TemporaryArtifact::make_staged_path(final_path);
  write_text(staged, "new");
  {
    TemporaryArtifact artifact(staged, final_path);
    ASSERT_TRUE(artifact.owns_staged_file());
    ASSERT_TRUE(fs::exists(staged));
  }

  ASSERT_FALSE(fs::exists(staged));
  ASSERT_EQ(read_text(final_path), std::string("old"));
}

TEST(TemporaryArtifactTest, MoveTransfersCleanupResponsibility) {
  TestDirectory directory;
  const fs::path final_path = directory.path() / "program.exe";
  const fs::path staged =
      TemporaryArtifact::make_staged_path(final_path);
  write_text(staged, "payload");

  TemporaryArtifact source(staged, final_path);
  TemporaryArtifact moved(std::move(source));
  ASSERT_FALSE(source.owns_staged_file());
  ASSERT_TRUE(moved.owns_staged_file());

  source.discard();
  ASSERT_TRUE(fs::exists(staged));

  moved.discard();
  ASSERT_FALSE(fs::exists(staged));
}

TEST(TemporaryArtifactTest, PublishReplacesTheFinalArtifact) {
  TestDirectory directory;
  const fs::path final_path = directory.path() / "program.exe";
  write_text(final_path, "old");
  const fs::path staged =
      TemporaryArtifact::make_staged_path(final_path);
  write_text(staged, "new");

  TemporaryArtifact artifact(staged, final_path);
  ASSERT_TRUE(artifact.publish());
  ASSERT_FALSE(artifact.owns_staged_file());
  ASSERT_FALSE(fs::exists(staged));
  ASSERT_EQ(read_text(final_path), std::string("new"));
}

TEST(TemporaryArtifactTest, PublishWithoutStagedFileLeavesTheOldArtifact) {
  TestDirectory directory;
  const fs::path final_path = directory.path() / "program.exe";
  write_text(final_path, "old");
  const fs::path staged =
      TemporaryArtifact::make_staged_path(final_path);

  TemporaryArtifact artifact(staged, final_path);
  ASSERT_FALSE(artifact.publish());
  ASSERT_TRUE(artifact.owns_staged_file());
  ASSERT_EQ(read_text(final_path), std::string("old"));
}

TEST(TemporaryDirectoryTest, PublishesACompleteTreeOverAnEmptyTarget) {
  TestDirectory directory;
  const fs::path final_path = directory.path() / "project";
  fs::create_directories(final_path);

  TemporaryDirectory staging(
      TemporaryDirectory::make_staged_path(final_path), final_path);
  ASSERT_TRUE(staging.create());
  fs::create_directories(staging.staged_path() / "src");
  write_text(staging.staged_path() / "src" / "main.nrqba", "main");
  const fs::path staged = staging.staged_path();

  ASSERT_TRUE(staging.publish());
  ASSERT_FALSE(staging.owns_staged_directory());
  ASSERT_FALSE(fs::exists(staged));
  ASSERT_EQ(read_text(final_path / "src" / "main.nrqba"), std::string("main"));
}

TEST(TemporaryDirectoryTest, RefusesANonEmptyTargetAndRemovesStaging) {
  TestDirectory directory;
  const fs::path final_path = directory.path() / "project";
  write_text(final_path, "existing");

  TemporaryDirectory staging(
      TemporaryDirectory::make_staged_path(final_path), final_path);
  ASSERT_TRUE(staging.create());
  write_text(staging.staged_path() / "README.md", "new");
  const fs::path staged = staging.staged_path();

  ASSERT_FALSE(staging.publish());
  ASSERT_TRUE(staging.owns_staged_directory());
  staging.discard();
  ASSERT_FALSE(fs::exists(staged));
  ASSERT_EQ(read_text(final_path), std::string("existing"));
}

TEST(TemporaryDirectoryTest, MoveTransfersDirectoryCleanup) {
  TestDirectory directory;
  const fs::path final_path = directory.path() / "project";
  TemporaryDirectory source(
      TemporaryDirectory::make_staged_path(final_path), final_path);
  ASSERT_TRUE(source.create());
  const fs::path staged = source.staged_path();

  TemporaryDirectory moved(std::move(source));
  ASSERT_FALSE(source.owns_staged_directory());
  ASSERT_TRUE(moved.owns_staged_directory());
  ASSERT_TRUE(fs::exists(staged));
  moved.discard();
  ASSERT_FALSE(fs::exists(staged));
}

TEST(TemporaryArtifactTest, StagedNamesAreUniqueSiblings) {
  TestDirectory directory;
  const fs::path final_path = directory.path() / "program.exe";
  const fs::path first = TemporaryArtifact::make_staged_path(final_path);
  const fs::path second = TemporaryArtifact::make_staged_path(final_path);
  ASSERT_NE(first, second);
  ASSERT_EQ(first.parent_path(), final_path.parent_path());
  ASSERT_EQ(second.parent_path(), final_path.parent_path());
}
