#include "fs/user_file_save.h"
#include "temp_folder.h"
#include "test_check.h"

#include <array>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

// A save of a file or folder that is not writable at save time: the refusal names the permissions unless the file's
// content, size, modification time or identity changed, and nothing is written in any case.
namespace {
  namespace fs = std::filesystem;
  using luciole::Location;
  using Status = luciole::UserFileSaveStatus;

  struct Fixture {
    luciole::test::TempFolder tempFolder{"luciole-save-permissions-test"};
    fs::path directory = tempFolder.path();
  };

  std::string contents(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  void write(const fs::path& path, const std::string& text, mode_t mode = 0644) {
    if (fs::exists(path)) {
      TEST_CHECK(::chmod(path.c_str(), 0644) == 0); // a file the last case locked
    }
    TEST_CHECK((std::ofstream(path, std::ios::binary | std::ios::trunc) << text).good());
    TEST_CHECK(::chmod(path.c_str(), mode) == 0);
  }

  std::set<std::string> names(const fs::path& directory) {
    std::set<std::string> result;
    for (const auto& item : fs::directory_iterator(directory)) {
      result.insert(item.path().filename().native());
    }
    return result;
  }

  Location at(const fs::path& path) { return Location::fromPath(path.native()); }

  // Reads `path`, lets `change` run, and saves "edited!!" against the revision read.
  luciole::UserFileSaveResult saveAfter(const fs::path& path, const std::function<void()>& change) {
    auto read = luciole::readUserFile(at(path), 1024, {});
    TEST_CHECK(read.has_value());
    // Past the coarsest timestamp tick, so no outcome depends on clock luck.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    change();
    return luciole::saveUserFile(at(path), "edited!!", read->identity, {});
  }

  // The refusal and the proof that nothing was written: the content and the folder are as the change left them.
  void expectRefused(
      const Fixture& fixture, const fs::path& path, const std::function<void()>& change, Status status,
      const std::string& expected
  ) {
    const auto result = saveAfter(path, change);
    TEST_CHECK(result.status == status && !result.identity && !result.message.empty());
    TEST_CHECK(contents(path) == expected);
    TEST_CHECK(names(fixture.directory) == std::set<std::string>{path.filename().native()});
  }

  void testPermissionsOnly() {
    const Fixture fixture;
    const auto path = fixture.directory / "notes.txt";
    write(path, "original");
    expectRefused(
        fixture, path, [&] { TEST_CHECK(::chmod(path.c_str(), 0444) == 0); }, Status::ReadOnlyFile, "original"
    );
  }

  void testFolderPermissionsOnly() {
    const Fixture fixture;
    const auto folder = fixture.directory / "locked";
    fs::create_directory(folder);
    const auto path = folder / "notes.txt";
    write(path, "original");
    const auto result = saveAfter(path, [&] { TEST_CHECK(::chmod(folder.c_str(), 0555) == 0); });
    TEST_CHECK(::chmod(folder.c_str(), 0755) == 0);
    TEST_CHECK(result.status == Status::ReadOnlyFolder && !result.identity);
    TEST_CHECK(contents(path) == "original" && names(folder) == std::set<std::string>{"notes.txt"});
  }

  void testFileAndFolderPermissions() {
    const Fixture fixture;
    const auto folder = fixture.directory / "locked";
    fs::create_directory(folder);
    const auto path = folder / "notes.txt";
    write(path, "original");
    const auto result = saveAfter(path, [&] {
      TEST_CHECK(::chmod(path.c_str(), 0444) == 0);
      TEST_CHECK(::chmod(folder.c_str(), 0555) == 0);
    });
    TEST_CHECK(::chmod(folder.c_str(), 0755) == 0);
    TEST_CHECK(result.status == Status::ReadOnlyFolder);
    TEST_CHECK(contents(path) == "original" && names(folder) == std::set<std::string>{"notes.txt"});
  }

  // A file another program rewrote stays refused as changed, whatever its permissions became.
  void testPermissionsAndContent() {
    const Fixture fixture;
    const auto path = fixture.directory / "notes.txt";
    const auto lock = [&] { TEST_CHECK(::chmod(path.c_str(), 0444) == 0); };
    write(path, "original");
    expectRefused(
        fixture, path,
        [&] {
          write(path, "different content");
          lock();
        },
        Status::ChangedOnDisk, "different content"
    );
    write(path, "original");
    expectRefused(
        fixture, path,
        [&] {
          write(path, "ORIGINAL");
          lock();
        },
        Status::ChangedOnDisk, "ORIGINAL"
    );
    write(path, "original");
    const timespec longAgo{.tv_sec = 1000000000, .tv_nsec = 0};
    const std::array past{longAgo, longAgo};
    expectRefused(
        fixture, path,
        [&] {
          TEST_CHECK(::utimensat(AT_FDCWD, path.c_str(), past.data(), 0) == 0);
          lock();
        },
        Status::ChangedOnDisk, "original"
    );
    write(path, "original");
    expectRefused(
        fixture, path,
        [&] {
          write(fixture.directory / "replacement", "original");
          fs::rename(fixture.directory / "replacement", path);
          lock();
        },
        Status::ChangedOnDisk, "original"
    );
  }

  // A permission change on a file that stays writable is still a change of the file.
  void testWritableStatusChange() {
    const Fixture fixture;
    const auto path = fixture.directory / "notes.txt";
    write(path, "original");
    expectRefused(
        fixture, path, [&] { TEST_CHECK(::chmod(path.c_str(), 0644) == 0); }, Status::ChangedOnDisk, "original"
    );
  }
} // namespace

int main() {
  if (::geteuid() == 0) {
    return 77; // root writes read-only files, so the refusals cannot be shown
  }
  testPermissionsOnly();
  testFolderPermissionsOnly();
  testFileAndFolderPermissions();
  testPermissionsAndContent();
  testWritableStatusChange();
  return 0;
}
