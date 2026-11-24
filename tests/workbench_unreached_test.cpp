#include "browser/entry_details.h"
#include "browser/workbench_model.h"
#include "browser/workbench_services.h"
#include "event_loop/glib_poll_source.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::Location;

  struct Scratch : luciole::test::TempFolder {
    Scratch() : TempFolder("luciole-unreached-model") {}
  };

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 5 : std::min(requested, 5)) >= 0);
        m_source.dispatch(fds, start);
      }
    }

  private:
    GlibPollSource m_source;
  };

  void write(const fs::path& path, std::size_t bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');
    TEST_CHECK(out.good());
  }

  // Folder `i` of `count` holds `files + (step * i)` files of 100 bytes.
  fs::path tree(const fs::path& root, int count, int files, int step = 1) {
    for (int i = 0; i < count; ++i) {
      for (int file = 0; file < files + (step * i); ++file) {
        write(root / std::format("s{:04}", i) / std::format("f{}", file), 100);
      }
    }
    return root;
  }

  std::string whole(int i, int files, int step) {
    return luciole::folderSizeText(100 * static_cast<std::uint64_t>(files + (step * i)), false);
  }

  // Services whose walks end after `budget` entries; the optional gate holds a worker after a partial table.
  struct Rig {
    explicit Rig(std::uint64_t budget, std::function<void()> hold = {})
        : service(options(budget, std::move(hold))), cache(service) {}
    static luciole::MeasureServiceOptions options(std::uint64_t budget, std::function<void()> hold) {
      luciole::MeasureServiceOptions result;
      result.publishInterval = std::chrono::milliseconds(1);
      result.walk.entryBudget = budget;
      result.afterPartial = std::move(hold);
      return result;
    }
    luciole::SubtreeMeasureService service;
    luciole::MeasureCache cache;
  };

  void settle(Pump& pump, luciole::WorkbenchModel& model) {
    pump.until([&] { return model.snapshot() && !model.measuring(); });
  }

  int exactCells(const luciole::WorkbenchSnapshot& snapshot, int count, int files, int step, int& lowerBounds) {
    int exact = 0;
    lowerBounds = 0;
    for (int i = 0; i < count; ++i) {
      const auto text = snapshot.folderSizeText(std::format("s{:04}", i));
      if (text == whole(i, files, step)) {
        ++exact;
      } else if (text.starts_with("≥ ")) {
        ++lowerBounds;
      }
    }
    return exact;
  }

  // The walk of the folder ends inside one of its folders, whichever that is; the ones after it are never entered.
  // Each gets its exact size from the pass, the folder that was cut stays a lower bound, and the totals follow.
  void sizesOfFoldersTheWalkNeverEntered(const fs::path& base) {
    constexpr int kCount = 8;
    // 10, 12 ... 24 files of 100 bytes: 13,600 bytes. The budget is odd and the prefix sums of the counts are even,
    // so the walk never ends exactly between two folders.
    const auto folder = tree(base / "siblings", kCount, 10, 2);
    Rig rig(kCount + 31);
    luciole::WorkbenchModel model(rig.cache, {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(Location::fromPath(folder.native()));
    settle(pump, model);
    int cut = 0;
    TEST_CHECK(exactCells(*model.snapshot(), kCount, 10, 2, cut) == kCount - 1 && cut == 1);
    const auto& tree = model.snapshot()->tree;
    const auto& root = tree.nodes[tree.root];
    TEST_CHECK(root.lowerBound && root.bytes >= 13600 - 2400 && root.bytes < 13600);
    // A folder the walk entered, whole or cut, is not walked again alone; the ones it never entered were.
    int walkedAlone = 0;
    for (int i = 0; i < kCount; ++i) {
      const auto place = Location::fromPath((folder / std::format("s{:04}", i)).native());
      const bool alone = rig.cache.find(place).record == 0;
      walkedAlone += alone ? 1 : 0;
      if (model.snapshot()->folderSizeText(std::format("s{:04}", i)).starts_with("≥ ")) {
        TEST_CHECK(!alone);
      }
    }
    TEST_CHECK(walkedAlone >= 5 && walkedAlone < kCount - 1);
    const auto status = rig.service.status();
    TEST_CHECK(status.queued == 0 && status.running == 0);
  }

  // A walk that ends between two folders, with none cut: after the pass the folder's total is exact, not a lower bound.
  void totalOfAFolderNobodyCut(const fs::path& base) {
    constexpr int kCount = 6;
    for (int i = 0; i < kCount; ++i) {
      for (int file = 0; file < 10; ++file) {
        write(base / "boundary" / std::format("s{:04}", i) / std::format("f{}", file), 100);
      }
    }
    Rig rig(kCount + 10); // the root's entries and the ten files of the first folder entered
    luciole::WorkbenchModel model(rig.cache, {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(Location::fromPath((base / "boundary").native()));
    settle(pump, model);
    const auto& tree = model.snapshot()->tree;
    const auto& root = tree.nodes[tree.root];
    TEST_CHECK(!root.lowerBound && root.bytes == 6000 && root.files == 60);
    for (int i = 0; i < kCount; ++i) {
      TEST_CHECK(model.snapshot()->folderSizeText(std::format("s{:04}", i)) == "1000 bytes");
    }
  }

  // More folders than the picture names: the ones its table folded are filled too.
  void foldersBeyondTheNamedOnes(const fs::path& base) {
    constexpr int kCount = 600;
    const auto folder = tree(base / "wide", kCount, 3, 0);
    Rig rig(kCount + 4); // the root's entries, the three files of one folder and one of the next
    luciole::WorkbenchModel model(rig.cache, {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(Location::fromPath(folder.native()));
    settle(pump, model);
    int cut = 0;
    TEST_CHECK(exactCells(*model.snapshot(), kCount, 3, 0, cut) == kCount - 1 && cut == 1);
  }

  // A folder with one folder the walk entered and cut has nothing for the pass; with two, whichever one is entered
  // first, one is exact and one is a lower bound.
  void fewFolders(const fs::path& base) {
    const auto none = tree(base / "none", 1, 10);
    Rig rig(1 + 3);
    luciole::WorkbenchModel model(rig.cache, {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(Location::fromPath(none.native()));
    settle(pump, model);
    int cut = 0;
    TEST_CHECK(exactCells(*model.snapshot(), 1, 10, 1, cut) == 0 && cut == 1);
    const auto one = tree(base / "one", 2, 10, 10);
    Rig second(2 + 15);
    luciole::WorkbenchModel again(second.cache, {.ancestorLevels = 0});
    again.setLocation(Location::fromPath(one.native()));
    settle(pump, again);
    TEST_CHECK(exactCells(*again.snapshot(), 2, 10, 10, cut) == 1 && cut == 1);
  }

  // Walks the pass started stop when the page goes to another folder, and what they counted is not applied.
  struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> armed = false;
    bool open = false;
    int held = 0;
    std::atomic<int> paceMs = 0; // when set, a partial table only waits this long
    void hold() {
      if (!armed) {
        return;
      }
      if (paceMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(paceMs));
        return;
      }
      std::unique_lock lock(mutex);
      ++held;
      changed.wait_for(lock, std::chrono::seconds(30), [&] { return open; });
    }
    void release() {
      const std::lock_guard lock(mutex);
      open = true;
      armed = false;
      changed.notify_all();
    }
    int heldNow() {
      const std::lock_guard lock(mutex);
      return held;
    }
  };

  int neverEntered(const luciole::MeasureCache& cache, const Location& folder) {
    const auto found = cache.find(folder);
    int count = 0;
    if (const auto* top = found.entry()) {
      for (auto c = top->firstChild; c < top->firstChild + top->childCount; ++c) {
        count += found.table->records[c].flags.notEntered ? 1 : 0;
      }
    }
    return count;
  }

  void leavingTheFolderMidPass(const fs::path& base) {
    constexpr int kCount = 6;
    const auto folder = tree(base / "leaving", kCount, 1500);
    write(base / "elsewhere" / "file", 10);
    Gate gate;
    Rig rig(kCount + 2000, [&gate] { gate.hold(); });
    luciole::WorkbenchModel model(rig.cache, {.ancestorLevels = 0});
    Pump pump;
    const auto place = Location::fromPath(folder.native());
    model.setLocation(place);
    pump.until([&] {
      const auto found = rig.cache.find(place);
      return found.entry() != nullptr && found.record == 0 && !found.measuring && !found.stale;
    });
    gate.armed = true;
    pump.until([&] { return gate.heldNow() > 0; });
    TEST_CHECK(model.measuring() && neverEntered(rig.cache, place) == kCount - 2); // one whole, one cut
    model.setLocation(Location::fromPath((base / "elsewhere").native()));
    gate.release();
    pump.until([&] { return rig.service.status().running == 0 && rig.service.status().queued == 0; });
    settle(pump, model);
    // Nothing the cancelled walks counted reached the table of the folder that was left.
    TEST_CHECK(neverEntered(rig.cache, place) == kCount - 2 || rig.cache.find(place).entry() == nullptr);
  }

  // Refresh while the pass runs: its walks stop, the folder is measured again and the pass runs once more, ending
  // with exact sizes; no walk is left behind.
  void refreshMidPass(const fs::path& base) {
    constexpr int kCount = 6;
    const auto folder = tree(base / "refreshing", kCount, 1500);
    Gate gate;
    Rig rig(kCount + 2000, [&gate] { gate.hold(); });
    luciole::WorkbenchModel model(rig.cache, {.ancestorLevels = 0});
    Pump pump;
    const auto place = Location::fromPath(folder.native());
    model.setLocation(place);
    pump.until([&] {
      const auto found = rig.cache.find(place);
      return found.entry() != nullptr && found.record == 0 && !found.measuring && !found.stale;
    });
    gate.armed = true;
    pump.until([&] { return gate.heldNow() > 0; });
    model.remeasure();
    model.remeasure();
    gate.release();
    settle(pump, model);
    pump.until([&] { return rig.service.status().running == 0 && rig.service.status().queued == 0; });
    int cut = 0;
    TEST_CHECK(exactCells(*model.snapshot(), kCount, 1500, 1, cut) == kCount - 1 && cut == 1);
    TEST_CHECK(neverEntered(rig.cache, place) == 0);
  }
  // Refresh on a folder with never-entered folders: from F5 to the end, no cell that showed a size is blank in any
  // snapshot, the instrument agrees with the cell, and in the end every size is the new one.
  void sizesStayThroughRefresh(const fs::path& base) {
    constexpr int kCount = 8;
    const auto folder = tree(base / "stay", kCount, 1500);
    Gate gate; // slows each walk after the first measure, so snapshots are built while the pass runs
    Rig rig(kCount + 2000, [&gate] { gate.hold(); });
    luciole::WorkbenchModel model(rig.cache, {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(Location::fromPath(folder.native()));
    settle(pump, model);
    int cut = 0;
    TEST_CHECK(exactCells(*model.snapshot(), kCount, 1500, 1, cut) == kCount - 1 && cut == 1);
    gate.paceMs = 15;
    gate.armed = true;
    std::vector<std::string> shown;
    for (int i = 0; i < kCount; ++i) {
      shown.push_back(model.snapshot()->folderSizeText(std::format("s{:04}", i)));
    }
    std::vector<std::shared_ptr<const luciole::WorkbenchSnapshot>> seen;
    model.setChangedCallback([&] {
      if (const auto latest = model.snapshot(); latest && (seen.empty() || seen.back() != latest)) {
        seen.push_back(latest);
      }
    });
    model.remeasure();
    settle(pump, model);
    model.setChangedCallback({});
    TEST_CHECK(exactCells(*model.snapshot(), kCount, 1500, 1, cut) == kCount - 1 && cut == 1);
    for (const auto& snapshot : seen) {
      for (int i = 0; i < kCount; ++i) {
        const auto name = std::format("s{:04}", i);
        TEST_CHECK(!snapshot->folderSizeText(name).empty());
        const auto id = snapshot->node(name);
        TEST_CHECK(id != luciole::workbench::kNoNode && !snapshot->tree.nodes[id].notEntered);
      }
    }
  }

  // A second Refresh before the first one has ended, at any point of it: no snapshot has a blank cell, a root with
  // nothing counted, or a row that is not entered, from the first press to the end.
  void sizesStayThroughTwoRefreshes(const fs::path& base) {
    constexpr int kCount = 8;
    const auto folder = tree(base / "twice", kCount, 1500);
    for (const int delayMs : {0, 60, 200, 500, 900}) {
      Gate gate;
      Rig rig(kCount + 2000, [&gate] { gate.hold(); });
      luciole::WorkbenchModel model(rig.cache, {.ancestorLevels = 0});
      Pump pump;
      model.setLocation(Location::fromPath(folder.native()));
      settle(pump, model);
      gate.paceMs = 15;
      gate.armed = true;
      std::vector<std::shared_ptr<const luciole::WorkbenchSnapshot>> seen;
      model.setChangedCallback([&] {
        if (const auto latest = model.snapshot(); latest && (seen.empty() || seen.back() != latest)) {
          seen.push_back(latest);
        }
      });
      model.remeasure();
      const auto second = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
      pump.until([&] { return std::chrono::steady_clock::now() >= second; });
      model.remeasure();
      settle(pump, model);
      model.setChangedCallback({});
      for (const auto& snapshot : seen) {
        const auto& root = snapshot->tree.nodes[snapshot->tree.root];
        TEST_CHECK(root.bytes > 0);
        for (int i = 0; i < kCount; ++i) {
          const auto name = std::format("s{:04}", i);
          TEST_CHECK(!snapshot->folderSizeText(name).empty());
          const auto id = snapshot->node(name);
          TEST_CHECK(id != luciole::workbench::kNoNode && !snapshot->tree.nodes[id].notEntered);
        }
      }
      int cut = 0;
      TEST_CHECK(exactCells(*model.snapshot(), kCount, 1500, 1, cut) == kCount - 1 && cut == 1);
    }
  }

  // Refresh while a walk runs: the next walk starts when that one ends, not after the wait a changing folder gets.
  void refreshStartsAtOnce(const fs::path& base) {
    const auto folder = tree(base / "atonce", 3, 300, 0);
    Gate gate;
    gate.armed = true;
    Rig rig(2'000'000, [&gate] { gate.hold(); });
    luciole::WorkbenchModel model(rig.cache, {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(Location::fromPath(folder.native()));
    pump.until([&] { return gate.heldNow() > 0; });
    model.remeasure();
    const auto released = std::chrono::steady_clock::now();
    gate.release();
    pump.until([&] { return model.snapshot() && !model.measuring(); });
    TEST_CHECK(std::chrono::steady_clock::now() - released < std::chrono::milliseconds(700));
  }
} // namespace

int main() {
  const Scratch scratch;
  sizesOfFoldersTheWalkNeverEntered(scratch.path());
  totalOfAFolderNobodyCut(scratch.path());
  foldersBeyondTheNamedOnes(scratch.path());
  fewFolders(scratch.path());
  leavingTheFolderMidPass(scratch.path());
  refreshMidPass(scratch.path());
  refreshStartsAtOnce(scratch.path());
  sizesStayThroughRefresh(scratch.path());
  sizesStayThroughTwoRefreshes(scratch.path());
  return 0;
}
