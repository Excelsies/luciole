#include "browser/browser_controller.h"
#include "browser/folder_views.h"
#include "config/folder_view_store.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "temp_folder.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <poll.h>
#include <print>
#include <stdexcept>
#include <vector>

// A folder remembered in Workbench comes back in Workbench, one remembered in a classic view stays in it, and a stored
// view mode that nothing reads is refused with an issue and leaves the folder as it was.
namespace {
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;
  using luciole::BrowserController;
  using luciole::DirectoryStatus;
  using luciole::FolderView;
  using luciole::FolderViewService;
  using luciole::FolderViewStore;
  using luciole::Location;
  using luciole::SortOptions;
  using luciole::ViewMode;

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0);
        m_source.dispatch(fds, start);
      }
    }

  private:
    GlibPollSource m_source;
  };

  struct Fixture {
    luciole::test::TempFolder temp{"luciole-folder-views-workbench"};
    fs::path path = temp.path();
    Fixture() {
      fs::create_directory(path / "a");
      fs::create_directory(path / "b");
      fs::create_directory(path / "c");
    }
    fs::path state() const { return path / "state" / "folder_views.toml"; }
  };

  void theStoreKeepsWorkbenchAndRefusesWhatItDoesNotKnow() {
    Fixture fixture;
    fs::create_directories(fixture.state().parent_path());
    std::ofstream(fixture.state()) << "schema_version = 1\n"
                                      "[[folders]]\nuri = \"file:///a\"\nview_mode = \"workbench\"\n"
                                      "[[folders]]\nuri = \"file:///b\"\nview_mode = \"cube\"\n"
                                      "[[folders]]\nuri = \"file:///c\"\nview_mode = 3\n"
                                      "[[folders]]\nuri = \"file:///d\"\nview_mode = \"grid\"\n";
    FolderViewStore store(fixture.state());
    store.load();
    TEST_CHECK(store.issues().size() == 2);
    TEST_CHECK(store.entries().size() == 2);
    TEST_CHECK(store.entries()[0].uri == "file:///a" && store.entries()[0].view.mode == ViewMode::Workbench);
    TEST_CHECK(store.entries()[1].uri == "file:///d" && store.entries()[1].view.mode == ViewMode::Grid);

    const std::vector<luciole::FolderViewEntry> saved{
        {.uri = "file:///w", .view = {.sort = std::nullopt, .mode = ViewMode::Workbench}}
    };
    TEST_CHECK(store.save(saved) != luciole::AtomicWriteState::NotCommitted);
    FolderViewStore reloaded(fixture.state());
    reloaded.load();
    TEST_CHECK(reloaded.issues().empty() && reloaded.entries() == saved);
  }

  void aFolderComesBackInTheViewItWasLeftIn() {
    Fixture fixture;
    Pump pump;
    const auto a = Location::fromPath(fixture.path / "a");
    const auto b = Location::fromPath(fixture.path / "b");
    const auto c = Location::fromPath(fixture.path / "c");
    FolderViewService views(fixture.state(), true);
    int changes = 0;
    views.setChangedCallback([&] { ++changes; });
    pump.until([&] { return changes == 1; });
    BrowserController browser(SortOptions{}, false);
    browser.setViewDefaults(ViewMode::List, luciole::kDefaultGridIconSize);
    browser.setFolderViews(&views);
    views.setMode(a, ViewMode::Workbench);
    views.setMode(b, ViewMode::List);

    // Remembered in Workbench: Workbench, whatever the default.
    browser.open(a);
    pump.until([&] { return browser.model().snapshot().status == DirectoryStatus::Ready; });
    TEST_CHECK(browser.viewport().mode == ViewMode::Workbench);
    // Not remembered: the default, never Workbench by itself.
    browser.open(c);
    TEST_CHECK(browser.viewport().mode == ViewMode::List);
    TEST_CHECK(browser.back() && browser.viewport().mode == ViewMode::Workbench);

    // A Workbench default does not pull a folder remembered in a classic view into Workbench.
    browser.setDefaultViewMode(ViewMode::Workbench);
    browser.open(b);
    TEST_CHECK(browser.viewport().mode == ViewMode::List);
    browser.open(c);
    TEST_CHECK(browser.viewport().mode == ViewMode::Workbench);

    // Forgetting the folder's choice returns it to the default.
    views.reset(a);
    browser.open(a);
    TEST_CHECK(browser.viewport().mode == ViewMode::Workbench);
    pump.until([&] { return !views.busy(); });
  }
} // namespace

int main() {
  try {
    theStoreKeepsWorkbenchAndRefusesWhatItDoesNotKnow();
    aFolderComesBackInTheViewItWasLeftIn();
    std::println("folder views workbench checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "folder views workbench test: {}", error.what());
    return 1;
  }
}
