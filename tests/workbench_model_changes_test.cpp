#include "browser/workbench_model.h"
#include "config/visit_service.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <poll.h>
#include <print>
#include <span>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::Location;
  using luciole::MeasureCache;
  using luciole::SubtreeMeasureService;
  using luciole::WorkbenchModel;
  using luciole::WorkbenchModelOptions;
  using luciole::workbench::kNoNode;
  using luciole::workbench::NodeId;
  using luciole::workbench::Tree;

  class Fixture {
  public:
    luciole::test::TempFolder tempFolder{"luciole-workbench-changes"};
    fs::path path = tempFolder.path();
  };

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        once();
      }
    }
    void forMilliseconds(int milliseconds) {
      const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
      until([&] { return std::chrono::steady_clock::now() >= end; });
    }
    void once() {
      std::vector<pollfd> fds;
      const auto start = m_source.addPollFds(fds);
      const int requested = m_source.pollTimeoutMs();
      TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 5 : std::min(requested, 5)) >= 0);
      m_source.dispatch(fds, start);
    }

  private:
    GlibPollSource m_source;
  };

  Location at(const fs::path& path) { return Location::fromPath(path.native()); }

  void writeFile(const fs::path& path, std::size_t bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << std::string(bytes, 'x');
  }

  luciole::MeasureServiceOptions quickPublication() {
    luciole::MeasureServiceOptions options;
    options.publishInterval = std::chrono::milliseconds(1);
    return options;
  }

  bool settled(const WorkbenchModel& model) { return model.snapshot() && !model.measuring(); }

  bool idle(const SubtreeMeasureService& service) {
    const auto status = service.status();
    return status.queued + status.running == 0;
  }

  NodeId named(const luciole::WorkbenchSnapshot& snapshot, const std::string& name) {
    for (const auto id : luciole::workbench::children(snapshot.tree, snapshot.tree.root)) {
      if (snapshot.tree.nodes[id].name == name) {
        return id;
      }
    }
    return kNoNode;
  }

  double rootBytes(const WorkbenchModel& model) {
    const auto& tree = model.snapshot()->tree;
    return tree.nodes[tree.root].bytes;
  }

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

  // Entries that appear, change or go in the folder reach the picture without anyone telling the model.
  void changesOnDisk(const fs::path& base) {
    const auto root = base / "disk";
    writeFile(root / "kept" / "data", 100);
    writeFile(root / "gone" / "data", 40);
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    WorkbenchModel model(cache, {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(at(root));
    pump.until([&] { return settled(model); });
    TEST_CHECK(rootBytes(model) == 140);

    writeFile(root / "note", 7);
    pump.until([&] { return settled(model) && named(*model.snapshot(), "note") != kNoNode; });
    TEST_CHECK(rootBytes(model) == 147);
    writeFile(root / "fresh" / "data", 20);
    pump.until([&] { return settled(model) && named(*model.snapshot(), "fresh") != kNoNode; });
    TEST_CHECK(rootBytes(model) == 167);
    fs::remove_all(root / "gone");
    pump.until([&] { return settled(model) && named(*model.snapshot(), "gone") == kNoNode; });
    TEST_CHECK(rootBytes(model) == 127);
  }

  // Many entries changed at once walk the folder again a few times, not once per entry.
  void burst(const fs::path& base) {
    const auto root = base / "burst";
    writeFile(root / "first", 1);
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    const Tables tables(cache, at(root));
    WorkbenchModel model(cache, {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(at(root));
    pump.until([&] { return settled(model); });
    for (int i = 0; i < 300; ++i) {
      writeFile(root / std::format("e{:03}", i), 2);
    }
    pump.until([&] { return settled(model) && rootBytes(model) == 601; });
    pump.forMilliseconds(300);
    TEST_CHECK(settled(model) && rootBytes(model) == 601);
    TEST_CHECK(tables.complete() >= 2 && tables.complete() <= 6);
  }

  // A file of the folder written every 50 ms walks the folder again a few times while it grows, not once per change,
  // and the picture shows its last size once the writes stop.
  void fileKeepsChanging(const fs::path& base) {
    const auto root = base / "growing";
    for (int d = 0; d < 20; ++d) {
      for (int f = 0; f < 20; ++f) {
        writeFile(root / std::format("d{:02}", d) / std::format("f{:02}", f), 1);
      }
    }
    writeFile(root / "log", 0);
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    const Tables tables(cache, at(root));
    WorkbenchModel model(cache, {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(at(root));
    pump.until([&] { return settled(model); });
    const auto before = tables.complete();
    constexpr int kWrites = 60;
    for (int i = 0; i < kWrites; ++i) {
      std::ofstream(root / "log", std::ios::app) << 'x';
      pump.forMilliseconds(50);
    }
    const auto during = tables.complete() - before;
    std::println("{} writes of one file, one per 50 ms: the folder was walked {} times", kWrites, during);
    TEST_CHECK(during >= 1 && during <= 5);
    pump.until([&] { return settled(model) && rootBytes(model) == 400 + kWrites; });
  }

  // A folder made unreadable once it is entered shows its error, and its picture again once it can be read.
  void unreadableOnceEntered(const fs::path& base) {
    if (::geteuid() == 0) {
      return;
    }
    const auto root = base / "unreadable";
    for (int i = 0; i < 40; ++i) {
      for (int f = 0; f < 50; ++f) {
        writeFile(root / std::format("d{:02}", i) / std::format("f{:02}", f), 10);
      }
    }
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    WorkbenchModel model(cache, {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(at(root));
    fs::permissions(root, fs::perms::none);
    pump.until([&] { return settled(model) && model.snapshot()->error.has_value(); });
    const auto shown = model.snapshot();
    TEST_CHECK(shown->error->kind == luciole::FileErrorKind::Denied && shown->location == at(root));
    TEST_CHECK(shown->jump.rows.empty() && shown->shares.empty());
    fs::permissions(root, fs::perms::owner_all);
    pump.until([&] { return settled(model) && !model.snapshot()->error && rootBytes(model) == 20'000; });
  }

  // A visit recorded while the page stays in its folder shows once the visit service reports it, as the page passes
  // it on.
  void visitsWhileStaying(const fs::path& base) {
    const auto root = base / "visited";
    writeFile(root / "a" / "data", 10);
    writeFile(root / "b" / "data", 10);
    luciole::VisitService visits(base / "visit-store", std::chrono::milliseconds(1));
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    WorkbenchModel model(cache, {.ancestorLevels = 0});
    const auto source = [&visits](const Location& folder) { return visits.visitsUnder(folder); };
    model.setVisits(source);
    visits.setChangedCallback([&] { model.setVisits(source); });
    Pump pump;
    pump.until([&] { return visits.ready(); });
    model.setLocation(at(root));
    pump.until([&] { return settled(model); });
    TEST_CHECK(model.snapshot()->tree.nodes[named(*model.snapshot(), "a")].visits == 0);
    visits.record(at(root / "a"));
    visits.record(at(root / "a"));
    pump.until([&] {
      const auto shown = model.snapshot();
      return settled(model) && shown->tree.nodes[named(*shown, "a")].visits == 2;
    });
    TEST_CHECK(model.snapshot()->tree.nodes[named(*model.snapshot(), "b")].visits == 0);
    visits.setChangedCallback({});
    pump.until([&] { return !visits.busy(); });
  }
  // What changes in a watched folder arrives at most once per interval: a few names alone, many as the whole folder,
  // a change of the folder itself as the whole folder, and nothing from a folder no longer watched.
  void folderChanges(const fs::path& base) {
    const auto folder = base / "changes";
    const auto other = base / "unwatched";
    fs::create_directories(folder);
    fs::create_directories(other);
    luciole::FolderChanges changes(std::chrono::milliseconds(50));
    std::vector<std::pair<std::vector<std::string>, bool>> calls;
    changes.setChangedCallback([&calls](std::span<const std::string> names, bool whole) {
      calls.emplace_back(std::vector<std::string>(names.begin(), names.end()), whole);
    });
    Pump pump;
    const auto quiet = [&] {
      for (std::size_t seen = calls.size(), round = 0; round < 300; ++round) {
        pump.once();
        if (calls.size() != seen) {
          seen = calls.size();
          round = 0;
        }
      }
    };
    changes.watch(Location::fromPath(folder.native()));
    std::ofstream(folder / "a") << "a";
    std::ofstream(folder / "b") << "b";
    pump.until([&] { return !calls.empty(); });
    quiet();
    std::vector<std::string> reported;
    for (const auto& [names, whole] : calls) {
      TEST_CHECK(!whole);
      reported.insert(reported.end(), names.begin(), names.end());
    }
    std::ranges::sort(reported);
    const auto [last, end] = std::ranges::unique(reported);
    reported.erase(last, end);
    TEST_CHECK((reported == std::vector<std::string>{"a", "b"}));

    calls.clear();
    for (int i = 0; i < 300; ++i) {
      std::ofstream(folder / std::format("e{:03}", i)) << "e";
    }
    pump.until([&] { return !calls.empty(); });
    quiet();
    TEST_CHECK(calls.front().second && calls.size() <= 4);
    for (const auto& [names, whole] : calls) {
      TEST_CHECK(whole ? names.empty() : names.size() <= luciole::FolderChanges::kNames);
    }

    calls.clear();
    fs::permissions(folder, fs::perms::owner_all | fs::perms::group_read);
    pump.until([&] { return !calls.empty(); });
    TEST_CHECK(calls.front().second && calls.front().first.empty());

    calls.clear();
    changes.watch(Location::fromPath(other.native()));
    std::ofstream(folder / "late") << "late";
    std::ofstream(other / "seen") << "seen";
    pump.until([&] { return !calls.empty(); });
    quiet();
    for (const auto& [names, whole] : calls) {
      TEST_CHECK(!whole && (names == std::vector<std::string>{"seen"}));
    }
    changes.stop();
    calls.clear();
    std::ofstream(other / "after") << "after";
    quiet();
    TEST_CHECK(calls.empty());
  }

  // An owner that destroys the model in its changed callback starts nothing more: no Jump Ahead walks follow.
  void destroyedInCallback(const fs::path& base) {
    const auto root = base / "callback";
    writeFile(root / "a" / "big" / "data", 1000);
    writeFile(root / "a" / "small" / "data", 10);
    writeFile(root / "b" / "mid" / "data", 100);
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    Pump pump;
    auto model = std::make_unique<WorkbenchModel>(cache, WorkbenchModelOptions{.ancestorLevels = 0});
    model->setChangedCallback([&] {
      if (model && model->snapshot() && !model->measuring()) {
        TEST_CHECK(!model->snapshot()->jump.rows.empty());
        model.reset();
      }
    });
    model->setLocation(at(root));
    pump.until([&] { return model == nullptr; });
    pump.forMilliseconds(50);
    TEST_CHECK(cache.tables() == 1 && idle(service));
  }

  void unmeasurable(const fs::path& base) {
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    WorkbenchModel model(cache, {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(at(base / "missing"));
    pump.until([&] { return settled(model); });
    TEST_CHECK(model.snapshot()->error && model.snapshot()->error->kind == luciole::FileErrorKind::Missing);
    TEST_CHECK(model.snapshot()->jump.rows.empty() && model.snapshot()->shares.empty());
    if (::geteuid() != 0) {
      fs::create_directories(base / "locked");
      fs::permissions(base / "locked", fs::perms::none);
      model.setLocation(at(base / "locked"));
      pump.until([&] { return settled(model) && model.snapshot()->location == at(base / "locked"); });
      TEST_CHECK(model.snapshot()->error && model.snapshot()->error->kind == luciole::FileErrorKind::Denied);
    }
    // A draft without a tree has nothing to graft a refreshed child into, and no entry has a share.
    Tree one;
    one.nodes.resize(1);
    one.root = 0;
    const luciole::WorkbenchDraft draft{
        .location = at(base),
        .ancestors = {},
        .measured = nullptr,
        .refreshed =
            {{.name = "x", .measured = std::make_shared<const luciole::MeasuredTree>(one), .error = {}, .from = {}}},
        .visits = nullptr,
        .settings = {},
        .error = std::nullopt,
    };
    const auto lone = luciole::finishSnapshot(draft, nullptr);
    TEST_CHECK(lone && lone->serial == 1 && lone->tree.nodes.empty() && lone->share({.name = "x", .bytes = 9}) == 0);
  }

} // namespace

int main() {
  const Fixture fixture;
  changesOnDisk(fixture.path);
  burst(fixture.path);
  fileKeepsChanging(fixture.path);
  unreadableOnceEntered(fixture.path);
  visitsWhileStaying(fixture.path);
  folderChanges(fixture.path);
  unmeasurable(fixture.path);
  destroyedInCallback(fixture.path);
  return 0;
}
