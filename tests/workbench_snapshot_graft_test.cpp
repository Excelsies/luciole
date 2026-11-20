#include "browser/measure_cache.h"
#include "browser/measured_tree.h"
#include "browser/workbench_snapshot.h"
#include "fs/subtree_measure.h"
#include "temp_folder.h"
#include "test_check.h"
#include "workbench/measure.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glib.h>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

// A refreshed child grafted into a snapshot decides the root's state together with the root's own record, and a
// snapshot that differs only in a state flag is still published.
namespace {
  namespace fs = std::filesystem;
  namespace wb = luciole::workbench;
  using luciole::Location;
  using luciole::MeasureCache;
  using luciole::MeasurePriority;
  using luciole::SubtreeMeasureService;

  struct Scratch : luciole::test::TempFolder {
    Scratch() : TempFolder("luciole-snapshot-graft") {}
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

  Location at(const fs::path& path) { return Location::fromPath(path.native()); }

  void writeFile(const fs::path& path, std::size_t bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << std::string(bytes, 'x');
  }

  void settle(MeasureCache& cache, const fs::path& folder) {
    pumpUntil([&] {
      const auto found = cache.measure(at(folder), MeasurePriority::Current);
      return found.entry() != nullptr && !found.measuring && !found.stale;
    });
  }

  // root/{a/{b/{x 100}, y 50}, c/{z 30}, top.txt 7}
  fs::path fixture(const fs::path& base) {
    const auto root = base / "root";
    writeFile(root / "a" / "b" / "x", 100);
    writeFile(root / "a" / "y", 50);
    writeFile(root / "c" / "z", 30);
    writeFile(root / "top.txt", 7);
    return root;
  }

  // The child's two levels as a cache with the given walk limits measures them, or as an empty cache knows them.
  std::shared_ptr<const luciole::MeasuredTree>
  childTree(const fs::path& child, const luciole::MeasureServiceOptions& options, bool measured) {
    SubtreeMeasureService service(options);
    MeasureCache cache(service);
    if (measured) {
      settle(cache, child);
    }
    return std::make_shared<const luciole::MeasuredTree>(luciole::buildMeasuredTree(
        {.current = at(child), .entries = {}, .ancestors = {}, .visits = {}, .now = 0, .levels = 2}, *cache.view()
    ));
  }

  // The root's snapshot with `a` refreshed from `built`, or not refreshed when null.
  std::shared_ptr<const luciole::WorkbenchSnapshot>
  snapshotWith(const fs::path& root, const std::shared_ptr<const luciole::MeasuredTree>& built) {
    SubtreeMeasureService service;
    MeasureCache cache(service);
    settle(cache, root);
    luciole::WorkbenchBuildInput input{
        .view = cache.view(),
        .location = at(root),
        .ancestors = nullptr,
        .children = {},
        .error = std::nullopt,
        .folderFinished = true,
        .now = 0,
    };
    if (built) {
      input.children.push_back({.name = "a", .location = at(root / "a"), .table = nullptr, .built = built});
    }
    const auto draft = luciole::buildDraft(input);
    TEST_CHECK(draft.has_value());
    auto snapshot = luciole::finishSnapshot(*draft, nullptr);
    TEST_CHECK(snapshot != nullptr);
    return snapshot;
  }

  const wb::Node& rootOf(const luciole::WorkbenchSnapshot& snapshot) { return snapshot.tree.nodes[snapshot.tree.root]; }

  const wb::Node& childOf(const luciole::WorkbenchSnapshot& snapshot, const std::string& name) {
    for (const auto id : wb::children(snapshot.tree, snapshot.tree.root)) {
      if (snapshot.tree.nodes[id].name == name) {
        return snapshot.tree.nodes[id];
      }
    }
    TEST_CHECK(false);
    return rootOf(snapshot);
  }

