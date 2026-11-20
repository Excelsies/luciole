#include "browser/measure_cache.h"
#include "temp_folder.h"
#include "test_check.h"

#include <chrono>
#include <cstdlib>
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
  using luciole::MeasureCacheOptions;
  using luciole::MeasurePriority;
  using luciole::SubtreeMeasureService;

  struct Scratch : luciole::test::TempFolder {
    Scratch() : TempFolder("luciole-measure-cache") {}
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

  // `folders` folders, each with `files` files of 10 bytes: 1 + folders records at the default detail depth.
  Location tree(const fs::path& root, int folders, int files) {
    fs::create_directories(root);
    for (int d = 0; d < folders; ++d) {
      const auto folder = root / std::format("d{:03}", d);
      fs::create_directories(folder);
      for (int f = 0; f < files; ++f) {
        std::ofstream(folder / std::format("f{}", f)) << "0123456789";
      }
    }
    return Location::fromPath(root.native());
  }

  luciole::MeasureServiceOptions serviceOptions() {
    luciole::MeasureServiceOptions options;
    options.publishInterval = std::chrono::milliseconds(1);
    return options;
  }

  std::uint64_t files(const luciole::MeasureFind& found) {
    TEST_CHECK(found.entry() != nullptr);
    return found.entry()->totals.files;
  }

  // Measures until a complete table is served.
  luciole::MeasureFind
  settle(MeasureCache& cache, const Location& folder, MeasurePriority priority = MeasurePriority::Current) {
    luciole::MeasureFind found;
    pumpUntil([&] {
      found = cache.measure(folder, priority);
      return found.entry() != nullptr && !found.measuring && !found.stale;
    });
    return found;
  }

  void missThenServe(const fs::path& base) {
    SubtreeMeasureService service(serviceOptions());
    MeasureCache cache(
        service, {.recordBudget = 1000, .checkAge = std::chrono::hours(1), .maxAge = std::chrono::hours(1)}
    );
    int first = 0;
    int second = 0;
    const auto a = cache.addListener([&] { ++first; });
    const auto b = cache.addListener([&] { ++second; });
    const auto folder = tree(base / "serve", 20, 50);
    const auto missing = cache.measure(folder, MeasurePriority::Current);
    TEST_CHECK(missing.entry() == nullptr && missing.measuring && !missing.stale);
    const auto found = settle(cache, folder);
    TEST_CHECK(files(found) == 1000 && !found.table->partial && found.record == 0);
    TEST_CHECK(first > 0 && first == second);
    TEST_CHECK(cache.tables() == 1 && cache.records() == 21);

    // A fresh table is served without a new walk.
    const auto again = cache.measure(folder, MeasurePriority::Current);
    TEST_CHECK(again.table == found.table && !again.measuring && service.status().queued == 0);

    // A folder inside a cached table is found without its own walk.
    const auto inside = cache.find(Location::fromPath((base / "serve" / "d007").native()));
    TEST_CHECK(inside.table == found.table && inside.entry()->name == "d007" && files(inside) == 50);
    TEST_CHECK(cache.find(Location::fromPath((base / "elsewhere").native())).entry() == nullptr);

    cache.removeListener(a);
    const int before = second;
    cache.invalidate(folder);
    TEST_CHECK(second == before + 1 && first == before);
    cache.removeListener(b);
  }

  void staleThenFresh(const fs::path& base) {
    SubtreeMeasureService service(serviceOptions());
    MeasureCache cache(
        service, {.recordBudget = 1000, .checkAge = std::chrono::milliseconds(0), .maxAge = std::chrono::hours(1)}
    );
    const auto folder = tree(base / "fresh", 20, 100);
    const auto measured = settle(cache, folder);
    TEST_CHECK(files(measured) == 2000);

    // An unchanged stamp keeps the table without walking.
    pumpFor(std::chrono::milliseconds(20));
    const auto checked = cache.measure(folder, MeasurePriority::Current);
    pumpFor(std::chrono::milliseconds(50));
    const auto unchanged = cache.measure(folder, MeasurePriority::Current);
    TEST_CHECK(checked.table == measured.table && unchanged.table == measured.table && !unchanged.stale);

    // A file added to the folder changes its stamp: the old table is served stale while the new walk runs.
    std::ofstream(base / "fresh" / "added") << "x";
    bool sawStale = false;
    luciole::MeasureFind current;
    pumpUntil([&] {
      current = cache.measure(folder, MeasurePriority::Current);
      if (current.stale) {
        sawStale = true;
        TEST_CHECK(files(current) == 2000);
      }
      return files(current) == 2001;
    });
    TEST_CHECK(sawStale && !current.stale && !current.measuring);

    // invalidate() of a folder inside marks the cached ancestor stale.
    std::ofstream(base / "fresh" / "d001" / "deeper") << "y";
    cache.invalidate(Location::fromPath((base / "fresh" / "d001").native()));
    TEST_CHECK(cache.find(folder).stale && files(cache.find(folder)) == 2001);
    const auto refreshed = settle(cache, folder);
    TEST_CHECK(files(refreshed) == 2002);
  }

  void eviction(const fs::path& base) {
    SubtreeMeasureService service(serviceOptions());
    MeasureCache cache(
        service, {.recordBudget = 12, .checkAge = std::chrono::hours(1), .maxAge = std::chrono::hours(1)}
    );
    const auto first = tree(base / "first", 4, 1);
    const auto second = tree(base / "second", 4, 1);
    const auto third = tree(base / "third", 4, 1);
    settle(cache, first);
    settle(cache, second);
    TEST_CHECK(cache.tables() == 2 && cache.records() == 10);
    cache.find(first);
    settle(cache, third);
    TEST_CHECK(cache.tables() == 2 && cache.records() == 10);
    TEST_CHECK(cache.find(second).entry() == nullptr);
    TEST_CHECK(cache.find(first).entry() != nullptr && cache.find(third).entry() != nullptr);

    // One table larger than the budget is kept while it is the most recent.
    const auto large = tree(base / "large", 20, 1);
    settle(cache, large);
    TEST_CHECK(cache.tables() == 1 && cache.records() == 21);
  }

  // A queued Ancestor walk raised to Current runs without the ancestor budget.
  void raisedPriority(const fs::path& base) {
    auto options = serviceOptions();
    options.workers = 1;
    options.ancestorBudget = 10;
    SubtreeMeasureService service(options);
    MeasureCache cache(
        service, {.recordBudget = 1000, .checkAge = std::chrono::hours(1), .maxAge = std::chrono::hours(1)}
    );
    const auto busy = tree(base / "busy", 100, 100);
    const auto folder = tree(base / "raised", 5, 5);
    TEST_CHECK(cache.measure(busy, MeasurePriority::Current).measuring);
    pumpUntil([&] { return service.status().running == 1; });
    TEST_CHECK(cache.measure(folder, MeasurePriority::Ancestor).measuring);
    TEST_CHECK(cache.measure(folder, MeasurePriority::Current).measuring);
    TEST_CHECK(service.status().queued == 1);
    const auto found = settle(cache, folder);
    TEST_CHECK(!found.table->truncated && files(found) == 25);
  }

  // A table an Ancestor walk truncated is walked again, without that budget, for a more urgent request.
  void truncatedAncestor(const fs::path& base) {
    auto options = serviceOptions();
    options.ancestorBudget = 50;
    SubtreeMeasureService service(options);
    MeasureCache cache(
        service, {.recordBudget = 1000, .checkAge = std::chrono::hours(1), .maxAge = std::chrono::hours(1)}
    );
    const auto folder = tree(base / "ancestor", 4, 100);
    const auto ancestor = settle(cache, folder, MeasurePriority::Ancestor);
    TEST_CHECK(ancestor.table->truncated && files(ancestor) < 400);
    TEST_CHECK(cache.measure(folder, MeasurePriority::Ancestor).table == ancestor.table);
    const auto rising = cache.measure(folder, MeasurePriority::Current);
    TEST_CHECK(rising.measuring && rising.stale && rising.table == ancestor.table);
    const auto found = settle(cache, folder);
    TEST_CHECK(!found.table->truncated && files(found) == 400);
    TEST_CHECK(!cache.measure(folder, MeasurePriority::Current).measuring);
  }

  // invalidate() while a walk runs: the walk's table arrives stale and the next request walks again.
  void changedDuringWalk(const fs::path& base) {
    SubtreeMeasureService service(serviceOptions());
    MeasureCache cache(
        service, {.recordBudget = 1000, .checkAge = std::chrono::hours(1), .maxAge = std::chrono::hours(1)}
    );
    const auto folder = tree(base / "during", 4, 4);
    TEST_CHECK(cache.measure(folder, MeasurePriority::Current).measuring);
    cache.invalidate(folder);
    pumpUntil([&] { return cache.find(folder).entry() != nullptr && !cache.find(folder).measuring; });
    const auto delivered = cache.find(folder);
    TEST_CHECK(delivered.entry() != nullptr && !delivered.table->partial && delivered.stale);
    const auto again = cache.measure(folder, MeasurePriority::Current);
    TEST_CHECK(again.measuring && again.table == delivered.table);
    const auto fresh = settle(cache, folder);
    TEST_CHECK(fresh.table != delivered.table && files(fresh) == 16);
  }

  // A table older than maxAge is walked again even though its folder's stamp is unchanged.
  void agedOut(const fs::path& base) {
    SubtreeMeasureService service(serviceOptions());
    MeasureCache cache(
        service, {.recordBudget = 1000, .checkAge = std::chrono::hours(1), .maxAge = std::chrono::milliseconds(50)}
    );
    const auto folder = tree(base / "aged", 2, 2);
    const auto first = settle(cache, folder);
    pumpFor(std::chrono::milliseconds(100));
    const auto aged = cache.measure(folder, MeasurePriority::Current);
    TEST_CHECK(aged.measuring && aged.stale && aged.table == first.table);
    pumpUntil([&] { return !cache.find(folder).measuring; });
    TEST_CHECK(cache.find(folder).table != first.table && files(cache.find(folder)) == 4);
  }

  // A view keeps the tables of its moment and finds folders as the cache does, on any thread.
  void viewIsASnapshot(const fs::path& base) {
    SubtreeMeasureService service(serviceOptions());
    MeasureCache cache(
        service, {.recordBudget = 1000, .checkAge = std::chrono::hours(1), .maxAge = std::chrono::hours(1)}
    );
    const auto folder = tree(base / "view", 4, 2);
    const auto before = cache.view();
    const auto measured = settle(cache, folder);
    const auto view = cache.view();
    TEST_CHECK(view != before && view == cache.view());
    luciole::MeasureFind own;
    luciole::MeasureFind inside;
    luciole::MeasureFind elsewhere;
    std::thread([&] {
      own = view->find(folder);
      inside = view->find(folder.child("d002"));
      elsewhere = view->find(Location::fromPath((base / "elsewhere").native()));
    }).join();
    TEST_CHECK(own.table == measured.table && own.record == 0 && !own.stale && !own.measuring);
    TEST_CHECK(inside.table == measured.table && inside.entry()->name == "d002" && files(inside) == 2);
    TEST_CHECK(elsewhere.entry() == nullptr && before->find(folder).entry() == nullptr);

    // The cache moves on; the view does not.
    cache.invalidate(folder);
    TEST_CHECK(!view->find(folder).stale && cache.view()->find(folder).stale);
    TEST_CHECK(cache.measure(folder, MeasurePriority::Current).measuring && cache.view()->find(folder).measuring);
    TEST_CHECK(!view->find(folder).measuring);
  }

  // Waits for the service's threads, without the main loop, until `done`; bounded by the number of reads.
  void
  threadsUntil(const SubtreeMeasureService& service, const std::function<bool(luciole::MeasureServiceStatus)>& done) {
    for (int round = 0; round < 10'000'000 && !done(service.status()); ++round) {
      std::this_thread::yield();
    }
    TEST_CHECK(done(service.status()));
  }

  // Dropping the last lease cancels the folder's walk and forgets a folder that has no complete table; another
  // lease keeps it going, and a walk asked for without a lease runs to its end.
  void leases(const fs::path& base) {
    auto options = serviceOptions();
    options.workers = 1;
    SubtreeMeasureService service(options);
    MeasureCache cache(
        service, {.recordBudget = 100'000, .checkAge = std::chrono::hours(1), .maxAge = std::chrono::hours(1)}
    );
    const auto folder = tree(base / "leased", 60, 60);
    auto first = cache.lease(folder);
    auto second = cache.lease(folder);
    TEST_CHECK(first.valid() && cache.measure(folder, MeasurePriority::Current).measuring);
    threadsUntil(service, [](auto status) { return status.queued == 0; });
    first.reset();
    TEST_CHECK(!first.valid() && cache.view()->find(folder).entry() == nullptr && cache.tables() == 1);
    second.reset();
    TEST_CHECK(cache.tables() == 0 && service.status().queued == 0);
    threadsUntil(service, [](auto status) { return status.running == 0; });
    pumpFor(std::chrono::milliseconds(20));
    TEST_CHECK(cache.tables() == 0 && cache.find(folder).entry() == nullptr);

    // A walk queued behind another leaves the queue at once.
    const auto busy = tree(base / "unleased", 60, 60);
    TEST_CHECK(cache.measure(busy, MeasurePriority::Current).measuring);
    auto queued = cache.lease(folder);
    TEST_CHECK(cache.measure(folder, MeasurePriority::Current).measuring);
    auto moved = std::move(queued);
    TEST_CHECK(moved.valid() && cache.tables() == 2);
    moved = {};
    // Only the busy walk is left, queued or running on the one worker.
    TEST_CHECK(service.status().queued + service.status().running <= 1 && cache.tables() == 1);

    // Without a lease, the walk ends with its table.
    TEST_CHECK(files(settle(cache, busy)) == 3600);

    // A complete table outlives the lease of its refresh, flagged stale until it is walked again.
    {
      const auto held = cache.lease(busy);
      cache.invalidate(busy);
      TEST_CHECK(cache.measure(busy, MeasurePriority::Current).measuring);
    }
    const auto kept = cache.find(busy);
    TEST_CHECK(files(kept) == 3600 && kept.stale && !kept.measuring && cache.tables() == 1);
  }

  // Tables without records, such as the error tables of missing folders, still count toward the budget.
  void emptyTablesEvicted(const fs::path& base) {
    SubtreeMeasureService service(serviceOptions());
    MeasureCache cache(
        service, {.recordBudget = 3, .checkAge = std::chrono::hours(1), .maxAge = std::chrono::hours(1)}
    );
    for (int i = 0; i < 6; ++i) {
      const auto missing = Location::fromPath((base / std::format("missing{}", i)).native());
      pumpUntil([&] { return !cache.measure(missing, MeasurePriority::Current).measuring; });
    }
    TEST_CHECK(cache.tables() == 3 && cache.records() == 3);
  }
} // namespace

int main() {
  const Scratch scratch;
  missThenServe(scratch.path());
  staleThenFresh(scratch.path());
  eviction(scratch.path());
  raisedPriority(scratch.path());
  truncatedAncestor(scratch.path());
  changedDuringWalk(scratch.path());
  agedOut(scratch.path());
  emptyTablesEvicted(scratch.path());
  viewIsASnapshot(scratch.path());
  leases(scratch.path());
  return 0;
}
