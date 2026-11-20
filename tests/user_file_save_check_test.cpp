#include "fs/user_file_save.h"
#include "temp_folder.h"
#include "test_check.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// What a save would refuse, asked before anything is written, for real files in a private folder.
namespace {
  namespace fs = std::filesystem;
  using luciole::Location;
  using Status = luciole::UserFileSaveStatus;

  struct Fixture {
    luciole::test::TempFolder tempFolder{"luciole-save-check-test"};
    fs::path directory = tempFolder.path();
  };

  void write(const fs::path& path, const std::string& text, mode_t mode = 0644) {
    TEST_CHECK((std::ofstream(path, std::ios::binary | std::ios::trunc) << text).good());
    TEST_CHECK(::chmod(path.c_str(), mode) == 0);
  }

  Location at(const fs::path& path) { return Location::fromPath(path.native()); }

  std::optional<Status> refusal(const fs::path& path) {
    const auto refused = luciole::userFileSaveRefusal(at(path));
    TEST_CHECK(!refused || !refused->message.empty());
    TEST_CHECK(!refused || !refused->identity);
    return refused ? std::optional(refused->status) : std::nullopt;
  }

  struct stat statOf(const fs::path& path) {
    struct stat info{};
    TEST_CHECK(::lstat(path.c_str(), &info) == 0);
    return info;
  }

  void testWritableFileHasNoRefusal() {
    const Fixture fixture;
    const auto path = fixture.directory / "notes.txt";
    write(path, "text\n");
    const auto before = statOf(path);
    TEST_CHECK(!refusal(path));
    const auto after = statOf(path);
    // Nothing was written: the file keeps its revision.
    TEST_CHECK(after.st_ino == before.st_ino && after.st_size == before.st_size);
    TEST_CHECK(after.st_mtim.tv_sec == before.st_mtim.tv_sec && after.st_mtim.tv_nsec == before.st_mtim.tv_nsec);
    TEST_CHECK(after.st_ctim.tv_sec == before.st_ctim.tv_sec && after.st_ctim.tv_nsec == before.st_ctim.tv_nsec);
    TEST_CHECK(std::distance(fs::directory_iterator(fixture.directory), fs::directory_iterator()) == 1);
  }

  void testEachRefusal() {
    const Fixture fixture;
    const auto readOnly = fixture.directory / "read-only.txt";
    write(readOnly, "text\n", 0444);
    TEST_CHECK(refusal(readOnly) == Status::ReadOnlyFile);

    const auto first = fixture.directory / "first.txt";
    write(first, "text\n");
    fs::create_hard_link(first, fixture.directory / "second.txt");
    TEST_CHECK(refusal(first) == Status::HardLinked);

    const auto folder = fixture.directory / "locked";
    fs::create_directory(folder);
    write(folder / "inside.txt", "text\n");
    TEST_CHECK(::chmod(folder.c_str(), 0555) == 0);
    const auto lockedFolder = refusal(folder / "inside.txt");
    TEST_CHECK(::chmod(folder.c_str(), 0755) == 0);
    TEST_CHECK(lockedFolder == Status::ReadOnlyFolder);

    TEST_CHECK(refusal(fixture.directory / "missing.txt") == Status::ChangedOnDisk);
    TEST_CHECK(refusal(fixture.directory) == Status::NotRegularFile);
    const auto remote = luciole::userFileSaveRefusal(Location::fromUri("sftp://example.invalid/home/file.txt"));
    TEST_CHECK(remote && remote->status == Status::NotLocal && !remote->message.empty());
  }

  // A link is answered for its target, as a save writes through it.
  void testSymlinkAnsweredForItsTarget() {
    const Fixture fixture;
    const auto target = fixture.directory / "target.txt";
    write(target, "text\n");
    fs::create_symlink(target, fixture.directory / "link.txt");
    TEST_CHECK(!refusal(fixture.directory / "link.txt"));
    TEST_CHECK(::chmod(target.c_str(), 0444) == 0);
    TEST_CHECK(refusal(fixture.directory / "link.txt") == Status::ReadOnlyFile);
  }
} // namespace

int main() {
  if (::geteuid() == 0) {
    return 77; // root writes read-only files, so the refusals cannot be shown
  }
  testWritableFileHasNoRefusal();
  testEachRefusal();
  testSymlinkAnsweredForItsTarget();
  return 0;
}
