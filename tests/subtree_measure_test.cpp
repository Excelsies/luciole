#include "fs/subtree_measure.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <glib.h>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::Location;
  using luciole::MeasurePriority;
  using luciole::MeasureServiceOptions;
  using luciole::MeasureTable;
  using luciole::SubtreeMeasureService;

  struct Scratch : luciole::test::TempFolder {
    Scratch() : TempFolder("luciole-measure-service") {}
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

  void pumpFor(std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    pumpUntil([&] { return std::chrono::steady_clock::now() >= end; });
  }

  // `folders` folders, each with `files` files of 10 bytes.
  Location tree(const fs::path& root, int folders, int files) {
    for (int d = 0; d < folders; ++d) {
      const auto folder = root / std::format("d{:03}", d);
      fs::create_directories(folder);
      for (int f = 0; f < files; ++f) {
        std::ofstream(folder / std::format("f{}", f)) << "0123456789";
      }
    }
    return Location::fromPath(root.native());
  }

  MeasureServiceOptions fastOptions(std::size_t workers = 2) {
    MeasureServiceOptions options;
    options.publishInterval = std::chrono::milliseconds(1);
    options.workers = workers;
    return options;
  }

  void progressive(const Location& big) {
    SubtreeMeasureService service(fastOptions());
    int changes = 0;
    service.setChangedCallback([&] { ++changes; });
    const auto handle = service.request(big, MeasurePriority::Current);
    TEST_CHECK(handle.valid() && !handle.done() && handle.table() == nullptr);
    std::set<const MeasureTable*> seen;
    std::vector<std::shared_ptr<const MeasureTable>> kept;
    pumpUntil([&] {
      if (const auto table = handle.table(); table && seen.insert(table.get()).second) {
        kept.push_back(table);
      }
      return handle.done();
    });
    const auto final = handle.table();
    TEST_CHECK(final != nullptr && !final->partial && !final->truncated && !final->error);
    TEST_CHECK(final->records[0].totals.files == 10000 && final->records[0].totals.apparentBytes == 100000);
    TEST_CHECK(changes >= 2 && kept.size() >= 2);
    for (std::size_t i = 0; i + 1 < kept.size(); ++i) {
      TEST_CHECK(kept[i]->partial);
      TEST_CHECK(kept[i]->records[0].totals.files <= kept[i + 1]->records[0].totals.files);
    }
    TEST_CHECK(service.status() == luciole::MeasureServiceStatus{});
  }

  void sharedWalk(const Location& big) {
    SubtreeMeasureService service(fastOptions());
    const auto first = service.request(big, MeasurePriority::Ancestor);
    const auto second = service.request(big, MeasurePriority::Current);
    // One snapshot, since a worker may take the job between two reads; the walk may also be over already.
    const auto status = service.status();
    TEST_CHECK(status.queued + status.running <= 1);
    pumpUntil([&] {
      TEST_CHECK(first.table() == second.table());
      return first.done();
    });
    TEST_CHECK(second.done() && first.table() == second.table() && !first.table()->truncated);

    // A finished walk is not shared: asking again walks again.
    const auto again = service.request(big, MeasurePriority::Current);
    TEST_CHECK(!again.done());
    pumpUntil([&] { return again.done(); });
    TEST_CHECK(again.table() != first.table() && again.table()->records[0].totals == first.table()->records[0].totals);
  }

  void dropCancels(const Location& big, const Location& other) {
    SubtreeMeasureService service(fastOptions(1));
    int changes = 0;
    service.setChangedCallback([&] { ++changes; });
    auto running = service.request(big, MeasurePriority::Current);
    pumpUntil([&] { return service.status().running == 1; });
    auto queued = service.request(other, MeasurePriority::Current);
    TEST_CHECK(service.status().queued == 1);
    queued.reset();
    TEST_CHECK(service.status().queued == 0 && !queued.valid());

    // Two handles: the walk lives until the last one goes.
    auto copy = service.request(big, MeasurePriority::Jump);
    running.reset();
    TEST_CHECK(service.status().running == 1);
    copy = luciole::MeasureHandle();
    pumpUntil([&] { return service.status().running == 0; });
    const auto before = changes;
    pumpFor(std::chrono::milliseconds(30));
    TEST_CHECK(changes == before);
  }

  // The ancestor walk is long and the ahead walk short, so finishing order shows which ran first.
  void priorityOrder(const Location& big, const Location& other, const Location& tiny) {
    SubtreeMeasureService service(fastOptions(1));
    const auto first = service.request(big, MeasurePriority::Current);
    pumpUntil([&] { return service.status().running == 1; });
    const auto ancestor = service.request(other, MeasurePriority::Ancestor);
    const auto ahead = service.request(tiny, MeasurePriority::Ahead);
    TEST_CHECK(service.status().queued == 2);
    // The publication at which each walk was first seen done.
    int publications = 0;
    int aheadDone = 0;
    int ancestorDone = 0;
    service.setChangedCallback([&] {
      ++publications;
      aheadDone = aheadDone == 0 && ahead.done() ? publications : aheadDone;
      ancestorDone = ancestorDone == 0 && ancestor.done() ? publications : ancestorDone;
    });
    pumpUntil([&] { return ancestor.done() && ahead.done() && first.done(); });
    TEST_CHECK(aheadDone > 0 && aheadDone < ancestorDone);
  }

  void twoAtOnce(const std::vector<Location>& locations) {
    SubtreeMeasureService service(fastOptions(5));
    std::vector<luciole::MeasureHandle> handles;
    handles.reserve(locations.size());
    for (const auto& location : locations) {
      handles.push_back(service.request(location, MeasurePriority::Ahead));
    }
    std::size_t most = 0;
    pumpUntil([&] {
      most = std::max(most, service.status().running);
      TEST_CHECK(service.status().running <= 2);
      return std::ranges::all_of(handles, [](const auto& handle) { return handle.done(); });
    });
    TEST_CHECK(most == 2);
  }

  void ancestorBudget(const Location& big) {
    auto options = fastOptions();
    options.ancestorBudget = 100;
    SubtreeMeasureService service(options);
    const auto ancestor = service.request(big, MeasurePriority::Ancestor);
    pumpUntil([&] { return ancestor.done(); });
    TEST_CHECK(ancestor.table()->truncated && ancestor.table()->entries == 100);
    TEST_CHECK(ancestor.walkPriority() == MeasurePriority::Ancestor);
    const auto current = service.request(big, MeasurePriority::Current);
    pumpUntil([&] { return current.done(); });
    TEST_CHECK(!current.table()->truncated && current.walkPriority() == MeasurePriority::Current);
  }

  // An Ancestor walk raised to Current while queued runs, and reports, as Current.
  void raisedWhileQueued(const Location& big, const Location& small) {
    auto options = fastOptions(1);
    options.ancestorBudget = 10;
    SubtreeMeasureService service(options);
    const auto busy = service.request(big, MeasurePriority::Current);
    pumpUntil([&] { return service.status().running == 1; });
    const auto ancestor = service.request(small, MeasurePriority::Ancestor);
    const auto raised = service.request(small, MeasurePriority::Current);
    pumpUntil([&] { return raised.done(); });
    TEST_CHECK(raised.walkPriority() == MeasurePriority::Current && !raised.table()->truncated);
    TEST_CHECK(ancestor.walkPriority() == MeasurePriority::Current);
  }

  // Every record lists its children in range, and its bytes are the sum of its files, child records and remainders.
  void checkSums(const MeasureTable& table) {
    for (std::uint32_t i = 0; i < table.records.size(); ++i) {
      const auto& record = table.records[i];
      TEST_CHECK(i == 0 || record.parent < i);
      std::uint64_t bytes = record.otherFiles.totals.apparentBytes + record.otherDirectories.totals.apparentBytes;
      for (const auto& file : record.files ? *record.files : std::vector<luciole::MeasureFile>{}) {
        bytes += file.apparentBytes;
      }
      for (std::uint32_t c = record.firstChild; c < record.firstChild + record.childCount; ++c) {
        TEST_CHECK(c < table.records.size() && table.records[c].parent == i);
        bytes += table.records[c].totals.apparentBytes;
      }
      TEST_CHECK(bytes == record.totals.apparentBytes);
    }
  }

  // Groups fold their children when they finish, while later copies of the table are still handed out.
  void foldingPartials(const fs::path& base) {
    for (int g = 0; g < 4; ++g) {
      tree(base / "folding" / std::format("g{}", g), 6, 200);
    }
    auto options = fastOptions();
    options.walk.childCap = 2;
    SubtreeMeasureService service(options);
    const auto handle = service.request(Location::fromPath((base / "folding").native()), MeasurePriority::Current);
    std::uint64_t last = 0;
    int folded = 0;
    pumpUntil([&] {
      if (const auto table = handle.table()) {
        checkSums(*table);
        TEST_CHECK(table->records[0].totals.apparentBytes >= last);
        last = table->records[0].totals.apparentBytes;
        folded += table->partial && std::ranges::any_of(table->records, [](const auto& record) {
                    return record.otherDirectories.count > 0;
                  });
      }
      return handle.done();
    });
    TEST_CHECK(folded > 0 && last == 4 * 6 * 200 * 10);
    const auto& final = *handle.table();
    TEST_CHECK(final.records[0].childCount == 2 && final.records[0].otherDirectories.count == 2);
    TEST_CHECK(final.records.size() == 1 + 2 + 2 * 2);
  }

  void failures(const fs::path& base) {
    SubtreeMeasureService service(fastOptions());
    const auto missing = service.request(Location::fromPath((base / "missing").native()), MeasurePriority::Current);
    const auto trash = service.request(Location::trash(), MeasurePriority::Current);
    pumpUntil([&] { return missing.done() && trash.done(); });
    TEST_CHECK(missing.table()->error && missing.table()->records.empty());
    TEST_CHECK(trash.table()->error && trash.table()->error->kind == luciole::FileErrorKind::Unsupported);
  }

  void handleOutlivesService(const Location& big) {
    luciole::MeasureHandle handle;
    {
      SubtreeMeasureService service(fastOptions());
      handle = service.request(big, MeasurePriority::Current);
      pumpUntil([&] { return handle.table() != nullptr; });
    }
    TEST_CHECK(handle.valid() && handle.table() != nullptr && !handle.done());
    handle.reset();
    TEST_CHECK(!handle.valid() && handle.table() == nullptr);
  }
} // namespace

int main() {
  const Scratch scratch;
  const auto big = tree(scratch.path() / "big", 100, 100);
  const auto other = tree(scratch.path() / "other", 100, 100);
  const auto small = tree(scratch.path() / "small", 4, 4);
  const auto tiny = tree(scratch.path() / "tiny", 1, 1);
  progressive(big);
  sharedWalk(big);
  dropCancels(big, other);
  priorityOrder(big, other, tiny);
  twoAtOnce({big, other, small, tiny});
  ancestorBudget(big);
  raisedWhileQueued(big, small);
  foldingPartials(scratch.path());
  failures(scratch.path());
  handleOutlivesService(big);
  return 0;
}
