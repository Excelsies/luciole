#include "browser/browser_controller.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <print>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;
  using luciole::BrowserController;
  using luciole::DirectoryStatus;
  using luciole::Location;

  struct Fixture {
    luciole::test::TempFolder tempFolder{"luciole-entries-test"};
    fs::path root = tempFolder.path();
    Fixture() {
      fs::create_directory(root / "a");
      fs::create_directory(root / "b");
      fs::create_directory(root / "c");
      std::ofstream(root / "file.txt") << "fixture";
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Location at(const char* name) const { return Location::fromPath((root / name).native()); }
  };

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 10 : std::min(requested, 10)) >= 0);
        m_source.dispatch(fds, start);
      }
    }
    // Waits until `browser` shows `location` with a final status, and returns that status.
    DirectoryStatus settle(const BrowserController& browser, const Location& location) {
      until([&] {
        const auto& snapshot = browser.model().snapshot();
        return snapshot.location == location && snapshot.status != DirectoryStatus::Loading;
      });
      return browser.model().snapshot().status;
    }
    void drainFor(std::chrono::milliseconds duration) {
      const auto deadline = std::chrono::steady_clock::now() + duration;
      until([&] { return std::chrono::steady_clock::now() >= deadline; });
    }

  private:
    GlibPollSource m_source;
  };

  struct Recorder {
    std::vector<Location> entered;
    explicit Recorder(BrowserController& browser) {
      browser.setEnteredCallback([this](const Location& location) { entered.push_back(location); });
    }
  };

  // Open, back and forward report a folder once it has loaded; a refresh does not.
  void testNavigationEntries() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    Recorder recorder(browser);
    const auto a = fixture.at("a");
    const auto b = fixture.at("b");
    browser.open(a);
    TEST_CHECK(recorder.entered.empty());
    TEST_CHECK(pump.settle(browser, a) == DirectoryStatus::Ready);
    TEST_CHECK((recorder.entered == std::vector{a}));
    browser.open(b);
    TEST_CHECK(pump.settle(browser, b) == DirectoryStatus::Ready);
    TEST_CHECK(browser.back());
    TEST_CHECK(pump.settle(browser, a) == DirectoryStatus::Ready);
    TEST_CHECK(browser.forward());
    TEST_CHECK(pump.settle(browser, b) == DirectoryStatus::Ready);
    browser.open(b);
    browser.refresh();
    pump.drainFor(100ms);
    TEST_CHECK(pump.settle(browser, b) == DirectoryStatus::Ready);
    TEST_CHECK((recorder.entered == std::vector{a, b, a, b}));

    // A folder left before it loads is not an entry.
    browser.open(fixture.at("c"));
    browser.open(a);
    TEST_CHECK(pump.settle(browser, a) == DirectoryStatus::Ready);
    pump.drainFor(100ms);
    TEST_CHECK((recorder.entered == std::vector{a, b, a, b, a}));
  }

  // A folder that fails to load is not an entry.
  void testFailedEntries() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    Recorder recorder(browser);
    const auto missing = fixture.at("missing");
    const auto file = fixture.at("file.txt");
    browser.open(missing);
    TEST_CHECK(pump.settle(browser, missing) == DirectoryStatus::Error);
    TEST_CHECK(browser.openText((fixture.root / "file.txt").native()));
    TEST_CHECK(pump.settle(browser, file) == DirectoryStatus::Error);
    TEST_CHECK(browser.back());
    TEST_CHECK(pump.settle(browser, missing) == DirectoryStatus::Error);
    pump.drainFor(100ms);
    TEST_CHECK(recorder.entered.empty());
    browser.open(fixture.at("a"));
    TEST_CHECK(pump.settle(browser, fixture.at("a")) == DirectoryStatus::Ready);
    TEST_CHECK((recorder.entered == std::vector{fixture.at("a")}));
  }

  // A duplicated history and a skipped opening are not entries; later navigation is.
  void testCopiedEntries() {
    Fixture fixture;
    Pump pump;
    const auto a = fixture.at("a");
    const auto b = fixture.at("b");
    BrowserController source({}, false);
    source.open(a);
    TEST_CHECK(pump.settle(source, a) == DirectoryStatus::Ready);
    source.open(b);
    TEST_CHECK(pump.settle(source, b) == DirectoryStatus::Ready);

    BrowserController copy({}, false);
    Recorder copied(copy);
    copy.open(b);
    copy.duplicateFrom(source);
    TEST_CHECK(pump.settle(copy, b) == DirectoryStatus::Ready);
    pump.drainFor(100ms);
    TEST_CHECK(copied.entered.empty());
    TEST_CHECK(copy.back());
    TEST_CHECK(pump.settle(copy, a) == DirectoryStatus::Ready);
    TEST_CHECK((copied.entered == std::vector{a}));

    BrowserController restored({}, false);
    Recorder reopened(restored);
    restored.open(a);
    restored.skipPendingEntry();
    TEST_CHECK(pump.settle(restored, a) == DirectoryStatus::Ready);
    pump.drainFor(100ms);
    TEST_CHECK(reopened.entered.empty());
    restored.open(b);
    TEST_CHECK(pump.settle(restored, b) == DirectoryStatus::Ready);
    TEST_CHECK((reopened.entered == std::vector{b}));

    // Without a callback nothing is reported, and nothing is kept for one set later.
    BrowserController untracked({}, false);
    untracked.open(a);
    TEST_CHECK(pump.settle(untracked, a) == DirectoryStatus::Ready);
    Recorder late(untracked);
    pump.drainFor(50ms);
    TEST_CHECK(late.entered.empty());
  }
} // namespace

int main() {
  try {
    testNavigationEntries();
    testFailedEntries();
    testCopiedEntries();
    std::println("browser entry checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "browser entries test: {}", error.what());
    return 1;
  }
}
