#include "browser/measured_tree.h"
#include "temp_folder.h"
#include "test_check.h"
#include "workbench/entry_text.h"
#include "workbench/nest_labels.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glib.h>
#include <print>
#include <string>
#include <thread>
#include <unistd.h>

// The Nest facts of folders from real walks: "not expanded" is for a folder whose entries are left out of the
// snapshot, never for a folder that is expanded and holds something that was not counted.
namespace {
  namespace fs = std::filesystem;
  using luciole::Location;
  using luciole::MeasureCache;
  using luciole::MeasurePriority;
  using luciole::SubtreeMeasureService;
  using luciole::workbench::kNoNode;
  using luciole::workbench::NodeId;
  using luciole::workbench::Tree;

  struct Scratch : luciole::test::TempFolder {
    Scratch() : TempFolder("luciole-tree-labels") {}
  };

  Location at(const fs::path& path) { return Location::fromPath(path.native()); }

  void writeFile(const fs::path& path, std::size_t bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');
    TEST_CHECK(out.good());
  }

  void pumpUntil(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  Tree measuredAlone(const fs::path& folder, const luciole::MeasureServiceOptions& options) {
    SubtreeMeasureService service(options);
    MeasureCache cache(service);
    pumpUntil([&] {
      const auto found = cache.measure(at(folder), MeasurePriority::Current);
      return found.entry() != nullptr && !found.measuring && !found.stale;
    });
    const luciole::MeasuredTreeInput request{
        .current = at(folder), .entries = {}, .ancestors = {}, .visits = {}, .now = 0
    };
    return luciole::buildMeasuredTree(request, *cache.view()).tree;
  }

  NodeId named(const Tree& tree, NodeId parent, const std::string& name) {
    for (const auto id : luciole::workbench::children(tree, parent)) {
      if (tree.nodes[id].name == name) {
        return id;
      }
    }
    return kNoNode;
  }

  std::size_t count(const std::string& text, const std::string& part) {
    std::size_t found = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) {
      ++found;
    }
    return found;
  }

  // root/{mid/{locked/{inside 40}, ok 10}, fine/{f 3}}, with locked mode 000.
  void skippedChild(const fs::path& base) {
    const auto root = base / "skipped";
    writeFile(root / "mid" / "locked" / "inside", 40);
    writeFile(root / "mid" / "ok", 10);
    writeFile(root / "fine" / "f", 3);
    if (::geteuid() == 0) {
      std::println("skipped: a folder with a skipped child (permission bits do not stop the superuser)");
      return;
    }
    fs::permissions(root / "mid" / "locked", fs::perms::none);
    const auto tree = measuredAlone(root, {});
    fs::permissions(root / "mid" / "locked", fs::perms::owner_all);
    const auto mid = named(tree, tree.root, "mid");
    const auto locked = named(tree, mid, "locked");
    TEST_CHECK(mid != kNoNode && locked != kNoNode);
    for (const auto id : {tree.root, mid}) {
      const auto facts = luciole::workbench::entryText(entryFacts(tree, id));
      TEST_CHECK(facts.contains("at least") && !facts.contains("not expanded"));
      TEST_CHECK(!tree.nodes[id].truncated && tree.nodes[id].lowerBound);
      TEST_CHECK(luciole::workbench::folderState(tree.nodes[id]) == luciole::workbench::FolderState::Listed);
    }
    const auto facts = luciole::workbench::entryText(entryFacts(tree, locked));
    TEST_CHECK(count(facts, "not measured") == 1);
    // The sentence from a real walk names the one folder it could not read, for the folders above it as well.
    for (const auto id : {tree.root, mid, locked}) {
      TEST_CHECK(
          luciole::workbench::entryText(entryFacts(tree, id)).ends_with("\nNot counted: 1 unreadable folder (locked).")
      );
    }
  }