  // Exact root, then `a` refreshed under a budget: the root is a lower bound with its child's flag intact; then `a`
  // refreshed in full: the root is exact again, because every graft starts from the root's own record.
  void lowerBoundChild(const fs::path& root) {
    const auto plain = snapshotWith(root, nullptr);
    TEST_CHECK(wb::measureState(rootOf(*plain)) == wb::MeasureState::Done);
    TEST_CHECK(wb::measureLabel(plain->tree, plain->tree.root, luciole::WorkbenchMeasure::Bytes) == "187 bytes");

    luciole::MeasureServiceOptions cut;
    cut.walk.entryBudget = 1;
    const auto limited = snapshotWith(root, childTree(root / "a", cut, true));
    TEST_CHECK(childOf(*limited, "a").lowerBound && !childOf(*limited, "a").partial);
    TEST_CHECK(rootOf(*limited).lowerBound && !rootOf(*limited).partial);
    const auto label = wb::measureLabel(limited->tree, limited->tree.root, luciole::WorkbenchMeasure::Bytes);
    TEST_CHECK(label.starts_with("at least "));

    const auto full = snapshotWith(root, childTree(root / "a", {}, true));
    TEST_CHECK(!childOf(*full, "a").lowerBound);
    TEST_CHECK(wb::measureState(rootOf(*full)) == wb::MeasureState::Done);
  }

  void measuringChild(const fs::path& root) {
    const auto snapshot = snapshotWith(root, childTree(root / "a", {}, false));
    TEST_CHECK(childOf(*snapshot, "a").partial);
    TEST_CHECK(wb::measureState(rootOf(*snapshot)) == wb::MeasureState::Measuring);
  }

  // A tree that differs from the previous one only in the root's lowerBound flag is published; an equal one is not.
  void flagOnlyChange(const fs::path& root) {
    SubtreeMeasureService service;
    MeasureCache cache(service);
    settle(cache, root);
    const luciole::WorkbenchBuildInput input{
        .view = cache.view(),
        .location = at(root),
        .ancestors = nullptr,
        .children = {},
        .error = std::nullopt,
        .folderFinished = true,
        .now = 0,
    };
    const auto draft = luciole::buildDraft(input);
    TEST_CHECK(draft.has_value());
    const auto first = luciole::finishSnapshot(*draft, nullptr);
    TEST_CHECK(first && luciole::finishSnapshot(*draft, first) == nullptr);

    auto flagged = *draft;
    auto tree = *draft->measured;
    tree.tree.nodes[tree.tree.root].lowerBound = true;
    flagged.measured = std::make_shared<const luciole::MeasuredTree>(std::move(tree));
    const auto next = luciole::finishSnapshot(flagged, first);
    TEST_CHECK(next && next->serial == 2 && rootOf(*next).lowerBound);
  }

  // A folder that cannot be read is "not measured" and hatched; every folder above it reads "at least", through the
  // snapshot the views use. A readable folder beside it stays exact.
  void unreadableFolder(const fs::path& base) {
    const auto root = base / "unreadable";
    writeFile(root / "a" / "locked" / "inside", 40);
    writeFile(root / "a" / "ok", 10);
    writeFile(root / "fine" / "f", 3);
    fs::permissions(root / "a" / "locked", fs::perms::none);
    const auto snapshot = snapshotWith(root, nullptr);
    fs::permissions(root / "a" / "locked", fs::perms::owner_all);
    if (::geteuid() == 0) {
      return; // Permission bits do not stop the superuser.
    }
    const auto bytes = [&](wb::NodeId id) {
      return wb::measureLabel(snapshot->tree, id, luciole::WorkbenchMeasure::Bytes);
    };
    const auto& tree = snapshot->tree;
    const auto a = childOf(*snapshot, "a");
    wb::NodeId aId = wb::kNoNode;
    wb::NodeId lockedId = wb::kNoNode;
    wb::NodeId fineId = wb::kNoNode;
    for (const auto id : wb::children(tree, tree.root)) {
      aId = tree.nodes[id].name == "a" ? id : aId;
      fineId = tree.nodes[id].name == "fine" ? id : fineId;
    }
    for (const auto id : wb::children(tree, aId)) {
      lockedId = tree.nodes[id].name == "locked" ? id : lockedId;
    }
    TEST_CHECK(lockedId != wb::kNoNode && fineId != wb::kNoNode);
    TEST_CHECK(wb::measureState(tree.nodes[lockedId]) == wb::MeasureState::LowerBound);
    TEST_CHECK(bytes(lockedId) == "not measured");
    TEST_CHECK(a.lowerBound && bytes(aId).starts_with("at least "));
    TEST_CHECK(rootOf(*snapshot).lowerBound && bytes(tree.root) == "at least 13 bytes");
    TEST_CHECK(wb::measureState(tree.nodes[fineId]) == wb::MeasureState::Done && bytes(fineId) == "3 bytes");
  }
} // namespace

int main() {
  const Scratch scratch;
  const auto root = fixture(scratch.path());
  lowerBoundChild(root);
  measuringChild(root);
  flagOnlyChange(root);
  unreadableFolder(scratch.path());
  return 0;
}
