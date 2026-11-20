#include "browser/lookahead_columns.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::BrowserController;
  using luciole::DirectoryModel;
  using luciole::DirectoryStatus;
  using luciole::Location;
  using luciole::LookaheadColumns;
  using luciole::LookaheadPreview;
  using luciole::LookaheadRequest;
  using luciole::WorkbenchEntry;

  constexpr std::size_t kSweep = 100;

  // root/a/{nested/{deeper/leaf.txt, kept.txt}, other/, file.txt, b.txt, .hidden.txt}, root/b/, root/sweep/f000..f099/
  class Fixture {
  public:
    Fixture() {
      fs::create_directories(path / "a" / "nested" / "deeper");
      fs::create_directories(path / "a" / "other");
      fs::create_directory(path / "b");
      std::ofstream(path / "a" / "nested" / "deeper" / "leaf.txt") << "leaf";
      std::ofstream(path / "a" / "nested" / "kept.txt") << "kept";
      std::ofstream(path / "a" / "file.txt") << "file";
      std::ofstream(path / "a" / "b.txt") << "b";
      std::ofstream(path / "a" / ".hidden.txt") << "hidden";
      for (std::size_t i = 0; i < kSweep; ++i) {
        fs::create_directories(path / "sweep" / std::format("f{:03}", i));
      }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    Location at(const fs::path& relative) const { return Location::fromPath((path / relative).native()); }
    WorkbenchEntry folder(const fs::path& relative) const { return {.location = at(relative), .folder = true}; }
    WorkbenchEntry file(const fs::path& relative) const { return {.location = at(relative), .folder = false}; }

    luciole::test::TempFolder tempFolder{"luciole-lookahead"};
    fs::path path = tempFolder.path();
  };

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        once();
      }
    }
    void once() {
      std::vector<pollfd> fds;
      const auto start = m_source.addPollFds(fds);
      const int requested = m_source.pollTimeoutMs();
      TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0);
      m_source.dispatch(fds, start);
    }
    void settled(const LookaheadColumns& columns) {
      until([&] {
        for (std::size_t i = 0; i < columns.count(); ++i) {
          if (columns.model(i).snapshot().status == DirectoryStatus::Loading) {
            return false;
          }
        }
        return true;
      });
    }

  private:
    GlibPollSource m_source;
  };

  LookaheadColumns makeColumns() { return LookaheadColumns({}, false, true); }

  std::vector<std::string> names(const DirectoryModel& model) {
    std::vector<std::string> result;
    for (const auto id : model.rows()) {
      result.push_back(model.entry(id)->name);
    }
    return result;
  }

  void testFolderTargetFillsColumnOne() {
    Fixture fixture;
    Pump pump;
    auto columns = makeColumns();
    const auto serial = columns.serial();
    columns.follow({.target = fixture.folder("a")});
    TEST_CHECK(columns.serial() > serial);
    TEST_CHECK(columns.count() == 1);
    TEST_CHECK(columns.location(0) == fixture.at("a"));
    TEST_CHECK(!columns.preview());
    pump.settled(columns);
    TEST_CHECK(columns.model(0).snapshot().status == DirectoryStatus::Ready);
    // The user's order (folders first, then names) and no hidden entries.
    TEST_CHECK((names(columns.model(0)) == std::vector<std::string>{"nested", "other", "b.txt", "file.txt"}));
    const auto ready = columns.serial();
    columns.follow({.target = fixture.folder("a")});
    TEST_CHECK(columns.serial() == ready);
  }

  void testListingFollowsSortAndHiddenSettings() {
    Fixture fixture;
    Pump pump;
    LookaheadColumns columns(
        {.field = luciole::SortField::Name, .descending = true, .directoriesFirst = false}, true, true
    );
    columns.follow({.target = fixture.folder("a")});
    pump.settled(columns);
    TEST_CHECK(
        (names(columns.model(0)) == std::vector<std::string>{"other", "nested", "file.txt", "b.txt", ".hidden.txt"})
    );
    const auto serial = columns.serial();
    columns.setListing({}, false, true);
    TEST_CHECK(columns.serial() > serial);
    TEST_CHECK((names(columns.model(0)) == std::vector<std::string>{"nested", "other", "b.txt", "file.txt"}));
    // A kept listing takes the settings too, before it reloads.
    columns.follow({.target = fixture.folder("b")});
    pump.settled(columns);
    columns.setListing({}, true, true);
    columns.follow({.target = fixture.folder("a")});
    TEST_CHECK(
        (names(columns.model(0)) == std::vector<std::string>{"nested", "other", ".hidden.txt", "b.txt", "file.txt"})
    );
    // So does a listing made later.
    std::ofstream(fixture.path / "a" / "nested" / ".dot") << "dot";
    columns.follow({.target = fixture.folder("a/nested")});
    pump.settled(columns);
    TEST_CHECK((names(columns.model(0)) == std::vector<std::string>{"deeper", ".dot", "kept.txt"}));
  }

  void testColumnTwoPinHoldsWhenThePickMoves() {
    Fixture fixture;
    auto columns = makeColumns();
    luciole::WorkbenchState state;
    state.setCursor(fixture.folder("a"));
    state.pin(1, fixture.file("a/nested/kept.txt"));
    const auto request = [&](const WorkbenchEntry& pick) {
      LookaheadRequest result{.target = state.target()};
      result.pins = {state.pinned(0), state.pinned(1)};
      result.picks = {pick, std::nullopt};
      return result;
    };
    const LookaheadPreview kept{.file = fixture.at("a/nested/kept.txt"), .slot = 2};
    columns.follow(request(fixture.folder("a/nested")));
    TEST_CHECK(columns.count() == 2);
    TEST_CHECK(columns.preview() == kept);
    columns.follow(request(fixture.folder("a/other"))); // a growing measure moves the pick
    TEST_CHECK(columns.count() == 2);
    TEST_CHECK(columns.location(1) == fixture.at("a/nested"));
    TEST_CHECK(columns.preview() == kept);
  }

  void testChainFillsColumnsTwoAndThree() {
    Fixture fixture;
    Pump pump;
    auto columns = makeColumns();
    LookaheadRequest request{.target = fixture.folder("a")};
    request.picks = {fixture.folder("a/nested"), fixture.folder("a/nested/deeper")};
    columns.follow(request);
    TEST_CHECK(columns.count() == 3);
    TEST_CHECK(columns.location(1) == fixture.at("a/nested"));
    TEST_CHECK(columns.location(2) == fixture.at("a/nested/deeper"));
    pump.settled(columns);
    TEST_CHECK((names(columns.model(1)) == std::vector<std::string>{"deeper", "kept.txt"}));
    TEST_CHECK((names(columns.model(2)) == std::vector<std::string>{"leaf.txt"}));
    TEST_CHECK(columns.load().running == 3);
  }

  void testPickOutsideTheColumnIsIgnored() {
    Fixture fixture;
    auto columns = makeColumns();
    LookaheadRequest request{.target = fixture.folder("a")};
    request.picks = {fixture.folder("b"), fixture.folder("a/nested/deeper")};
    columns.follow(request);
    TEST_CHECK(columns.count() == 1);
  }

  void testFileTargetIsAPreviewAndNoListing() {
    Fixture fixture;
    auto columns = makeColumns();
    columns.follow({.target = fixture.file("a/file.txt")});
    TEST_CHECK(columns.count() == 0);
    TEST_CHECK((columns.preview() == LookaheadPreview{.file = fixture.at("a/file.txt"), .slot = 0}));
    TEST_CHECK(columns.load().alive == 0);
    columns.follow({});
    TEST_CHECK(!columns.preview());
  }

  void testPinOverridesTheChain() {
    Fixture fixture;
    auto columns = makeColumns();
    LookaheadRequest request{.target = fixture.folder("a")};
    request.picks = {fixture.folder("a/nested"), fixture.folder("a/nested/deeper")};
    request.pins = {fixture.folder("a/other"), std::nullopt};
    columns.follow(request);
    TEST_CHECK(columns.count() == 2); // the pick for the third column is not inside the pinned folder
    TEST_CHECK(columns.location(1) == fixture.at("a/other"));
    request.pins = {fixture.file("a/file.txt"), std::nullopt};
    columns.follow(request);
    TEST_CHECK(columns.count() == 1);
    TEST_CHECK((columns.preview() == LookaheadPreview{.file = fixture.at("a/file.txt"), .slot = 1}));
    request.pins = {std::nullopt, fixture.file("a/nested/kept.txt")};
    columns.follow(request);
    TEST_CHECK(columns.count() == 2);
    TEST_CHECK((columns.preview() == LookaheadPreview{.file = fixture.at("a/nested/kept.txt"), .slot = 2}));
    request.pins = {fixture.folder("b"), std::nullopt}; // not inside column one: the chain is used
    columns.follow(request);
    TEST_CHECK(columns.count() == 3);
    TEST_CHECK(!columns.preview());
  }

  void testRetargetReusesLoadedFolders() {
    Fixture fixture;
    Pump pump;
    auto columns = makeColumns();
    columns.follow({.target = fixture.folder("a")});
    pump.settled(columns);
    const auto* first = &columns.model(0);
    columns.follow({.target = fixture.folder("b")});
    pump.settled(columns);
    TEST_CHECK(first->snapshot().status == DirectoryStatus::Cancelled); // stopped while not shown
    TEST_CHECK(columns.load().running == 1);
    columns.follow({.target = fixture.folder("a")});
    TEST_CHECK(&columns.model(0) == first);
    TEST_CHECK(names(columns.model(0)).size() == 4); // its rows stay while it refreshes
    pump.settled(columns);
    TEST_CHECK(columns.model(0).snapshot().status == DirectoryStatus::Ready);
    // A folder shown in column two moves to column one without loading again.
    LookaheadRequest request{.target = fixture.folder("a")};
    request.picks = {fixture.folder("a/nested"), std::nullopt};
    columns.follow(request);
    pump.settled(columns);
    const auto* nested = &columns.model(1);
    columns.follow({.target = fixture.folder("a/nested")});
    TEST_CHECK(&columns.model(0) == nested);
    TEST_CHECK(columns.model(0).snapshot().status == DirectoryStatus::Ready);
  }

  void testSweepKeepsABoundedSet() {
    Fixture fixture;
    Pump pump;
    auto columns = makeColumns();
    for (std::size_t i = 0; i < kSweep; ++i) {
      columns.follow({.target = fixture.folder(fs::path("sweep") / std::format("f{:03}", i))});
      pump.once();
      const auto load = columns.load();
      TEST_CHECK(load.running <= LookaheadColumns::kColumns);
      TEST_CHECK(load.alive <= LookaheadColumns::kColumns + LookaheadColumns::kSpare + 1);
      columns.collectRetired();
    }
    pump.settled(columns);
    TEST_CHECK(columns.load().running == 1);
    TEST_CHECK(columns.load().alive == 1 + LookaheadColumns::kSpare);
    columns.clear();
    TEST_CHECK(columns.count() == 0);
    TEST_CHECK(columns.load().running == 0);
    columns.collectRetired();
    TEST_CHECK(columns.load().alive == 0);
  }

  void testFolderThatDisappearsWhileShown() {
    Fixture fixture;
    Pump pump;
    auto columns = makeColumns();
    LookaheadRequest request{.target = fixture.folder("a")};
    request.picks = {fixture.folder("a/nested"), fixture.folder("a/nested/deeper")};
    columns.follow(request);
    pump.settled(columns);
    fs::remove_all(fixture.path / "a" / "nested");
    pump.until([&] {
      return columns.model(1).snapshot().status == DirectoryStatus::Error
          && columns.model(2).snapshot().status == DirectoryStatus::Error
          && names(columns.model(0)).size() == 3;
    });
    TEST_CHECK(columns.count() == 3); // the columns stay until the request changes
    columns.follow({.target = fixture.folder("a")});
    TEST_CHECK(columns.count() == 1);
    TEST_CHECK(columns.load().running == 1);
  }

  void testUnreadableFolder() {
    Fixture fixture;
    Pump pump;
    fs::create_directories(fixture.path / "locked" / "inside");
    fs::permissions(fixture.path / "locked", fs::perms::none);
    auto columns = makeColumns();
    columns.follow({.target = fixture.folder("locked")});
    pump.settled(columns);
    TEST_CHECK(columns.count() == 1);
    TEST_CHECK(columns.model(0).snapshot().status == DirectoryStatus::Error);
    TEST_CHECK(columns.model(0).snapshot().error.has_value());
    TEST_CHECK(columns.model(0).rows().empty());
  }

  void testPageControllerIsNeverTouched() {
    Fixture fixture;
    Pump pump;
    BrowserController page({}, false);
    std::size_t pageChanges = 0;
    page.setChangedCallback([&] { ++pageChanges; });
    page.open(fixture.at("a"));
    pump.until([&] { return page.model().snapshot().status == DirectoryStatus::Ready; });
    // The folder's own access and filesystem queries finish on their own and notify the page.
    pump.until([&] {
      const auto& snapshot = page.model().snapshot();
      return !snapshot.contentTypesPending && !snapshot.metadataLoading;
    });
    const auto generation = page.model().snapshot().generation;
    const auto visits = page.visitSerial();
    const auto before = pageChanges;
    auto columns = makeColumns();
    const auto rows = page.model().rows();
    for (const auto id : rows) {
      const auto* entry = page.model().entry(id);
      columns.follow(
          {.target = WorkbenchEntry{
               .location = entry->location(),
               .folder = entry->type == luciole::FileType::Directory,
           }}
      );
      pump.settled(columns);
    }
    columns.clear();
    columns.collectRetired();
    TEST_CHECK(pageChanges == before);
    TEST_CHECK(page.model().snapshot().generation == generation);
    TEST_CHECK(page.visitSerial() == visits);
    TEST_CHECK(page.model().selection().selected.empty());
    TEST_CHECK(!page.canBack());
    TEST_CHECK(page.model().snapshot().location == fixture.at("a"));
  }

  void testChangedCallbackReportsListings() {
    Fixture fixture;
    Pump pump;
    auto columns = makeColumns();
    std::size_t changes = 0;
    columns.setChangedCallback([&] { ++changes; });
    columns.follow({.target = fixture.folder("a")});
    pump.settled(columns);
    TEST_CHECK(changes >= 2); // the new column, then its listing
  }
} // namespace

int main() {
  testFolderTargetFillsColumnOne();
  testListingFollowsSortAndHiddenSettings();
  testChainFillsColumnsTwoAndThree();
  testPickOutsideTheColumnIsIgnored();
  testFileTargetIsAPreviewAndNoListing();
  testPinOverridesTheChain();
  testColumnTwoPinHoldsWhenThePickMoves();
  testRetargetReusesLoadedFolders();
  testSweepKeepsABoundedSet();
  testFolderThatDisappearsWhileShown();
  testUnreadableFolder();
  testPageControllerIsNeverTouched();
  testChangedCallbackReportsListings();
  return 0;
}
