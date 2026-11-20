#include "browser/measured_tree.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glib.h>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::ListedEntry;
  using luciole::Location;
  using luciole::MeasureCache;
  using luciole::MeasuredTreeInput;
  using luciole::MeasurePriority;
  using luciole::SubtreeMeasureService;
  using luciole::workbench::kNoNode;
  using luciole::workbench::NodeId;
  using luciole::workbench::Tree;

  struct Scratch : luciole::test::TempFolder {
    Scratch() : TempFolder("luciole-measured-tree") {}
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

  void writeFile(const fs::path& path, std::size_t bytes) { std::ofstream(path) << std::string(bytes, 'x'); }

  Location at(const fs::path& path) { return Location::fromPath(path.native()); }

  void settle(MeasureCache& cache, const fs::path& folder, MeasurePriority priority = MeasurePriority::Current) {
    pumpUntil([&] {
      const auto found = cache.measure(at(folder), priority);
      return found.entry() != nullptr && !found.measuring && !found.stale;
    });
  }

  // top/{parent/{current/{a/{x.bin 300, b/{y.txt 20, c/{z 5, d/{w 1}}}}, f1.txt 100, f2.png 50}, sibling/{s 7},
  // note.md 9}, other/}
  fs::path fixture(const fs::path& base) {
    const auto current = base / "top" / "parent" / "current";
    fs::create_directories(current / "a" / "b" / "c" / "d");
    fs::create_directories(base / "top" / "parent" / "sibling");
    fs::create_directories(base / "top" / "other");
    writeFile(current / "a" / "x.bin", 300);
    writeFile(current / "a" / "b" / "y.txt", 20);
    writeFile(current / "a" / "b" / "c" / "z", 5);
    writeFile(current / "a" / "b" / "c" / "d" / "w", 1);
    writeFile(current / "f1.txt", 100);
    writeFile(current / "f2.png", 50);
    writeFile(base / "top" / "parent" / "sibling" / "s", 7);
    writeFile(base / "top" / "parent" / "note.md", 9);
    return current;
  }

  MeasuredTreeInput input(const fs::path& current) {
    const auto parent = current.parent_path();
    return {
        .current = at(current),
        .entries =
            {{.name = "a", .folder = true, .items = 2},
             {.name = "f1.txt", .bytes = 100, .modified = 10},
             {.name = "f2.png", .bytes = 50, .modified = 20}},
        .ancestors =
            {{.location = at(parent),
              .entries =
                  {{.name = "current", .folder = true},
                   {.name = "sibling", .folder = true, .items = 1},
                   {.name = "note.md", .bytes = 9, .modified = 30}}},
             {.location = at(parent.parent_path()),
              .entries = {{.name = "parent", .folder = true}, {.name = "other", .folder = true}}}},
        .visits = {},
        .now = 1000,
    };
  }

  NodeId named(const Tree& tree, NodeId parent, const std::string& name) {
    for (const auto id : luciole::workbench::children(tree, parent)) {
      if (tree.nodes[id].name == name) {
        return id;
      }
    }
    return kNoNode;
  }

  std::vector<std::string> names(const Tree& tree, NodeId parent) {
    std::vector<std::string> result;
    for (const auto id : luciole::workbench::children(tree, parent)) {
      result.push_back(tree.nodes[id].name);
    }
    std::ranges::sort(result);
    return result;
  }

  // Every child points back at its parent and every non-root node is somebody's child once.
  void checkLinks(const Tree& tree) {
    std::vector<int> seen(tree.nodes.size(), 0);
    for (NodeId id = 0; id < tree.nodes.size(); ++id) {
      for (const auto child : luciole::workbench::children(tree, id)) {
        TEST_CHECK(tree.nodes[child].parent == id);
        ++seen[child];
      }
    }
    for (NodeId id = 0; id < tree.nodes.size(); ++id) {
      TEST_CHECK(seen[id] == (tree.nodes[id].parent == kNoNode ? 0 : 1));
    }
  }

  void measuredCurrent(const fs::path& current) {
    SubtreeMeasureService service;
    MeasureCache cache(service);
    settle(cache, current);
    auto request = input(current);
    const std::map<std::string, double> visits{
        {at(current).uri(), 7}, {at(current / "a").uri(), 5}, {at(current / "f1.txt").uri(), 2}
    };
    request.visits = [&](const Location& location) {
      const auto found = visits.find(location.uri());
      return found == visits.end() ? 0.0 : found->second;
    };
    const auto measured = luciole::buildMeasuredTree(request, *cache.view());
    const auto& tree = measured.tree;
    checkLinks(tree);
    TEST_CHECK(measured.remainders.empty() && tree.now == 1000);
    const auto& root = tree.nodes[tree.root];
    TEST_CHECK(root.name == "current" && root.folder && !root.partial && !root.hidden);
    TEST_CHECK(root.bytes == 476 && root.files == 6 && root.items == 3 && root.visits == 7);
    TEST_CHECK((names(tree, tree.root) == std::vector<std::string>{"a", "f1.txt", "f2.png"}));
    const auto a = named(tree, tree.root, "a");
    TEST_CHECK(tree.nodes[a].bytes == 326 && tree.nodes[a].files == 4 && tree.nodes[a].items == 2);
    TEST_CHECK(tree.nodes[a].visits == 5 && tree.nodes[a].category == 1);
    const auto f1 = named(tree, tree.root, "f1.txt");
    TEST_CHECK(!tree.nodes[f1].folder && tree.nodes[f1].bytes == 100 && tree.nodes[f1].visits == 2);
    const auto b = named(tree, a, "b");
    TEST_CHECK((names(tree, b) == std::vector<std::string>{"c", "y.txt"}));
    const auto c = named(tree, b, "c");
    TEST_CHECK(tree.nodes[c].bytes == 6 && tree.nodes[c].files == 2 && tree.nodes[c].childCount == 0);
    TEST_CHECK(luciole::workbench::depth(tree, c) == 5);

    // Ancestors without a measure list their entries by name, flagged partial.
    TEST_CHECK(tree.ancestors.size() == 2 && tree.nodes[tree.root].parent == tree.ancestors[0]);
    const auto& parent = tree.nodes[tree.ancestors[0]];
    TEST_CHECK(parent.name == "parent" && parent.partial && parent.items == 3);
    TEST_CHECK((names(tree, tree.ancestors[0]) == std::vector<std::string>{"current", "note.md", "sibling"}));
    TEST_CHECK(named(tree, tree.ancestors[0], "current") == tree.root);
    const auto sibling = named(tree, tree.ancestors[0], "sibling");
    TEST_CHECK(tree.nodes[sibling].partial && tree.nodes[sibling].items == 1 && tree.nodes[sibling].bytes == 0);
    TEST_CHECK(tree.nodes[named(tree, tree.ancestors[0], "note.md")].bytes == 9);
    TEST_CHECK(tree.nodes[tree.ancestors[1]].name == "top" && tree.nodes[tree.ancestors[1]].parent == kNoNode);
    TEST_CHECK(named(tree, tree.ancestors[1], "parent") == tree.ancestors[0]);

    // A measured ancestor comes from the cache.
    settle(cache, current.parent_path(), MeasurePriority::Ancestor);
    const auto withParent = luciole::buildMeasuredTree(input(current), *cache.view()).tree;
    checkLinks(withParent);
    const auto& measuredParent = withParent.nodes[withParent.ancestors[0]];
    TEST_CHECK(!measuredParent.partial && measuredParent.bytes == 476 + 7 + 9 && measuredParent.items == 3);
    const auto measuredSibling = named(withParent, withParent.ancestors[0], "sibling");
    TEST_CHECK(!withParent.nodes[measuredSibling].partial && withParent.nodes[measuredSibling].bytes == 7);
    TEST_CHECK(withParent.nodes[measuredSibling].childCount == 0);
    TEST_CHECK(named(withParent, withParent.ancestors[0], "current") == withParent.root);
  }

  void listedOnly(const fs::path& current) {
    SubtreeMeasureService service;
    const MeasureCache cache(service);
    const auto tree = luciole::buildMeasuredTree(input(current), *cache.view()).tree;
    checkLinks(tree);
    const auto& root = tree.nodes[tree.root];
    TEST_CHECK(root.name == "current" && root.partial && root.items == 3 && root.bytes == 150 && root.files == 2);
    const auto a = named(tree, tree.root, "a");
    TEST_CHECK(tree.nodes[a].folder && tree.nodes[a].partial && tree.nodes[a].items == 2 && tree.nodes[a].bytes == 0);
    TEST_CHECK(tree.nodes[named(tree, tree.root, "f2.png")].modified == 20);

    // An ancestor listing that lacks the folder on the path still holds it.
    auto unlisted = input(current);
    unlisted.ancestors[0].entries.clear();
    const auto lone = luciole::buildMeasuredTree(unlisted, *cache.view()).tree;
    checkLinks(lone);
    TEST_CHECK((names(lone, lone.ancestors[0]) == std::vector<std::string>{"current"}));
    TEST_CHECK(lone.nodes[lone.ancestors[0]].items == 1);

    // A level that is not the parent of the one before ends the chain.
    auto broken = input(current);
    broken.ancestors[1].location = at(current);
    TEST_CHECK(luciole::buildMeasuredTree(broken, *cache.view()).tree.ancestors.size() == 1);
    broken.ancestors[0].location = at(current / "a");
    TEST_CHECK(luciole::buildMeasuredTree(broken, *cache.view()).tree.ancestors.empty());
  }

  // A listed folder whose child has a measure shows that child's levels from the cache.
  void listedWithMeasuredChild(const fs::path& current) {
    SubtreeMeasureService service;
    MeasureCache cache(service);
    settle(cache, current / "a");
    const auto tree = luciole::buildMeasuredTree(input(current), *cache.view()).tree;
    checkLinks(tree);
    TEST_CHECK(tree.nodes[tree.root].partial && tree.nodes[tree.root].bytes == 476);
    const auto a = named(tree, tree.root, "a");
    TEST_CHECK(!tree.nodes[a].partial && tree.nodes[a].bytes == 326);
    const auto b = named(tree, a, "b");
    TEST_CHECK((names(tree, b) == std::vector<std::string>{"c", "y.txt"}));
    TEST_CHECK(tree.nodes[named(tree, b, "c")].childCount == 0);
  }

  void insideAncestorTable(const fs::path& current) {
    SubtreeMeasureService service;
    MeasureCache cache(service);
    settle(cache, current.parent_path());
    auto request = input(current);
    request.ancestors.clear();
    const auto tree = luciole::buildMeasuredTree(request, *cache.view()).tree;
    TEST_CHECK(!tree.nodes[tree.root].partial && tree.nodes[tree.root].bytes == 476);
    // b sits at the parent table's detail depth, so its folder c has no record: b is not expanded.
    const auto b = named(tree, named(tree, tree.root, "a"), "b");
    TEST_CHECK(b != kNoNode && tree.nodes[b].bytes == 26 && tree.nodes[b].childCount == 0);
    TEST_CHECK(luciole::workbench::folderState(tree.nodes[b]) == luciole::workbench::FolderState::NotExpanded);

    // As the current folder, b comes from its listing instead.
    request.current = at(current / "a" / "b");
    request.entries = {{.name = "c", .folder = true, .items = 2}, {.name = "y.txt", .bytes = 20}};
    const auto edge = luciole::buildMeasuredTree(request, *cache.view()).tree;
    checkLinks(edge);
    TEST_CHECK(edge.nodes[edge.root].partial && edge.nodes[edge.root].name == "b");
    TEST_CHECK((names(edge, edge.root) == std::vector<std::string>{"c", "y.txt"}));
  }

  Tree withAncestor(MeasureCache& cache, const fs::path& current) {
    const MeasuredTreeInput request{
        .current = at(current),
        .entries = {},
        .ancestors = {{.location = at(current.parent_path()), .entries = {}}},
        .visits = {},
        .now = 0,
    };
    const auto tree = luciole::buildMeasuredTree(request, *cache.view()).tree;
    checkLinks(tree);
    TEST_CHECK(tree.ancestors.size() == 1);
    return tree;
  }

  // The current folder is a symlink in its parent: it takes the symlink's place.
  void symlinkedCurrent(const fs::path& base) {
    fs::create_directories(base / "p" / "real");
    writeFile(base / "p" / "real" / "data", 1000);
    fs::create_directory_symlink("real", base / "p" / "link");
    SubtreeMeasureService service;
    MeasureCache cache(service);
    settle(cache, base / "p");
    settle(cache, base / "p" / "link");
    const auto tree = withAncestor(cache, base / "p" / "link");
    TEST_CHECK((names(tree, tree.ancestors[0]) == std::vector<std::string>{"link", "real"}));
    TEST_CHECK(named(tree, tree.ancestors[0], "link") == tree.root && tree.nodes[tree.root].folder);
  }

  // The current folder was folded into its parent's folder remainder: the remainder leaves it out.
  void foldedCurrent(const fs::path& base) {
    const auto parent = base / "q";
    for (const auto& [name, bytes] : {std::pair{"big", 1000}, {"mid", 100}, {"small", 10}}) {
      fs::create_directories(parent / name);
      writeFile(parent / name / "data", bytes);
    }
    luciole::MeasureServiceOptions options;
    options.walk.childCap = 1;
    SubtreeMeasureService service(options);
    MeasureCache cache(service);
    settle(cache, parent);
    settle(cache, parent / "small");
    const auto measured = luciole::buildMeasuredTree(
        {.current = at(parent / "small"),
         .entries = {},
         .ancestors = {{.location = at(parent), .entries = {}}},
         .visits = {},
         .now = 0},
        *cache.view()
    );
    const auto& tree = measured.tree;
    checkLinks(tree);
    TEST_CHECK((names(tree, tree.ancestors[0]) == std::vector<std::string>{"", "big", "small"}));
    TEST_CHECK(named(tree, tree.ancestors[0], "small") == tree.root && measured.remainders.size() == 1);
    const auto& rest = tree.nodes[measured.remainders[0]];
    TEST_CHECK(rest.parent == tree.ancestors[0] && rest.items == 1 && rest.bytes == 100 && rest.files == 1);

    // Folded alone, it leaves no remainder.
    fs::remove_all(parent / "mid");
    cache.invalidate(at(parent));
    settle(cache, parent);
    const auto alone = withAncestor(cache, parent / "small");
    TEST_CHECK((names(alone, alone.ancestors[0]) == std::vector<std::string>{"big", "small"}));
  }

  void remaindersAndLimits(const fs::path& current) {
    luciole::MeasureServiceOptions options;
    options.walk.fileCap = 1;
    options.walk.childCap = 0;
    SubtreeMeasureService service(options);
    MeasureCache cache(service);
    settle(cache, current);
    auto request = input(current);
    request.ancestors.clear();
    const auto measured = luciole::buildMeasuredTree(request, *cache.view());
    const auto& tree = measured.tree;
    checkLinks(tree);
    TEST_CHECK(measured.remainders.size() == 2);
    TEST_CHECK((names(tree, tree.root) == std::vector<std::string>{"", "", "f1.txt"}));
    for (const auto id : measured.remainders) {
      const auto& node = tree.nodes[id];
      TEST_CHECK(node.parent == tree.root && node.name.empty() && node.childCount == 0);
      if (node.folder) {
        TEST_CHECK(node.items == 1 && node.bytes == 326 && node.files == 4);
      } else {
        TEST_CHECK(node.items == 1 && node.files == 1 && node.bytes == 50);
      }
    }

    request.levels = 1;
    request.maxNodes = 2;
    const auto capped = luciole::buildMeasuredTree(request, *cache.view()).tree;
    TEST_CHECK(capped.nodes.size() == 1 && capped.nodes[capped.root].truncated);
  }

  // A file remainder's items count every folded name; its files leave out a later name of a hard-linked file.
  void foldedHardLink(const fs::path& base) {
    const auto folder = base / "linked";
    fs::create_directories(folder);
    writeFile(folder / "f", 300);
    fs::create_hard_link(folder / "f", folder / "g");
    luciole::MeasureServiceOptions options;
    options.walk.fileCap = 0;
    SubtreeMeasureService service(options);
    MeasureCache cache(service);
    settle(cache, folder);
    const auto measured = luciole::buildMeasuredTree(
        {.current = at(folder), .entries = {}, .ancestors = {}, .visits = {}, .now = 0}, *cache.view()
    );
    TEST_CHECK(measured.remainders.size() == 1);
    const auto& rest = measured.tree.nodes[measured.remainders[0]];
    TEST_CHECK(!rest.folder && rest.items == 2 && rest.files == 1 && rest.bytes == 300);
  }

  luciole::workbench::Tree measuredAlone(const fs::path& folder, const luciole::MeasureServiceOptions& options) {
    SubtreeMeasureService service(options);
    MeasureCache cache(service);
    settle(cache, folder);
    const MeasuredTreeInput request{.current = at(folder), .entries = {}, .ancestors = {}, .visits = {}, .now = 0};
    return luciole::buildMeasuredTree(request, *cache.view()).tree;
  }

  void flags(const fs::path& base) {
    const auto root = base / "flags";
    fs::create_directories(root / "locked");
    fs::permissions(root / "locked", fs::perms::none);
    const auto open = measuredAlone(root, {});
    luciole::MeasureServiceOptions small;
    small.walk.entryBudget = 1;
    const auto limited = measuredAlone(root, small);
    fs::permissions(root / "locked", fs::perms::owner_all);
    const bool blocked = ::geteuid() != 0; // permission bits do not stop the superuser
    TEST_CHECK(open.nodes[open.root].lowerBound == blocked && limited.nodes[limited.root].lowerBound);
    TEST_CHECK(limited.nodes[named(limited, limited.root, "locked")].lowerBound);
    if (blocked) {
      const auto& locked = open.nodes[named(open, open.root, "locked")];
      TEST_CHECK(locked.unreadable && locked.lowerBound && open.nodes[open.root].lowerBound);
    }

    // The filesystem root's listing alone: /proc is another filesystem.
    const auto count =
        static_cast<std::uint64_t>(std::ranges::distance(fs::directory_iterator("/"), fs::directory_iterator()));
    luciole::MeasureServiceOptions top;
    top.walk.entryBudget = count;
    top.walk.detailDepth = 1;
    const auto filesystem = measuredAlone("/", top);
    TEST_CHECK(filesystem.nodes[filesystem.root].name == "/");
    TEST_CHECK(filesystem.nodes[named(filesystem, filesystem.root, "proc")].otherFilesystem);
  }

  // Holds the walk after its first partial table, so the walk is certainly running while the table is read.
  struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool reached = false;
    bool open = false;
    bool timedOut = false;

    void hold() {
      std::unique_lock lock(mutex);
      if (std::exchange(reached, true)) {
        return;
      }
      timedOut = !changed.wait_for(lock, std::chrono::seconds(20), [this] { return open; });
    }

    void release() {
      const std::scoped_lock lock(mutex);
      open = true;
      changed.notify_all();
    }
  };

  // A running walk is seen as a partial record that is not a lower bound; once released, it finishes exact.
  void runningRecord(const fs::path& base) {
    const auto root = base / "running";
    fs::create_directories(root / "inner");
    writeFile(root / "inner" / "x", 40);
    writeFile(root / "y", 2);
    Gate gate;
    luciole::MeasureServiceOptions options;
    options.publishInterval = std::chrono::milliseconds(0); // every finished folder publishes
    options.afterPartial = [&gate] { gate.hold(); };
    SubtreeMeasureService service(options);
    MeasureCache cache(service);
    pumpUntil([&] {
      const auto found = cache.measure(at(root), MeasurePriority::Current);
      const auto* record = found.entry();
      return record != nullptr && record->flags.partial;
    });
    const auto found = cache.measure(at(root), MeasurePriority::Current);
    const auto tree = luciole::buildMeasuredTree(
        {.current = at(root), .entries = {}, .ancestors = {}, .visits = {}, .now = 0}, *cache.view()
    );
    gate.release();
    TEST_CHECK(!gate.timedOut && found.measuring);
    TEST_CHECK(tree.tree.nodes[tree.tree.root].partial && !tree.tree.nodes[tree.tree.root].lowerBound);
    settle(cache, root);
    const auto done = cache.find(at(root));
    TEST_CHECK(done.entry() != nullptr && !done.entry()->flags.partial && !done.entry()->flags.truncated);
    TEST_CHECK(done.entry()->totals.apparentBytes == 42);
  }

  // A running walk is partial and not a lower bound; a finished cut-short walk is a lower bound and not partial; a
  // finished full walk is neither; a folder the cut-short walk never entered is a lower bound of zero.
  void measureStates(const fs::path& base) {
    const auto root = base / "states";
    fs::create_directories(root / "inner");
    writeFile(root / "inner" / "x", 40);
    const auto full = measuredAlone(root, {});
    TEST_CHECK(!full.nodes[full.root].partial && !full.nodes[full.root].lowerBound);
    TEST_CHECK(!full.nodes[named(full, full.root, "inner")].lowerBound);

    luciole::MeasureServiceOptions cut;
    cut.walk.entryBudget = 1;
    const auto limited = measuredAlone(root, cut);
    TEST_CHECK(limited.nodes[limited.root].lowerBound && !limited.nodes[limited.root].partial);
    const auto& inner = limited.nodes[named(limited, limited.root, "inner")];
    TEST_CHECK(inner.folder && inner.lowerBound && !inner.partial && inner.bytes == 0 && inner.items == 0);

    runningRecord(base);
  }
} // namespace

int main() {
  const Scratch scratch;
  const auto current = fixture(scratch.path());
  measuredCurrent(current);
  listedOnly(current);
  listedWithMeasuredChild(current);
  insideAncestorTable(current);
  remaindersAndLimits(current);
  symlinkedCurrent(scratch.path());
  foldedCurrent(scratch.path());
  foldedHardLink(scratch.path());
  flags(scratch.path());
  measureStates(scratch.path());
  return 0;
}
