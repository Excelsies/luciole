#include "browser/workbench_model.h"
#include "browser/workbench_services.h"
#include "event_loop/glib_poll_source.h"
#include "fs/file_query.h"
#include "fs/folder_totals_walk.h"
#include "fs/gio_ptr.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <poll.h>
#include <print>
#include <string>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::Location;
  using luciole::WorkbenchSnapshot;
  using luciole::workbench::Node;

  // A snapshot whose folder holds one child per node, named n0, n1, ...
  WorkbenchSnapshot snapshotOf(const std::vector<Node>& children) {
    luciole::workbench::Tree tree;
    tree.root = 0;
    tree.nodes.push_back({.name = "root", .folder = true});
    std::vector<luciole::WorkbenchShare> shares;
    for (const auto& child : children) {
      const auto id = static_cast<luciole::workbench::NodeId>(tree.nodes.size());
      tree.nodes.push_back(child);
      tree.nodes.back().name = "n" + std::to_string(id);
      shares.push_back({.name = "n" + std::to_string(id), .id = id, .share = 0});
    }
    std::ranges::sort(shares, {}, &luciole::WorkbenchShare::name);
    return {
        .serial = 1,
        .location = Location::fromPath("/r"),
        .settings = {},
        .tree = std::move(tree),
        .jump = {},
        .jumpLocations = {},
        .shares = std::move(shares),
        .total = 0,
        .error = std::nullopt,
    };
  }

  // The same text as the List view's cell, with literals worked out by hand: 1536 bytes are 1.5\u00a0KiB.
  void textOfMeasuredNodes() {
    const auto snapshot = snapshotOf({
        {.name = "", .bytes = 1536, .folder = true},                          // n1 measured
        {.name = "", .bytes = 1536, .folder = true, .partial = true},         // n2 still measuring
        {.name = "", .bytes = 1536, .folder = true, .lowerBound = true},      // n3 cut short
        {.name = "", .bytes = 0, .folder = true, .partial = true},            // n4 nothing counted yet
        {.name = "", .bytes = 1536, .folder = true, .notMeasured = true},     // n5
        {.name = "", .bytes = 1536, .folder = true, .otherFilesystem = true}, // n6
        {.name = "", .bytes = 1536, .folder = true, .unreadable = true},      // n7
        {.name = "", .bytes = 0, .folder = true},                             // n8 empty and measured
        {.name = "", .bytes = 1536, .folder = false},                         // n9 a file
    });
    TEST_CHECK(snapshot.folderSizeText("n1") == "1.5\u00a0KiB");
    TEST_CHECK(snapshot.folderSizeText("n2") == "≥ 1.5\u00a0KiB");
    TEST_CHECK(snapshot.folderSizeText("n3") == "≥ 1.5\u00a0KiB");
    TEST_CHECK(snapshot.folderSizeText("n4").empty());
    TEST_CHECK(snapshot.folderSizeText("n5").empty());
    TEST_CHECK(snapshot.folderSizeText("n6").empty());
    TEST_CHECK(snapshot.folderSizeText("n7").empty());
    TEST_CHECK(snapshot.folderSizeText("n8") == "0 bytes");
    TEST_CHECK(snapshot.folderSizeText("n9").empty());
    TEST_CHECK(snapshot.folderSizeText("missing").empty());
  }

  void textOfAFolderTheSnapshotDoesNotMeasure() {
    auto snapshot = snapshotOf({{.name = "", .bytes = 1536, .folder = true}});
    snapshot.notMeasured = true;
    TEST_CHECK(snapshot.folderSizeText("n1").empty());
  }

  void write(const fs::path& path, std::size_t bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << std::string(bytes, 'x');
  }

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

  luciole::DirectoryEntry entryOf(const fs::path& path) {
    luciole::DirectoryEntry result{
        .place = Location::fromPath(path.native()), .name = {}, .size = {}, .modifiedSeconds = {}
    };
    const auto facts = luciole::readFileDetails(result.location(), {});
    result.type = facts.type;
    result.setIdentity(facts.identity);
    result.setEtag(facts.etag);
    result.contentRevision = facts.contentRevision;
    return result;
  }

  // The folder `links` holds a 1000 byte file and a second name for it, `plain` one of 500 bytes. Workbench counts
  // the linked file once; the List view's walk counts every name.
  void hardLinkedFileCountsOnce(const fs::path& base) {
    write(base / "links" / "a", 1000);
    fs::create_hard_link(base / "links" / "a", base / "links" / "b");
    write(base / "plain" / "c", 500);

    luciole::WorkbenchServices services;
    luciole::WorkbenchModel model(services.measureCache(), {.ancestorLevels = 0});
    Pump pump;
    model.setLocation(Location::fromPath(base.native()));
    pump.until([&] { return model.snapshot() && !model.measuring(); });
    const auto snapshot = model.snapshot();

    const auto links = snapshot->node("links");
    TEST_CHECK(links != luciole::workbench::kNoNode);
    TEST_CHECK(snapshot->tree.nodes[links].bytes == 1000); // the instrument's number for the folder
    TEST_CHECK(snapshot->folderSizeText("links") == "1000 bytes");
    TEST_CHECK(snapshot->folderSizeText("plain") == "500 bytes");

    // The List view's walk of the same folder adds the file at both of its names.
    const auto totals = luciole::walkFolderTotals({entryOf(base / "links")}, {}, [](const auto&) {});
    TEST_CHECK(totals.bytes == 2000 && !totals.partial());
  }
  // The walk of the folder stops after each partial table until the main context has seen what the model publishes
  // for it, so every partial table is observed. In every snapshot a folder's cell is empty, starts with "≥", or is
  // the final text; once it shows a number it never goes empty again; the last text has no mark.
  void cellsGrowWithoutBlinking(const fs::path& base) {
    constexpr int kFolders = 6;
    for (int folder = 0; folder < kFolders; ++folder) {
      for (int file = 0; file < 400; ++file) {
        write(base / "grow" / std::format("d{}", folder) / std::format("f{:03}", file), 10);
      }
    }
    struct Gate {
      std::mutex mutex;
      std::condition_variable changed;
      std::size_t stored = 0;   // partial tables stored by the walk
      std::size_t released = 0; // partial tables the main context has looked at
    } gate;
    luciole::MeasureServiceOptions options;
    options.publishInterval = std::chrono::milliseconds(1);
    options.afterPartial = [&gate] {
      std::unique_lock lock(gate.mutex);
      const auto mine = ++gate.stored;
      gate.changed.notify_all();
      gate.changed.wait_for(lock, std::chrono::seconds(30), [&] { return gate.released >= mine; });
    };
    luciole::SubtreeMeasureService service(options);
    luciole::MeasureCache cache(service);
    luciole::WorkbenchModel model(cache, {.ancestorLevels = 0});
    std::vector<std::shared_ptr<const WorkbenchSnapshot>> seen;
    model.setChangedCallback([&] {
      if (const auto latest = model.snapshot(); latest && (seen.empty() || seen.back() != latest)) {
        seen.push_back(latest);
      }
    });
    const auto stored = [&] {
      const std::lock_guard lock(gate.mutex);
      return gate.stored;
    };
    const auto release = [&] {
      const std::lock_guard lock(gate.mutex);
      gate.released = gate.stored;
      gate.changed.notify_all();
    };
    Pump pump;
    model.setLocation(Location::fromPath((base / "grow").native()));
    std::size_t looked = 0;
    while (!(model.snapshot() && !model.measuring())) {
      pump.until([&] { return stored() > looked || (model.snapshot() && !model.measuring()); });
      if (stored() > looked) {
        // The model publishes at most every kPublishInterval: wait for what it makes of this table.
        const auto before = seen.size();
        const auto limit = std::chrono::steady_clock::now() + 3 * luciole::WorkbenchModel::kPublishInterval;
        pump.until([&] { return seen.size() > before || std::chrono::steady_clock::now() > limit; });
        looked = stored();
        release();
      }
    }
    release();
    model.setChangedCallback({});
    TEST_CHECK(seen.size() >= 3 && looked >= 3);
    const auto last = [&](const std::string& name) { return seen.back()->folderSizeText(name); };
    std::size_t marked = 0;
    for (int folder = 0; folder < kFolders; ++folder) {
      const auto name = std::format("d{}", folder);
      const auto final = last(name);
      TEST_CHECK(final == "3.9\u00a0KiB" && !final.starts_with("≥")); // 400 files of 10 bytes
      bool shown = false;
      for (const auto& snapshot : seen) {
        const auto text = snapshot->folderSizeText(name);
        TEST_CHECK(text.empty() || text.starts_with("≥") || text == final);
        TEST_CHECK(!shown || !text.empty());
        shown = shown || !text.empty();
        marked += text.starts_with("≥") ? 1 : 0;
      }
    }
    TEST_CHECK(marked > 0); // a partial table was seen with a number in it
  }

} // namespace

int main() {
  const luciole::test::TempFolder folder{"luciole-workbench-folder-size"};
  textOfMeasuredNodes();
  textOfAFolderTheSnapshotDoesNotMeasure();
  hardLinkedFileCountsOnce(folder.path());
  cellsGrowWithoutBlinking(folder.path());
  std::println("workbench folder size checks passed");
  return 0;
}
