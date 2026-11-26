#include "event_loop/glib_poll_source.h"
#include "fs/file_error.h"
#include "operations/archive_service.h"
#include "temp_folder.h"
#include "test_check.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/browser/file_error_text.h"
#include "ui/controls/archive_view.h"
#include "ui/controls/label.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <print>
#include <string>
#include <vector>

// A file named .zip that is no archive: the service ends in a specific error, with no catalog, and the archive
// browser says so in its status line instead of showing what looks like an empty archive.
namespace {
  namespace fs = std::filesystem;

  void until(GlibPollSource& source, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::vector<pollfd> fds;
      const auto first = source.addPollFds(fds);
      TEST_CHECK(::poll(fds.data(), fds.size(), 20) >= 0);
      source.dispatch(fds, first);
    }
  }

  bool shows(const Node& node, const std::string& text) {
    if (const auto* label = dynamic_cast<const Label*>(&node); label != nullptr && label->text() == text) {
      return true;
    }
    return std::ranges::any_of(node.children(), [&](const auto& child) { return shows(*child, text); });
  }

  void aFileThatIsNoArchiveGivesASpecificError(
      const fs::path& folder, const std::string& name, const std::string& body, const std::string& expected
  ) {
    const auto path = folder / name;
    TEST_CHECK((std::ofstream(path, std::ios::binary) << body).good());
    GlibPollSource source;
    luciole::ArchiveService service;
    TEST_CHECK(service.inspect(luciole::Location::fromPath(path.native())));
    until(source, [&] { return !service.busy(); });
    TEST_CHECK(service.error().has_value() && service.catalog() == nullptr);
    const auto text = luciole::fileErrorText(service.error());
    std::println("{}: {}", name, text);
    TEST_CHECK(text == expected);
    luciole::FileIconCache icons;
    ArchiveView view(
        icons, [] { return 0U; }, [] {}, [](std::vector<std::string>) {}, [](std::vector<std::string>) {},
        [](const std::string&) {}, [] {}, [] {}
    );
    view.sync(nullptr, service.phase(), service.items(), service.bytes(), service.error());
    TEST_CHECK(shows(view, text));
    TEST_CHECK(!shows(view, "0 items"));
  }
} // namespace

int main() {
  luciole::test::TempFolder temp{"luciole-archive-error"};
  aFileThatIsNoArchiveGivesASpecificError(
      temp.path(), "notes.zip", "this is not a zip\n", "Unrecognized archive format"
  );
  aFileThatIsNoArchiveGivesASpecificError(
      temp.path(), "empty.zip", "", "Archive is empty, special, or exceeds the combined input-size limit."
  );
  std::println("archive-inspect-error: all checks passed");
  return 0;
}
