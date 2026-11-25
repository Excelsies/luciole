#include "browser/lookahead_columns.h"
#include "event_loop/glib_poll_source.h"
#include "fs/open_path_guard.h"
#include "fs/user_file_save.h"
#include "temp_folder.h"
#include "test_check.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <poll.h>
#include <print>
#include <string>

// A cancelled listing or file read goes on until its worker notices, with the folder or file still open. The
// registry says so, so the removal of a drive waits for it: whenever a descriptor under the folder is held, the
// registry says a path there is open.
namespace {
  namespace fs = std::filesystem;
  using luciole::Location;
  using luciole::pathOpenWithin;

  std::size_t held(const fs::path& root) {
    std::size_t count = 0;
    for (const auto& file : fs::directory_iterator("/proc/self/fd")) {
      std::error_code error;
      const auto target = fs::read_symlink(file.path(), error).native();
      count += !error && (target == root.native() || target.starts_with(root.native() + "/"));
    }
    return count;
  }

  // A descriptor held before and after the registry was read was held while it was read, so the registry must have
  // named it; one sample alone cannot tell, as the worker may end between the two reads.
  bool namedWhileHeld(const fs::path& root, bool& open) {
    const bool before = held(root) > 0;
    const bool named = pathOpenWithin(root.native());
    const bool after = held(root) > 0;
    open = before || after;
    return !(before && after) || named;
  }

  void pump(GlibPollSource& source) {
    std::vector<pollfd> fds;
    const auto first = source.addPollFds(fds);
    TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
    source.dispatch(fds, first);
  }

  // A mount point is not its sibling that starts with the same letters, however the root is spelled.
  void containment() {
    const luciole::OpenPathGuard guard("/mnt/usb2/folder/file");
    TEST_CHECK(pathOpenWithin("/mnt/usb2") && pathOpenWithin("/mnt/usb2/") && pathOpenWithin("/mnt/usb2/folder"));
    TEST_CHECK(pathOpenWithin("/mnt/usb2/folder/file") && pathOpenWithin("/") && pathOpenWithin("/mnt"));
    TEST_CHECK(!pathOpenWithin("/mnt/usb") && !pathOpenWithin("/mnt/usb2/fold") && !pathOpenWithin("/media"));
    TEST_CHECK(!pathOpenWithin("/mnt/usb2/folder/file/more"));
    {
      const luciole::OpenPathGuard second("/mnt/usb/x");
      TEST_CHECK(pathOpenWithin("/mnt/usb"));
    }
    TEST_CHECK(!pathOpenWithin("/mnt/usb"));
  }

  void listingsHoldTheFolderUntilTheyEnd() {
    luciole::test::TempFolder temp{"luciole-open-path"};
    const auto drive = temp.path() / "drive";
    fs::create_directories(drive / "big");
    for (int i = 0; i < 60000; ++i) {
      std::ofstream(drive / "big" / std::format("f{:05}", i)) << "x";
    }
    const auto root = Location::fromPath(drive.native());
    GlibPollSource source;
    bool seen = false;
    for (int trial = 0; trial < 8; ++trial) {
      luciole::LookaheadColumns columns({}, false, true);
      columns.follow(
          {.target = luciole::WorkbenchEntry{.location = Location::fromPath((drive / "big").native()), .folder = true}}
      );
      // The listing starts from the main loop, and its worker opens the folder a moment later.
      for (int round = 0; round < 5000 && held(drive) == 0; ++round) {
        pump(source);
      }
      columns.releaseLocation(root);
      // From the release until the descriptor is gone, a held folder is always reported.
      for (int round = 0; round < 20'000; ++round) {
        bool open = false;
        TEST_CHECK(namedWhileHeld(drive, open));
        seen = seen || open;
        if (!open && !pathOpenWithin(drive.native())) {
          break;
        }
        pump(source);
      }
      TEST_CHECK(!pathOpenWithin(drive.native()));
    }
    TEST_CHECK(seen); // the folder was open at least once while the test looked
  }

  void readsHoldTheFileUntilTheyEnd() {
    luciole::test::TempFolder temp{"luciole-open-path"};
    const auto file = temp.path() / "big.bin";
    {
      std::ofstream out(file, std::ios::binary);
      const std::string block(1 << 20, 'x');
      for (int i = 0; i < 128; ++i) {
        out << block;
      }
    }
    GlibPollSource source;
    luciole::CancelToken token;
    bool done = false;
    luciole::startUserFileRead(
        Location::fromPath(file.native()), std::size_t{128} << 20, token,
        [&done](std::expected<luciole::UserFileContent, luciole::FileError>) { done = true; }
    );
    bool seen = false;
    for (int round = 0; round < 20'000 && !done; ++round) {
      bool open = false;
      TEST_CHECK(namedWhileHeld(temp.path(), open));
      seen = seen || open;
      if (open) {
        token.cancel();
      }
      pump(source);
    }
    TEST_CHECK(seen && done && held(temp.path()) == 0 && !pathOpenWithin(temp.path().native()));
  }
} // namespace

int main() {
  containment();
  listingsHoldTheFolderUntilTheyEnd();
  readsHoldTheFileUntilTheyEnd();
  std::println("open-path-guard: all checks passed");
  return 0;
}