  // root/{mid/{deep/{x 5}}} walked one level deep: mid's folder has no record, so mid is not expanded.
  void belowDetailDepth(const fs::path& base) {
    const auto root = base / "shallow";
    writeFile(root / "mid" / "deep" / "x", 5);
    luciole::MeasureServiceOptions options;
    options.walk.detailDepth = 1;
    const auto tree = measuredAlone(root, options);
    const auto mid = named(tree, tree.root, "mid");
    TEST_CHECK(mid != kNoNode && tree.nodes[mid].childCount == 0);
    TEST_CHECK(luciole::workbench::entryText(entryFacts(tree, mid)).contains("not expanded"));
    TEST_CHECK(!luciole::workbench::entryText(entryFacts(tree, tree.root)).contains("not expanded"));
  }

  // root/{inner/{x 5}} with an entry budget that stops the walk before inner is read: inner has no entries.
  void cutWithNoEntries(const fs::path& base) {
    const auto root = base / "cut";
    writeFile(root / "inner" / "x", 5);
    luciole::MeasureServiceOptions options;
    options.walk.entryBudget = 1;
    const auto tree = measuredAlone(root, options);
    const auto inner = named(tree, tree.root, "inner");
    TEST_CHECK(inner != kNoNode && tree.nodes[inner].childCount == 0);
    TEST_CHECK(tree.nodes[inner].truncated && tree.nodes[inner].lowerBound);
    TEST_CHECK(luciole::workbench::folderState(tree.nodes[inner]) == luciole::workbench::FolderState::NotExpanded);
    TEST_CHECK(luciole::workbench::entryText(entryFacts(tree, inner)).contains("not measured"));
    TEST_CHECK(!tree.nodes[tree.root].truncated && tree.nodes[tree.root].lowerBound);
  }

  // top/{usr/{a}, big/{f1..f5}} with usr open and top walked as an ancestor under a budget of two entries. The ancestor
  // is a measured record the budget cut short, not a folder known by its listing: it reads "not measured", and its
  // tooltip says it lies above the open folder, names no folder and has no share.
  void ancestorCutByItsBudget(const fs::path& base) {
    const auto top = base / "ancestor" / "top";
    writeFile(top / "usr" / "a", 3);
    for (const auto* name : {"f1", "f2", "f3", "f4", "f5"}) {
      writeFile(top / "big" / name, 7);
    }
    luciole::MeasureServiceOptions options;
    options.ancestorBudget = 2;
    SubtreeMeasureService service(options);
    MeasureCache cache(service);
    pumpUntil([&] {
      const auto current = cache.measure(at(top / "usr"), MeasurePriority::Current);
      const auto above = cache.measure(at(top), MeasurePriority::Ancestor);
      return current.entry() != nullptr && !current.measuring && above.entry() != nullptr && !above.measuring;
    });
    const luciole::MeasuredTreeInput request{
        .current = at(top / "usr"),
        .entries = {},
        .ancestors = {{.location = at(top), .entries = {}}},
        .visits = {},
        .now = 0
    };
    const auto tree = luciole::buildMeasuredTree(request, *cache.view()).tree;
    TEST_CHECK(tree.ancestors.size() == 1);
    const auto above = tree.ancestors.front();
    const auto& node = tree.nodes[above];
    TEST_CHECK(!node.partial && node.lowerBound);
    const luciole::workbench::LayoutSettings settings;
    const auto facts = luciole::workbench::entryFacts(tree, above, &settings);
    TEST_CHECK(facts.share.empty());
    TEST_CHECK(
        facts.notCounted
        == "Not counted: measuring this folder stopped at a limit, at another filesystem or at a folder that cannot be "
           "read."
    );
    // The open folder was walked in full and has none.
    TEST_CHECK(luciole::workbench::entryFacts(tree, tree.root).notCounted.empty());
  }
} // namespace

int main() {
  const Scratch scratch;
  skippedChild(scratch.path());
  belowDetailDepth(scratch.path());
  cutWithNoEntries(scratch.path());
  ancestorCutByItsBudget(scratch.path());
  return 0;
}
