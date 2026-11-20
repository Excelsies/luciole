#include "browser/workbench_model.h"
#include "browser/workbench_services.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <print>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::Location;
  using luciole::MeasureCache;
  using luciole::SubtreeMeasureService;
  using luciole::WorkbenchModel;
  using luciole::WorkbenchModelOptions;
  using luciole::WorkbenchServices;
  using luciole::WorkbenchSnapshot;
  using luciole::workbench::kNoNode;
  using luciole::workbench::NodeId;
  using luciole::workbench::Tree;
  using Snapshot = std::shared_ptr<const WorkbenchSnapshot>;

  class Fixture {
  public:
    luciole::test::TempFolder tempFolder{"luciole-workbench-model"};
    fs::path path = tempFolder.path();
  };

  double threadMilliseconds() {
    timespec now{};
    TEST_CHECK(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return (static_cast<double>(now.tv_sec) * 1e3) + (static_cast<double>(now.tv_nsec) / 1e6);
  }

  // Runs the default main context; busy() is the processor time its dispatches took on this thread.
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
      const double before = threadMilliseconds();
      m_source.dispatch(fds, start);
      m_busy += threadMilliseconds() - before;
    }
    double busy() const { return m_busy; }

  private:
    GlibPollSource m_source;
    double m_busy = 0;
  };

  // Every snapshot the model publishes, with when it arrived.
  class Recorder {
  public:
    explicit Recorder(WorkbenchModel& model) : m_model(model) {
      model.setChangedCallback([this] {
        if (const auto latest = m_model.snapshot(); latest && (seen.empty() || seen.back() != latest)) {
          seen.push_back(latest);
          arrivals.push_back(std::chrono::steady_clock::now());
        }
      });
    }
    ~Recorder() { m_model.setChangedCallback({}); }
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    std::vector<Snapshot> seen;
    std::vector<std::chrono::steady_clock::time_point> arrivals;

  private:
    WorkbenchModel& m_model;
  };

  Location at(const fs::path& path) { return Location::fromPath(path.native()); }

  bool idle(const SubtreeMeasureService& service) {
    const auto status = service.status();
    return status.queued + status.running == 0;
  }

  void writeFile(const fs::path& path, std::size_t bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << std::string(bytes, 'x');
  }

  // 400 folders: 20 at the top, 19 inside each, and 50 files of 10 bytes in every one of them.
  fs::path largeTree(const fs::path& root) {
    for (int a = 0; a < 20; ++a) {
      const auto top = root / std::format("d{:02}", a);
      for (int b = -1; b < 19; ++b) {
        const auto folder = b < 0 ? top : top / std::format("s{:02}", b);
        for (int f = 0; f < 50; ++f) {
          writeFile(folder / std::format("f{:02}", f), 10);
        }
      }
    }
    return root;
  }

  // a/{big/1000, small/10}, b/mid/100, .hid/deep/5000 and a file of 7 bytes. Empty files in .hid/deep make the
  // walk outlast its first partial table.
  fs::path jumpTree(const fs::path& root) {
    writeFile(root / "a" / "big" / "data", 1000);
    writeFile(root / "a" / "small" / "data", 10);
    writeFile(root / "b" / "mid" / "data", 100);
    writeFile(root / ".hid" / "deep" / "data", 5000);
    for (int i = 0; i < 5000; ++i) {
      writeFile(root / ".hid" / "deep" / std::format("e{:04}", i), 0);
    }
    writeFile(root / "note", 7);
    return root;
  }

  luciole::MeasureServiceOptions quickPublication(std::size_t workers = 2) {
    luciole::MeasureServiceOptions options;
    options.publishInterval = std::chrono::milliseconds(1);
    options.workers = workers;
    return options;
  }

  NodeId named(const Tree& tree, NodeId parent, const std::string& name) {
    for (const auto id : luciole::workbench::children(tree, parent)) {
      if (tree.nodes[id].name == name) {
        return id;
      }
    }
    return kNoNode;
  }

  luciole::workbench::Node child(const Snapshot& snapshot, const std::string& name) {
    const auto id = named(snapshot->tree, snapshot->tree.root, name);
    TEST_CHECK(id != kNoNode);
    return snapshot->tree.nodes[id];
  }

  bool settled(const WorkbenchModel& model) { return model.snapshot() && !model.measuring(); }

  // A snapshot only adds to the one before: every child stays, sizes never shrink, a finished child stays as it was.
  void checkRefines(const Tree& before, const Tree& after) {
    TEST_CHECK(after.nodes[after.root].bytes >= before.nodes[before.root].bytes);
    for (const auto id : luciole::workbench::children(before, before.root)) {
      const auto& was = before.nodes[id];
      const auto now = named(after, after.root, was.name);
      TEST_CHECK(now != kNoNode);
      TEST_CHECK(after.nodes[now].bytes >= was.bytes && after.nodes[now].files >= was.files);
      if (!was.partial) {
        TEST_CHECK(!after.nodes[now].partial && after.nodes[now].bytes == was.bytes);
      }
    }
  }

  void progressive(const fs::path& root) {
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    WorkbenchModel model(cache, {.ancestorLevels = 0});
    Recorder recorder(model);
    Pump pump;
    TEST_CHECK(model.snapshot() == nullptr && model.serial() == 0);
    model.setLocation(at(root));
    TEST_CHECK(model.measuring());
    pump.until([&] { return settled(model); });
    TEST_CHECK(recorder.seen.size() >= 2);

    const auto& first = recorder.seen.front()->tree;
    TEST_CHECK(first.nodes[first.root].partial && luciole::workbench::children(first, first.root).size() == 20);
    TEST_CHECK(std::ranges::any_of(luciole::workbench::children(first, first.root), [&](NodeId id) {
      return first.nodes[id].partial;
    }));
    for (std::size_t i = 1; i < recorder.seen.size(); ++i) {
      TEST_CHECK(recorder.seen[i]->serial > recorder.seen[i - 1]->serial);
      checkRefines(recorder.seen[i - 1]->tree, recorder.seen[i]->tree);
    }
    // Builds start at least kPublishInterval apart; a build's own time shifts its arrival a little.
    const auto spread = recorder.arrivals.back() - recorder.arrivals.front() + std::chrono::milliseconds(50);
    TEST_CHECK(spread >= WorkbenchModel::kPublishInterval * static_cast<int>(recorder.seen.size() - 1));

    const auto last = model.snapshot();
    TEST_CHECK(last == recorder.seen.back() && last->serial == model.serial() && !last->error);
    TEST_CHECK(last->location == at(root) && last->tree.ancestors.empty());
    const auto& top = last->tree.nodes[last->tree.root];
    TEST_CHECK(!top.partial && top.bytes == 200'000 && top.files == 20'000 && top.items == 20);
    const auto d07 = child(last, "d07");
    TEST_CHECK(!d07.partial && d07.bytes == 10'000 && d07.files == 1000 && d07.items == 69);
    TEST_CHECK(named(last->tree, last->node("d07"), "s18") != kNoNode);
  }

  // Visits below `root`, ordered by URI, or in reverse to check that the model orders them itself.
  luciole::WorkbenchVisits visitsOf(
      const fs::path& root, const std::vector<std::pair<std::string, std::uint32_t>>& counts, bool ordered = true
  ) {
    return [root, counts, ordered](const Location& folder) {
      std::vector<luciole::VisitRecord> records;
      for (const auto& [path, count] : counts) {
        const auto uri = at(root / path).uri();
        if (uri.starts_with(folder.uri())) {
          records.push_back({.uri = uri, .visit = {.count = count, .last = 1}});
        }
      }
      std::ranges::sort(records, {}, &luciole::VisitRecord::uri);
      if (!ordered) {
        std::ranges::reverse(records);
      }
      return records;
    };
  }

  std::vector<std::string> rowPaths(const Snapshot& snapshot) {
    std::vector<std::string> paths;
    for (const auto& row : snapshot->jump.rows) {
      paths.push_back(row.path);
    }
    return paths;
  }

  // Waits for a change the model was just asked for: a build runs, then measuring() turns false again.
  void republished(Pump& pump, WorkbenchModel& model) {
    pump.until([&] { return model.measuring(); });
    pump.until([&] { return settled(model); });
  }

  void jumpSharesAndVisits(const fs::path& base) {
    const auto root = jumpTree(base / "anc" / "jump");
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    WorkbenchModel model(cache, {.ancestorLevels = 1});
    Pump pump;
    // Tables in the cache when the ancestor's own table first appears: the folder's and those of the Jump folders.
    std::optional<std::size_t> tablesAtAncestor;
    const auto watch = cache.addListener([&] {
      if (!tablesAtAncestor && cache.find(at(base / "anc")).record == 0) {
        tablesAtAncestor = cache.tables();
      }
    });
    const luciole::workbench::LayoutSettings settings{.emphasis = 1};
    model.setSettings(settings);
    model.setVisits(visitsOf(root, {{"a", 2}, {"b/mid", 3}}));
    model.setLocation(at(root));
    pump.until([&] { return settled(model); });
    auto shown = model.snapshot();
    TEST_CHECK(child(shown, "a").visits == 2 && child(shown, "b").visits == 0);
    TEST_CHECK(shown->tree.nodes[named(shown->tree, shown->node("b"), "mid")].visits == 3);
    TEST_CHECK((rowPaths(shown) == std::vector<std::string>{"b/mid", "a/big", "a/small"}));
    TEST_CHECK(shown->jumpLocations.size() == 3 && shown->jumpLocations[0] == at(root / "b" / "mid"));
    const auto direct = luciole::workbench::jumpAhead(shown->tree, settings);
    TEST_CHECK(direct.rows.size() == shown->jump.rows.size() && direct.partial == shown->jump.partial);
    for (std::size_t i = 0; i < direct.rows.size(); ++i) {
      TEST_CHECK(direct.rows[i].id == shown->jump.rows[i].id && direct.rows[i].label == shown->jump.rows[i].label);
      TEST_CHECK(direct.rows[i].share == shown->jump.rows[i].share);
    }

    // Shares: weights are bytes plus one at emphasis 1; a hidden entry has none. A name the tree does not hold
    // weighs what its listing says.
    TEST_NEAR(shown->share({.name = "a"}), 1011.0 / 1120.0, 1e-12);
    TEST_NEAR(shown->share({.name = "b"}), 101.0 / 1120.0, 1e-12);
    TEST_NEAR(shown->share({.name = "note", .bytes = 99}), 8.0 / 1120.0, 1e-12);
    TEST_CHECK(shown->share({.name = ".hid"}) == 0 && shown->share({.name = ".new", .hidden = true}) == 0);
    TEST_NEAR(shown->share({.name = "absent", .bytes = 55}), 56.0 / 1120.0, 1e-12);
    TEST_CHECK(shown->node("absent") == kNoNode);
    for (const auto& entry : shown->shares) {
      TEST_CHECK(entry.share == luciole::workbench::share(shown->tree, entry.id, settings));
    }

    // Jump ahead folders are measured next, then the ancestor.
    pump.until([&] { return tablesAtAncestor.has_value(); });
    TEST_CHECK(*tablesAtAncestor == 5);
    cache.removeListener(watch);
    pump.until([&] { return settled(model) && model.snapshot()->tree.ancestors.size() == 1; });
    shown = model.snapshot();
    pump.until([&] {
      shown = model.snapshot();
      return !shown->tree.nodes[shown->tree.ancestors[0]].partial;
    });
    const auto& parent = shown->tree.nodes[shown->tree.ancestors[0]];
    TEST_CHECK(
        parent.name == "anc"
        && parent.bytes == 6117
        && named(shown->tree, shown->tree.ancestors[0], "jump") == shown->tree.root
    );

    // The same visits again build an equal tree: no new serial. Other visits or settings do, from the same tree.
    const auto serial = model.serial();
    model.setVisits(visitsOf(root, {{"a", 2}, {"b/mid", 3}}));
    republished(pump, model);
    TEST_CHECK(model.serial() == serial && model.snapshot() == shown);
    model.setVisits(visitsOf(root, {{"a/big", 9}, {"b/mid", 3}}, false));
    republished(pump, model);
    TEST_CHECK(model.serial() == serial + 1 && rowPaths(model.snapshot())[0] == "a/big");
    model.setSettings({.emphasis = 1, .showHidden = true});
    republished(pump, model);
    TEST_CHECK(model.serial() == serial + 2 && model.snapshot()->tree.now == shown->tree.now);
    TEST_CHECK((rowPaths(model.snapshot()) == std::vector<std::string>{"a/big", "b/mid", ".hid/deep", "a/small"}));
    TEST_CHECK(model.snapshot()->share({.name = ".hid"}) > 0);
  }

  // The first snapshot already lists each ancestor's entries, and measuring() does not wait for ancestor walks.
  void ancestorsListed(const fs::path& base) {
    const auto parent = base / "level";
    writeFile(parent / "here" / "data", 4);
    writeFile(parent / "file", 3);
    fs::create_directories(parent / "other");
    for (int i = 0; i < 8000; ++i) {
      writeFile(parent / "bulk" / std::format("e{:04}", i), 0);
    }
    SubtreeMeasureService service(quickPublication(1));
    MeasureCache cache(service);
    // Holds the only worker, so the folder's walk waits while the listing is read.
    TEST_CHECK(cache.measure(at(parent / "bulk"), luciole::MeasurePriority::Current).measuring);
    WorkbenchModel model(cache, {.ancestorLevels = 1});
    Recorder recorder(model);
    Pump pump;
    model.setLocation(at(parent / "here"));
    pump.until([&] { return settled(model); });
    const auto& first = recorder.seen.front()->tree;
    TEST_CHECK(first.ancestors.size() == 1 && luciole::workbench::children(first, first.ancestors[0]).size() == 4);
    TEST_CHECK(named(first, first.ancestors[0], "other") != kNoNode && first.nodes[first.ancestors[0]].items == 4);
    TEST_CHECK(!idle(service) && cache.find(at(parent)).record != 0);
    pump.until([&] { return idle(service) && settled(model); });
    const auto& last = model.snapshot()->tree;
    TEST_CHECK(last.nodes[named(last, last.ancestors[0], "bulk")].files == 8000);
  }

  void invalidated(const fs::path& base) {
    const auto root = base / "inv";
    writeFile(root / "x" / "f", 100);
    writeFile(root / "y" / "g", 200);
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    WorkbenchModel model(cache, {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(at(root));
    pump.until([&] { return settled(model); });
    const auto folderTable = cache.find(at(root)).table;

    writeFile(root / "x" / "h", 50);
    model.invalidateChild("x");
    pump.until([&] { return settled(model) && child(model.snapshot(), "x").bytes == 150; });
    const auto shown = model.snapshot();
    TEST_CHECK(shown->tree.nodes[shown->tree.root].bytes == 350 && child(shown, "y").bytes == 200);
    TEST_CHECK(child(shown, "x").files == 2 && shown->tree.nodes[shown->tree.root].files == 3);
    TEST_CHECK(cache.find(at(root)).table == folderTable && cache.find(at(root / "y")).table == folderTable);
    TEST_CHECK(cache.tables() == 2);
    writeFile(root / "x" / "i", 25);
    model.invalidateChild("x");
    pump.until([&] { return settled(model) && child(model.snapshot(), "x").bytes == 175; });
    const auto rootBytes = [&] { return model.snapshot()->tree.nodes[model.snapshot()->tree.root].bytes; };
    TEST_CHECK(rootBytes() == 375 && cache.find(at(root)).table == folderTable);
    if (::geteuid() != 0) {
      fs::permissions(root / "y", fs::perms::none);
      model.invalidateChild("y");
      pump.until([&] { return settled(model) && child(model.snapshot(), "y").unreadable; });
      fs::permissions(root / "y", fs::perms::owner_all);
    }

    // A child that is gone leaves the picture; a file walks the folder again, known or new.
    fs::remove_all(root / "y");
    model.invalidateChild("y");
    pump.until([&] { return settled(model) && model.snapshot()->node("y") == kNoNode; });
    TEST_CHECK(rootBytes() == 175);
    writeFile(root / "z", 7);
    model.invalidateChild("z");
    pump.until([&] { return settled(model) && model.snapshot()->node("z") != kNoNode; });
    TEST_CHECK(rootBytes() == 182 && cache.find(at(root)).table != folderTable);
    writeFile(root / "z", 17);
    model.invalidateChild("z");
    pump.until([&] { return settled(model) && rootBytes() == 192; });
  }

  // 515 folders d000 to d514 with one file of 100 + i bytes, and 30 files f00 to f29 of 10 + i bytes: the walk
  // folds the three smallest folders and the six smallest files.
  fs::path foldedTree(const fs::path& root) {
    for (int i = 0; i < 515; ++i) {
      writeFile(root / std::format("d{:03}", i) / "data", 100 + static_cast<std::size_t>(i));
    }
    for (int i = 0; i < 30; ++i) {
      writeFile(root / std::format("f{:02}", i), 10 + static_cast<std::size_t>(i));
    }
    return root;
  }

  // A folded entry has the share its listing gives it; a folded folder that changes walks the folder again.
  void folded(const fs::path& base) {
    const auto root = foldedTree(base / "folded");
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    WorkbenchModel model(cache, {.ancestorLevels = 0});
    Pump pump;
    model.setSettings({.emphasis = 1});
    model.setLocation(at(root));
    pump.until([&] { return settled(model); });
    // Kept folders 184,064, their remainder 304, kept files 684, their remainder 76, each with one added.
    const auto shown = model.snapshot();
    TEST_CHECK(shown->node("f00") == kNoNode && shown->node("d000") == kNoNode);
    TEST_NEAR(shown->share({.name = "f00", .bytes = 10}), 11.0 / 185'128.0, 1e-15);
    TEST_NEAR(shown->share({.name = "d000", .folder = true}), 1.0 / 185'128.0, 1e-15);
    TEST_NEAR(shown->share({.name = "f29"}), 40.0 / 185'128.0, 1e-15);
    const auto& top = shown->tree.nodes[shown->tree.root];
    TEST_CHECK(top.bytes == 183'855 + 735 && top.items == 545);

    writeFile(root / "d000" / "more", 50);
    model.invalidateChild("d000");
    pump.until([&] { return settled(model) && model.snapshot() != shown; });
    const auto& after = model.snapshot()->tree.nodes[model.snapshot()->tree.root];
    TEST_CHECK(after.bytes == 183'855 + 735 + 50 && after.items == 545 && after.files == 546);
  }

  // Destroying the model cancels its walk, and the cache keeps nothing of a walk that did not end.
  void destroyedWhileMeasuring(const fs::path& root) {
    SubtreeMeasureService service(quickPublication());
    MeasureCache cache(service);
    Pump pump;
    auto model = std::make_unique<WorkbenchModel>(cache, WorkbenchModelOptions{.ancestorLevels = 0});
    model->setLocation(at(root));
    TEST_CHECK(service.status().queued + service.status().running == 1);
    model.reset();
    TEST_CHECK(cache.tables() == 0 && service.status().queued == 0);
    // Again once the walk has published a table the model has yet to turn into a snapshot.
    model = std::make_unique<WorkbenchModel>(cache, WorkbenchModelOptions{.ancestorLevels = 0});
    model->setLocation(at(root));
    pump.until([&] { return cache.find(at(root)).record == 0; });
    TEST_CHECK(cache.find(at(root)).measuring && model->measuring());
    model.reset();
    TEST_CHECK(cache.find(at(root)).entry() == nullptr && cache.tables() == 0);
    pump.until([&] { return idle(service); });
    pump.forMilliseconds(150);
    TEST_CHECK(cache.tables() == 0);
  }

  // Time to the first and the final snapshot of 20,000 files in 400 folders, with the application's services, and
  // the main loop's processor time per snapshot.
  void timing(const fs::path& root) {
    WorkbenchServices services;
    WorkbenchModel model(services.measureCache(), {.ancestorLevels = 0});
    Recorder recorder(model);
    Pump pump;
    const auto start = std::chrono::steady_clock::now();
    model.setLocation(at(root));
    pump.until([&] { return settled(model); });
    const auto since = [&](auto point) { return std::chrono::duration<double, std::milli>(point - start).count(); };
    std::println(
        "workbench model: first snapshot {:.1f} ms, final {:.1f} ms, {} snapshots, {} nodes, main loop {:.2f} ms each",
        since(recorder.arrivals.front()), since(recorder.arrivals.back()), recorder.seen.size(),
        model.snapshot()->tree.nodes.size(), pump.busy() / static_cast<double>(recorder.seen.size())
    );
    TEST_CHECK(model.snapshot()->tree.nodes[model.snapshot()->tree.root].files == 20'000);
    // The trees are built on a worker: the main loop only hands over pointers.
    TEST_CHECK(pump.busy() / static_cast<double>(recorder.seen.size()) < 2.0);
  }
} // namespace

int main() {
  const Fixture fixture;
  const auto large = largeTree(fixture.path / "large");
  timing(large);
  progressive(large);
  jumpSharesAndVisits(fixture.path);
  invalidated(fixture.path);
  folded(fixture.path);
  ancestorsListed(fixture.path);
  destroyedWhileMeasuring(large);
  return 0;
}
