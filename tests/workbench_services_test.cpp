#include "browser/workbench_services.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "fs/location_within.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <poll.h>
#include <print>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::Location;
  using luciole::MeasureCache;
  using luciole::MeasurePriority;
  using luciole::SubtreeMeasureService;
  using luciole::WorkbenchServices;
  using luciole::WorkbenchWalks;
  using Want = WorkbenchWalks::Want;

  class Fixture {
  public:
    luciole::test::TempFolder tempFolder{"luciole-workbench-services"};
    fs::path path = tempFolder.path();
  };

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
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

  private:
    GlibPollSource m_source;
  };

  // `folders` folders of `files` files each.
  Location tree(const fs::path& root, int folders, int files) {
    for (int d = 0; d < folders; ++d) {
      const auto folder = root / std::format("d{:02}", d);
      fs::create_directories(folder);
      for (int f = 0; f < files; ++f) {
        std::ofstream(folder / std::format("f{}", f)) << "0123456789";
      }
    }
    return Location::fromPath(root.native());
  }

  // Waits for the service's threads to end every walk, without the main loop, so the cache has yet to take the
  // results; returns the most walks seen running at once. Bounded by the number of reads.
  std::size_t threadsDone(const SubtreeMeasureService& service) {
    std::size_t most = 0;
    for (int round = 0; round < 10'000'000; ++round) {
      const auto status = service.status();
      most = std::max(most, status.running);
      if (status.queued + status.running == 0) {
        break;
      }
      std::this_thread::yield();
    }
    return most;
  }

  bool idle(const SubtreeMeasureService& service) {
    const auto status = service.status();
    return status.queued + status.running == 0;
  }

  luciole::MeasureServiceOptions quickPublication(std::size_t workers = 2) {
    luciole::MeasureServiceOptions options;
    options.publishInterval = std::chrono::milliseconds(1);
    options.workers = workers;
    return options;
  }

  // Updates the walks whenever the cache notifies, as a page's model does.
  class Driven {
  public:
    explicit Driven(MeasureCache& cache) : walks(cache, [this] { walks.update(); }), m_cache(cache) {
      m_listener = cache.addListener([this] { walks.update(); });
    }
    ~Driven() { m_cache.removeListener(m_listener); }
    Driven(const Driven&) = delete;
    Driven& operator=(const Driven&) = delete;
    void enter(const Location& folder, std::span<const Location> ancestors = {}) {
      walks.enter(folder, ancestors);
      walks.update();
    }
    // Walks queued or running for this page.
    std::size_t walking() const {
      return static_cast<std::size_t>(std::ranges::count_if(walks.walks(), [](const WorkbenchWalks::Walk& walk) {
        return walk.started && !walk.finished;
      }));
    }
    WorkbenchWalks walks;

  private:
    MeasureCache& m_cache;
    std::uint64_t m_listener = 0;
  };

  // Counts the complete tables of one folder the cache holds, as it notifies.
  class Tables {
  public:
    Tables(MeasureCache& cache, Location folder) : m_cache(cache), m_folder(std::move(folder)) {
      m_listener = cache.addListener([this] {
        const auto found = m_cache.find(m_folder);
        if (found.record == 0 && !found.table->partial && !std::ranges::contains(m_tables, found.table)) {
          m_tables.push_back(found.table);
        }
      });
    }
    ~Tables() { m_cache.removeListener(m_listener); }
    Tables(const Tables&) = delete;
    Tables& operator=(const Tables&) = delete;
    std::size_t complete() const { return m_tables.size(); }

  private:
    MeasureCache& m_cache;
    Location m_folder;
    std::uint64_t m_listener = 0;
    std::vector<std::shared_ptr<const luciole::MeasureTable>> m_tables;
  };

  // The cache serves what the service walks, and the service runs two walks at once, never more.
  void measuresThroughTheCache(const fs::path& base) {
    WorkbenchServices services;
    Pump pump;
    std::vector<Location> folders;
    folders.reserve(6);
    for (int i = 0; i < 6; ++i) {
      folders.push_back(tree(base / std::format("root{}", i), 30, 30));
    }
    for (const auto& folder : folders) {
      TEST_CHECK(services.measureCache().measure(folder, MeasurePriority::Current).measuring);
    }
    const auto most = threadsDone(services.measureService());
    pump.until([&] {
      return std::ranges::none_of(folders, [&](const Location& folder) {
        return services.measureCache().measure(folder, MeasurePriority::Current).measuring;
      });
    });
    for (const auto& folder : folders) {
      const auto found = services.measureCache().find(folder);
      TEST_CHECK(found.entry() != nullptr && found.entry()->totals.files == 900);
    }
    TEST_CHECK(services.measureCache().tables() == folders.size() && most == 2);
  }

  // Destroying the services while a walk is queued or running cancels it and returns.
  void destroyedWhileWalking(const fs::path& base) {
    const auto folder = tree(base / "busy", 60, 60);
    Pump pump;
    {
      WorkbenchServices services;
      TEST_CHECK(services.measureCache().measure(folder, MeasurePriority::Current).measuring);
      const auto status = services.measureService().status();
      TEST_CHECK(status.queued + status.running == 1);
    }
    pump.once();
  }

  // Holds a walk after its first partial table, so the walk certainly reads while the test looks.
  struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool reached = false;
    bool open = false;
    void hold() {
      std::unique_lock lock(mutex);
      if (std::exchange(reached, true)) {
        return;
      }
      changed.notify_all();
      TEST_CHECK(changed.wait_for(lock, std::chrono::seconds(20), [this] { return open; }));
    }
    // Waits until a walk is held.
    void awaitReached() {
      std::unique_lock lock(mutex);
      TEST_CHECK(changed.wait_for(lock, std::chrono::seconds(20), [this] { return reached; }));
    }
    void release() {
      const std::scoped_lock lock(mutex);
      open = true;
      changed.notify_all();
    }
  };

  // The descriptors of this process that name `root` or a file below it.
  std::size_t held(const fs::path& root) {
    std::size_t count = 0;
    for (const auto& file : fs::directory_iterator("/proc/self/fd")) {
      std::error_code error;
      const auto target = fs::read_symlink(file.path(), error).native();
      count += !error && (target == root.native() || target.starts_with(root.native() + "/"));
    }
    return count;
  }

  // A drive removed under a Workbench page: its walks leave the page, and the service reports the cancelled walk as
  // reading until its thread has let go of the folders it holds, then nothing holds the drive.
  void releasingTheDriveEndsItsWalks(const fs::path& base) {
    const auto volume = tree(base / "volume", 6, 20);
    const auto other = tree(base / "elsewhere", 1, 1);
    Pump pump;
    Gate gate;
    auto options = quickPublication(1);
    options.publishInterval = std::chrono::milliseconds(0); // every finished folder publishes
    options.afterPartial = [&gate] { gate.hold(); };
    SubtreeMeasureService service(options);
    MeasureCache cache(service);
    Driven page(cache);
    page.enter(volume);
    gate.awaitReached();
    TEST_CHECK(service.reading(volume));
    TEST_CHECK(held(base / "volume") > 0);
    TEST_CHECK(!service.reading(other));
    page.walks.release(other);
    TEST_CHECK(page.walks.walks().size() == 1);
    page.walks.release(volume);
    TEST_CHECK(page.walks.walks().empty());
    TEST_CHECK(service.reading(volume) && held(base / "volume") > 0); // the cancelled walk has not let go yet
    gate.release();
    pump.until([&] { return !service.reading(volume); });
    TEST_CHECK(held(base / "volume") == 0);
  }

  // Two pages on one folder share one walk.
  void sharedWalk(const fs::path& base) {
    const auto folder = tree(base / "shared", 10, 10);
    WorkbenchServices services;
    Driven first(services.measureCache());
    Driven second(services.measureCache());
    const Tables tables(services.measureCache(), folder);
    Pump pump;
    first.enter(folder);
    second.enter(folder);
    // The walk may already have ended on its thread; a second walk would show here or as a second table below.
    const auto status = services.measureService().status();
    TEST_CHECK(status.queued + status.running <= 1 && services.measureCache().tables() == 1);
    pump.until([&] { return first.walks.folderFinished() && second.walks.folderFinished(); });
    TEST_CHECK(first.walks.folder()->found.entry()->totals.files == 100);
    TEST_CHECK(second.walks.folder()->found.table == first.walks.folder()->found.table);
    TEST_CHECK(tables.complete() == 1);
  }

  // Leaving a folder drops its walks: started ones are cancelled and leave no table. Jump walks start once the
  // folder's walk ended, at most `limit` of them.
  void leftWalksEnd(const fs::path& base) {
    const auto away = tree(base / "away", 8, 2);
    const auto next = tree(base / "next", 1, 1);
    std::vector<Location> jumps;
    jumps.reserve(8);
    for (int d = 0; d < 8; ++d) {
      jumps.push_back(away.child(std::format("d{:02}", d)));
    }
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    Driven page(cache);
    Pump pump;
    page.enter(away);
    page.walks.planJumps(jumps, 6);
    page.walks.update();
    TEST_CHECK(std::ranges::count(page.walks.walks(), Want::Jump, &WorkbenchWalks::Walk::want) == 6);
    TEST_CHECK(std::ranges::count_if(page.walks.walks(), &WorkbenchWalks::Walk::started) == 1);
    pump.until([&] { return page.walks.folderFinished(); });
    page.walks.update();
    TEST_CHECK(std::ranges::count_if(page.walks.walks(), &WorkbenchWalks::Walk::started) == 1 + 2);
    // The Jump walks may have ended on their threads, but the cache has not taken their tables yet.
    page.enter(next);
    TEST_CHECK(page.walking() == 1 && cache.tables() == 2);
    pump.until([&] { return page.walks.folderFinished() && idle(service); });
    TEST_CHECK(cache.tables() == 2 && !page.walks.waiting());
  }

  // Walking through many folders quickly keeps at most kWalks walks queued, and the page at most kWalks of its own.
  void sweep(const fs::path& base) {
    std::vector<Location> folders;
    folders.reserve(50);
    for (int i = 0; i < 50; ++i) {
      folders.push_back(tree(base / "sweep" / std::format("s{:02}", i), 1, 1));
    }
    WorkbenchServices services;
    Driven page(services.measureCache());
    Pump pump;
    for (const auto& folder : folders) {
      page.enter(folder);
      const auto status = services.measureService().status();
      TEST_CHECK(status.queued <= WorkbenchWalks::kWalks && status.running <= WorkbenchWalks::kWalks);
      TEST_CHECK(page.walking() <= WorkbenchWalks::kWalks);
    }
    pump.until([&] {
      const auto status = services.measureService().status();
      TEST_CHECK(status.queued <= WorkbenchWalks::kWalks && status.running <= WorkbenchWalks::kWalks);
      return page.walks.folderFinished() && idle(services.measureService());
    });
    TEST_CHECK(page.walks.folder()->location == folders.back() && services.measureCache().tables() == 1);
  }

  // A walk that ends stale is not walked again: the new folder's, whose child changed while it ran. The walk of the
  // folder left meanwhile is cancelled, though a change below it arrived.
  void staleWalksEndOnce(const fs::path& base) {
    const auto left = tree(base / "left", 4, 4);
    const auto folder = tree(base / "left" / "d00", 2, 2);
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    const Tables leftTables(cache, left);
    const Tables folderTables(cache, folder);
    Driven page(cache);
    Pump pump;
    page.enter(left);
    page.enter(folder);
    page.walks.refreshChild("d01");
    page.walks.update();
    pump.until([&] { return !page.walks.waiting() && idle(service); });
    for (int i = 0; i < 20; ++i) {
      pump.once();
    }
    TEST_CHECK(leftTables.complete() == 0 && folderTables.complete() == 1 && idle(service));
  }

  // Waits for the service's threads, without the main loop, until nothing is queued; bounded by the number of reads.
  void taken(const SubtreeMeasureService& service) {
    for (int round = 0; round < 10'000'000 && service.status().queued > 0; ++round) {
      std::this_thread::yield();
    }
    TEST_CHECK(service.status().queued == 0);
  }

  // Leaving a folder cancels its walk, so the next folder's walk runs on the one worker at once instead of after it.
  void leavingEndsTheWalk(const fs::path& base) {
    const auto big = tree(base / "big", 100, 100);
    const auto small = tree(base / "small", 2, 2);
    double fullWalk = 0;
    {
      SubtreeMeasureService service(quickPublication(1));
      MeasureCache cache(service);
      Driven page(cache);
      Pump pump;
      const auto start = std::clock();
      page.enter(big);
      pump.until([&] { return page.walks.folderFinished() && idle(service); });
      fullWalk = static_cast<double>(std::clock() - start);
    }
    SubtreeMeasureService service(quickPublication(1));
    MeasureCache cache(service);
    const Tables bigTables(cache, big);
    Driven page(cache);
    Pump pump;
    page.enter(big);
    taken(service);
    const auto start = std::clock();
    page.enter(small);
    TEST_CHECK(page.walking() == 1 && cache.tables() == 1);
    pump.until([&] { return page.walks.folderFinished() && idle(service); });
    const auto spent = static_cast<double>(std::clock() - start);
    TEST_CHECK(bigTables.complete() == 0 && cache.find(big).entry() == nullptr);
    TEST_CHECK(page.walks.folder()->found.entry()->totals.files == 4);
    TEST_CHECK(spent < fullWalk / 2);
    std::println(
        "leaving a folder: the next folder's walk ended after {:.1f} ms of processor time; a whole walk of the folder "
        "left takes {:.1f} ms",
        spent * 1000.0 / CLOCKS_PER_SEC, fullWalk * 1000.0 / CLOCKS_PER_SEC
    );
  }

  // A walk two pages wait for ends only when both have left; while one stays, it finishes for that one.
  void sharedWalkEndsWithItsLastPage(const fs::path& base) {
    const auto folder = tree(base / "both", 60, 60);
    const auto elsewhere = tree(base / "elsewhere", 1, 1);
    SubtreeMeasureService service(quickPublication(1));
    MeasureCache cache(service);
    const Tables tables(cache, folder);
    Driven first(cache);
    Driven second(cache);
    Pump pump;
    first.enter(folder);
    second.enter(folder);
    taken(service);
    first.enter(elsewhere);
    TEST_CHECK(cache.tables() == 2 && second.walking() == 1);
    second.enter(elsewhere);
    TEST_CHECK(cache.tables() == 1);
    pump.until([&] { return first.walks.folderFinished() && second.walks.folderFinished() && idle(service); });
    TEST_CHECK(tables.complete() == 0 && cache.find(folder).entry() == nullptr);

    first.enter(folder);
    second.enter(folder);
    taken(service);
    first.enter(elsewhere);
    pump.until([&] { return second.walks.folderFinished() && idle(service); });
    TEST_CHECK(tables.complete() == 1 && second.walks.folder()->found.entry()->totals.files == 3600);
  }

  // Entering a folder below one whose walk runs keeps that walk for the new folder's ancestor level.
  void ancestorKeepsTheWalk(const fs::path& base) {
    const auto parent = tree(base / "parent", 60, 60);
    const auto child = parent.child("d00");
    SubtreeMeasureService service(quickPublication(1));
    MeasureCache cache(service);
    const Tables tables(cache, parent);
    Driven page(cache);
    Pump pump;
    page.enter(parent);
    taken(service);
    const std::vector<Location> ancestors{parent};
    page.enter(child, ancestors);
    const auto& level = page.walks.walks()[1];
    TEST_CHECK(level.want == Want::Ancestor && level.started && !page.walks.jumpsPlanned());
    pump.until([&] { return page.walks.folderFinished() && idle(service); });
    TEST_CHECK(tables.complete() == 1);
  }

  // A child changed again while its walk runs is walked again once that walk ends.
  void walkedAgain(const fs::path& base) {
    const auto folder = tree(base / "again", 2, 1);
    const auto child = folder.child("d00");
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    Driven page(cache);
    Pump pump;
    page.enter(folder);
    pump.until([&] { return page.walks.folderFinished(); });
    page.walks.refreshChild("d00");
    page.walks.update();
    // The walk ends on its thread; the cache takes its table only on the main loop, after the change below.
    threadsDone(service);
    std::ofstream(base / "again" / "d00" / "late") << "late";
    page.walks.refreshChild("d00");
    pump.until([&] { return !page.walks.waiting() && idle(service); });
    TEST_CHECK(cache.find(child).record == 0 && cache.find(child).entry()->totals.apparentBytes == 14);
  }

  // Up to kRefreshedChildren children are walked again alone; one more walks the whole folder again.
  void manyRefreshes(const fs::path& base) {
    const auto folder = tree(base / "many", 20, 1);
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    Driven page(cache);
    Pump pump;
    page.enter(folder);
    pump.until([&] { return page.walks.folderFinished(); });
    const auto table = page.walks.folder()->found.table;
    for (std::size_t i = 0; i < WorkbenchWalks::kRefreshedChildren; ++i) {
      page.walks.refreshChild(std::format("d{:02}", i));
    }
    page.walks.update();
    pump.until([&] { return !page.walks.waiting() && idle(service); });
    TEST_CHECK(cache.find(folder).table == table);
    TEST_CHECK(std::ranges::count(page.walks.walks(), Want::Child, &WorkbenchWalks::Walk::want) == 16);
    page.walks.refreshChild(std::format("d{:02}", WorkbenchWalks::kRefreshedChildren));
    page.walks.update();
    TEST_CHECK(std::ranges::count(page.walks.walks(), Want::Child, &WorkbenchWalks::Walk::want) == 0);
    pump.until([&] { return !page.walks.waiting() && idle(service); });
    TEST_CHECK(cache.find(folder).table != table && cache.find(folder).entry()->totals.files == 20);
  }

  // Going up into an ancestor whose walk runs under the ancestor budget measures that folder in full.
  void upIntoTheAncestorWalk(const fs::path& base) {
    const auto parent = tree(base / "up", 30, 30);
    const auto child = parent.child("d00");
    auto options = quickPublication(1);
    options.ancestorBudget = 100;
    SubtreeMeasureService service(options);
    MeasureCache cache(service);
    Driven page(cache);
    Pump pump;
    const std::vector<Location> ancestors{parent};
    page.enter(child, ancestors);
    pump.until([&] { return page.walks.folderFinished(); });
    page.walks.planJumps({}, 0);
    page.walks.update();
    TEST_CHECK(page.walks.walks()[1].started && !page.walks.walks()[1].finished);
    page.enter(parent);
    TEST_CHECK(page.walks.folder()->started);
    pump.until([&] { return page.walks.folderFinished() && idle(service); });
    const auto& found = page.walks.folder()->found;
    TEST_CHECK(found.entry() != nullptr && found.entry()->totals.files == 900 && !found.table->truncated);
  }

  // A change of the folder while its walk runs still counts once that walk goes on as the next folder's ancestor:
  // the walk ends stale and is walked once more.
  void changeKeptWhenCarried(const fs::path& base) {
    const auto folder = tree(base / "carried", 20, 20);
    const auto child = folder.child("d00");
    SubtreeMeasureService service(quickPublication(1));
    MeasureCache cache(service);
    const Tables tables(cache, folder);
    Driven page(cache);
    Pump pump;
    page.enter(folder);
    page.walks.walkFolderAgain();
    const std::vector<Location> ancestors{folder};
    page.enter(child, ancestors);
    TEST_CHECK(page.walks.walks()[1].started);
    pump.until([&] { return page.walks.folderFinished(); });
    page.walks.planJumps({}, 0);
    page.walks.update();
    pump.until([&] { return page.walks.walks()[1].finished && idle(service) && !cache.find(folder).stale; });
    TEST_CHECK(tables.complete() == 2 && cache.find(folder).entry()->totals.files == 400);
  }
} // namespace

int main() {
  const Fixture fixture;
  measuresThroughTheCache(fixture.path);
  destroyedWhileWalking(fixture.path);
  sharedWalk(fixture.path);
  releasingTheDriveEndsItsWalks(fixture.path);
  leftWalksEnd(fixture.path);
  sweep(fixture.path);
  staleWalksEndOnce(fixture.path);
  leavingEndsTheWalk(fixture.path);
  sharedWalkEndsWithItsLastPage(fixture.path);
  ancestorKeepsTheWalk(fixture.path);
  walkedAgain(fixture.path);
  manyRefreshes(fixture.path);
  upIntoTheAncestorWalk(fixture.path);
  changeKeptWhenCarried(fixture.path);
  return 0;
}
