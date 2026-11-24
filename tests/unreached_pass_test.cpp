#include "browser/measure_cache.h"
#include "browser/unreached_pass.h"
#include "fs/subtree_measure_walk.h"
#include "temp_folder.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <glib.h>
#include <string>
#include <thread>

namespace {
  namespace fs = std::filesystem;
  using luciole::Location;
  using luciole::MeasureCache;
  using luciole::MeasureTable;
  using luciole::SubtreeMeasureService;
  using luciole::UnreachedPass;

  struct Scratch : luciole::test::TempFolder {
    Scratch() : TempFolder("luciole-unreached") {}
  };

  void pumpUntil(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void writeFile(const fs::path& path, std::size_t bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');
    TEST_CHECK(out.good());
  }

  // `count` folders of `files + i` files of 100 bytes each, and a 7 byte file.
  Location tree(const fs::path& root, int count, int files) {
    writeFile(root / "top", 7);
    for (int i = 0; i < count; ++i) {
      for (int file = 0; file < files + i; ++file) {
        writeFile(root / std::format("s{:03}", i) / std::format("f{}", file), 100);
      }
    }
    return Location::fromPath(root.native());
  }

  // A service whose root walk ends after the root's own entries and `extra` more.
  struct Rig {
    Rig(std::uint64_t budget, std::size_t workers = 2) : service(options(budget, workers)), cache(service) {}
    static luciole::MeasureServiceOptions options(std::uint64_t budget, std::size_t workers) {
      luciole::MeasureServiceOptions result;
      result.publishInterval = std::chrono::milliseconds(1);
      result.workers = workers;
      result.walk.entryBudget = budget;
      return result;
    }
    // The finished table of the root.
    std::shared_ptr<const MeasureTable> settle(const Location& folder) {
      luciole::MeasureFind found;
      pumpUntil([&] {
        found = cache.measure(folder, luciole::MeasurePriority::Current);
        return found.entry() != nullptr && !found.measuring;
      });
      return found.table;
    }
    // Runs the pass until it has nothing pending; the largest number of its walks at once is `peak`.
    void run(UnreachedPass& pass, const Location& folder, std::size_t slots) {
      pumpUntil([&] {
        const auto found = cache.find(folder);
        pass.update(folder, found.table, slots);
        peak = std::max(peak, pass.running());
        return !pass.pending();
      });
    }
    SubtreeMeasureService service;
    MeasureCache cache;
    std::size_t peak = 0;
  };

  std::uint64_t sizeOf(const MeasureTable& table, const std::string& name) {
    const auto& top = table.records[0];
    for (auto c = top.firstChild; c < top.firstChild + top.childCount; ++c) {
      if (table.records[c].name == name) {
        return table.records[c].totals.apparentBytes;
      }
    }
    return top.folded && top.folded->find(name) ? top.folded->find(name)->apparentBytes : UINT64_MAX;
  }

  int neverEntered(const MeasureTable& table) {
    int count = 0;
    const auto& top = table.records[0];
    for (auto c = top.firstChild; c < top.firstChild + top.childCount; ++c) {
      count += table.records[c].flags.notEntered ? 1 : 0;
    }
    if (top.folded) {
      for (const auto& entry : top.folded->entries) {
        count += (entry.flags & luciole::FoldedChildren::kNotEntered) != 0 ? 1 : 0;
      }
    }
    return count;
  }

  // A walk with every folder entered has nothing to do and starts nothing.
  void nothingNeverEntered(const fs::path& base) {
    Rig rig(2'000'000);
    const auto folder = tree(base / "whole", 4, 3);
    UnreachedPass pass(rig.cache);
    const auto table = rig.settle(folder);
    TEST_CHECK(neverEntered(*table) == 0);
    pass.update(folder, table, 2);
    TEST_CHECK(!pass.pending() && pass.running() == 0 && rig.cache.find(folder).table == table);
    pass.update(folder, nullptr, 2);
    TEST_CHECK(!pass.pending());
  }

  // Whichever folder the budget cut and whichever it never entered, every other folder ends with its exact size, the
  // cut folder stays as it was, and the root is a lower bound with the sum of what is known.
  void sizesOfNeverEnteredFolders(const fs::path& base) {
    for (const int count : {1, 2, 9}) {
      Rig rig(count + 1 + 4); // the root's entries, then four files of the entered folder
      const auto folder = tree(base / std::format("many{}", count), count, 5);
      UnreachedPass pass(rig.cache);
      const auto table = rig.settle(folder);
      TEST_CHECK(neverEntered(*table) == count - 1);
      rig.run(pass, folder, 2);
      const auto after = rig.cache.find(folder).table;
      TEST_CHECK(neverEntered(*after) == 0);
      std::uint64_t sum = 7;
      int cut = 0;
      for (int i = 0; i < count; ++i) {
        const auto name = std::format("s{:03}", i);
        const auto bytes = sizeOf(*after, name);
        const auto whole = 100 * static_cast<std::uint64_t>(5 + i);
        if (bytes == whole) {
          sum += whole;
        } else {
          ++cut;
          sum += bytes;
          TEST_CHECK(bytes < whole);
        }
      }
      TEST_CHECK(cut == 1);
      TEST_CHECK(after->records[0].totals.apparentBytes == sum && after->records[0].flags.truncated);
      TEST_CHECK(rig.peak <= 2);
    }
  }

  // Past the cap of children, the rest stay never entered; past the entry budget no further folder starts.
  void limits(const fs::path& base) {
    Rig rig(12 + 1 + 1, 1); // the root's entries and one file of the entered folder
    const auto folder = tree(base / "limits", 12, 4);
    const auto table = rig.settle(folder);
    TEST_CHECK(neverEntered(*table) == 11);
    UnreachedPass capped(rig.cache, {.children = 3, .entries = 2'000'000, .applyEvery = std::chrono::milliseconds(0)});
    rig.run(capped, folder, 1);
    TEST_CHECK(neverEntered(*rig.cache.find(folder).table) == 11 - 3);
    // Each folder counts at least its four first files: a budget of five stops after two of them with one worker.
    Rig again(12 + 1 + 1, 1);
    const auto second = tree(base / "limits2", 12, 4);
    const auto start = again.settle(second);
    UnreachedPass budgeted(again.cache, {.children = 100, .entries = 5, .applyEvery = std::chrono::milliseconds(0)});
    again.run(budgeted, second, 1);
    const auto left = neverEntered(*again.cache.find(second).table);
    TEST_CHECK(left > 0 && left < neverEntered(*start));
    TEST_CHECK(!budgeted.pending());
  }

  // Leaving the folder, or a table that is not the planned one, drops the pass: nothing runs or is applied after.
  void reset(const fs::path& base) {
    Rig rig(12 + 1 + 1, 1); // the root's entries and one file of the entered folder
    const auto folder = tree(base / "reset", 12, 4);
    const auto table = rig.settle(folder);
    UnreachedPass pass(rig.cache);
    pass.update(folder, table, 1);
    TEST_CHECK(pass.pending() && pass.running() == 1);
    pass.reset();
    TEST_CHECK(!pass.pending() && pass.running() == 0);
    pumpUntil([&] { return rig.service.status().running == 0 && rig.service.status().queued == 0; });
    TEST_CHECK(rig.cache.find(folder).table == table && neverEntered(*table) == 11);
    // A table made by a later walk is a new plan: the folders of the old one are not applied to it.
    pass.update(folder, table, 1);
    rig.cache.invalidate(folder);
    luciole::MeasureFind found;
    pumpUntil([&] {
      found = rig.cache.measure(folder, luciole::MeasurePriority::Current);
      return !found.measuring && !found.stale;
    });
    TEST_CHECK(found.table != table);
    pass.update(folder, found.table, 1);
    TEST_CHECK(pass.pending() && pass.running() == 1);
    rig.run(pass, folder, 1);
    TEST_CHECK(neverEntered(*rig.cache.find(folder).table) == 0);
  }
} // namespace

int main() {
  const Scratch scratch;
  nothingNeverEntered(scratch.path());
  sizesOfNeverEnteredFolders(scratch.path());
  limits(scratch.path());
  reset(scratch.path());
  return 0;
}
