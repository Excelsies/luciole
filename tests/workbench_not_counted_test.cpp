#include "test_check.h"
#include "workbench/not_counted.h"
#include "workbench_fixture.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The sentence that says why an amount reads "at least": the folders the snapshot flags, at most three by name, the
// general sentence for what it cannot place, and none for an exact amount.
namespace {
  using namespace luciole::workbench;
  using luciole::test::WorkbenchRow;
  using luciole::test::workbenchTree;

  constexpr const char* kGeneral = "Not counted: some folders below were not counted in full.";

  // Flags a node the way a measured snapshot does for a folder that was not counted in full.
  void cut(Tree& tree, NodeId id) { tree.nodes[id].lowerBound = true; }
  void mountPoint(Tree& tree, NodeId id) {
    tree.nodes[id].otherFilesystem = true;
    tree.nodes[id].truncated = true;
    cut(tree, id);
  }
  void locked(Tree& tree, NodeId id) {
    tree.nodes[id].unreadable = true;
    cut(tree, id);
  }

  // "/" holding the named folders, each with a gigabyte; ids: 0 "/", then 1.. in the order given.
  Tree rootWith(const std::vector<std::string_view>& names) {
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "/", .folder = true, .bytes = 1e9, .items = 3}};
    for (const auto name : names) {
      rows.push_back({.parent = 0, .name = name, .folder = true, .bytes = 1e9, .items = 1});
    }
    auto tree = workbenchTree(rows, 0);
    cut(tree, 0);
    return tree;
  }

  void mountsAreNamedOnce() {
    auto tree = rootWith({"proc", "sys", "dev", "usr"});
    mountPoint(tree, 1);
    mountPoint(tree, 2);
    mountPoint(tree, 3);
    TEST_CHECK(notCountedText(tree, 0) == "Not counted: proc, sys and dev are on another filesystem.");
    auto two = rootWith({"proc", "sys", "usr"});
    mountPoint(two, 1);
    mountPoint(two, 2);
    TEST_CHECK(notCountedText(two, 0) == "Not counted: proc and sys are on another filesystem.");
    auto one = rootWith({"proc", "usr"});
    mountPoint(one, 1);
    TEST_CHECK(notCountedText(one, 0) == "Not counted: proc is on another filesystem.");
  }

  void unreadableFoldersAreCounted() {
    auto tree = rootWith({"cache", "lost+found", "usr"});
    locked(tree, 1);
    locked(tree, 2);
    TEST_CHECK(notCountedText(tree, 0) == "Not counted: 2 unreadable folders (cache, lost+found).");
    auto one = rootWith({"cache", "usr"});
    locked(one, 1);
    TEST_CHECK(notCountedText(one, 0) == "Not counted: 1 unreadable folder (cache).");
  }

  void moreThanThreeNamesTheFirstThree() {
    auto tree = rootWith({"a", "b", "c", "d", "e", "usr"});
    for (NodeId id = 1; id <= 5; ++id) {
      mountPoint(tree, id);
    }
    TEST_CHECK(notCountedText(tree, 0) == "Not counted: a, b, c and 2 more are on another filesystem.");
    auto locks = rootWith({"a", "b", "c", "d"});
    for (NodeId id = 1; id <= 4; ++id) {
      locked(locks, id);
    }
    TEST_CHECK(notCountedText(locks, 0) == "Not counted: 4 unreadable folders (a, b, c and 1 more).");
  }

  void kindsAreToldTogether() {
    auto tree = rootWith({"proc", "cache", "usr"});
    mountPoint(tree, 1);
    locked(tree, 2);
    TEST_CHECK(notCountedText(tree, 0) == "Not counted: proc is on another filesystem; 1 unreadable folder (cache).");
  }

  // Folders nested below the node are found through the lower bounds on the way down.
  void foldersBelowAreFound() {
    auto tree = workbenchTree(
        {{.parent = -1, .name = "r", .folder = true, .bytes = 5e9, .items = 2},
         {.parent = 0, .name = "mnt", .folder = true, .bytes = 4e9, .items = 2},
         {.parent = 1, .name = "usb", .folder = true},
         {.parent = 1, .name = "data", .folder = true, .bytes = 4e9, .items = 1},
         {.parent = 0, .name = "home", .folder = true, .bytes = 1e9, .items = 1},
         {.parent = 4, .name = "me", .folder = true, .bytes = 1e9}},
        0
    );
    cut(tree, 0);
    cut(tree, 1);
    mountPoint(tree, 2);
    TEST_CHECK(notCountedText(tree, 0) == "Not counted: usb is on another filesystem.");
    TEST_CHECK(notCountedText(tree, 1) == "Not counted: usb is on another filesystem.");
  }

  // The node's own flag counts, so a mount point names itself.
  void aMountPointNamesItself() {
    auto tree = rootWith({"proc", "usr"});
    mountPoint(tree, 1);
    TEST_CHECK(notCountedText(tree, 1) == "Not counted: proc is on another filesystem.");
    TEST_CHECK(notCountedText(tree, 2).empty());
  }

  void whatTheSnapshotCannotPlaceIsToldGenerally() {
    auto tree = rootWith({"big", "usr"});
    TEST_CHECK(notCountedText(tree, 0) == kGeneral);
    cut(tree, 1);
    TEST_CHECK(notCountedText(tree, 0) == kGeneral);
    // A folder named beside one that cannot be placed says both.
    auto both = rootWith({"proc", "big", "usr"});
    mountPoint(both, 1);
    cut(both, 2);
    TEST_CHECK(
        notCountedText(both, 0)
        == "Not counted: proc is on another filesystem; some folders below were not counted "
           "in full."
    );
  }

  void anExactAmountHasNoSentence() {
    auto tree = rootWith({"proc"});
    mountPoint(tree, 1);
    tree.nodes[0].lowerBound = false;
    TEST_CHECK(notCountedText(tree, 0).empty());
    tree.nodes[0].lowerBound = true;
    tree.nodes[0].partial = true; // still measuring: the amount says so itself
    TEST_CHECK(notCountedText(tree, 0).empty());
    TEST_CHECK(notCountedText(tree, kNoNode).empty());
    TEST_CHECK(notCountedText(tree, 99).empty());
  }

  // A size that reads "not measured" has the same reason.
  void notMeasuredHasTheSameReason() {
    auto tree = rootWith({"proc"});
    mountPoint(tree, 1);
    tree.nodes[1].bytes = 0;
    TEST_CHECK(notCountedText(tree, 1) == "Not counted: proc is on another filesystem.");
  }

  void severalNodesAreSummed() {
    auto tree = rootWith({"proc", "sys", "usr", "cache"});
    mountPoint(tree, 1);
    mountPoint(tree, 2);
    locked(tree, 4);
    const std::vector<NodeId> merged{1, 2, 3, 4};
    TEST_CHECK(
        notCountedText(tree, merged)
        == "Not counted: proc and sys are on another filesystem; 1 unreadable folder "
           "(cache)."
    );
    const std::vector<NodeId> exact{3};
    TEST_CHECK(notCountedText(tree, exact).empty());
    TEST_CHECK(notCountedText(tree, std::vector<NodeId>{}).empty());
  }

  // A walk that reaches the cap says only the general sentence: what it found may be a part of the whole.
  void theCapGivesTheGeneralSentence() {
    auto tree = rootWith({"proc", "a", "b", "c", "d", "e"});
    for (NodeId id = 1; id <= 6; ++id) {
      mountPoint(tree, id);
    }
    TEST_CHECK(notCountedText(tree, 0, 7) == "Not counted: proc, a, b and 3 more are on another filesystem.");
    TEST_CHECK(notCountedText(tree, 0, 6) == kGeneral);
    TEST_CHECK(notCountedText(tree, 0, 1) == kGeneral);
  }

  // Folders that are exact are never entered, so a huge exact subtree costs nothing against the cap.
  void exactSubtreesAreNotVisited() {
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "r", .folder = true, .bytes = 1e9, .items = 2},
        {.parent = 0, .name = "proc", .folder = true},
        {.parent = 0, .name = "big", .folder = true, .bytes = 1e9, .items = 500},
    };
    for (std::size_t i = 0; i < 500; ++i) {
      rows.push_back({.parent = 2, .name = "f", .bytes = 10});
    }
    auto tree = workbenchTree(rows, 0);
    cut(tree, 0);
    mountPoint(tree, 1);
    TEST_CHECK(notCountedText(tree, 0, 5) == "Not counted: proc is on another filesystem.");
  }

  // A folder above the open one is measured under a limit its flags do not show: no list of folders, only what is
  // true of every such folder.
  void aFolderAboveTheOpenOneSaysSo() {
    auto tree = workbenchTree(
        {{.parent = -1, .name = "/", .folder = true, .bytes = 1e9, .items = 2},
         {.parent = 0, .name = "home", .folder = true, .bytes = 1e9, .items = 1},
         {.parent = 1, .name = "secret", .folder = true, .unreadable = true, .truncated = true}},
        0
    );
    cut(tree, 0);
    cut(tree, 1);
    locked(tree, 2);
    tree.root = 1;
    tree.ancestors = {0};
    TEST_CHECK(
        notCountedText(tree, 0)
        == "Not counted: measuring this folder stopped at a limit, at another filesystem or at a folder that cannot "
           "be read."
    );
    // The open folder and what is below it are walked in full, so they name their folders.
    TEST_CHECK(notCountedText(tree, 1) == "Not counted: 1 unreadable folder (secret).");
    // An ancestor that reads exact has none.
    tree.nodes[0].lowerBound = false;
    TEST_CHECK(notCountedText(tree, 0).empty());
  }

  // A flagged folder is named and its lower-bound children are still followed: the cause below it is found too.
  void aFlaggedFolderIsFollowedBelow() {
    auto tree = workbenchTree(
        {{.parent = -1, .name = "r", .folder = true, .bytes = 1e9, .items = 1},
         {.parent = 0, .name = "locked", .folder = true, .bytes = 1e9, .items = 1},
         {.parent = 1, .name = "inner", .folder = true, .bytes = 1e9}},
        0
    );
    cut(tree, 0);
    locked(tree, 1);
    mountPoint(tree, 2);
    TEST_CHECK(notCountedText(tree, 0) == "Not counted: inner is on another filesystem; 1 unreadable folder (locked).");
  }

  // A hundred thousand lower-bound children are more than any cap: the answer is the general sentence, whole and
  // quickly, whatever the cap.
  void aWideFolderGivesTheGeneralSentence() {
    // Built directly: the fixture helper compares every row with every parent.
    Tree tree;
    constexpr std::uint32_t kKids = 100000;
    tree.nodes.resize(kKids + 1);
    tree.nodes[0] = {
        .name = "r", .firstChild = 0, .childCount = kKids, .bytes = 1e9, .folder = true, .lowerBound = true
    };
    for (std::uint32_t i = 1; i <= kKids; ++i) {
      tree.nodes[i] = {
          .name = "m", .parent = 0, .folder = true, .lowerBound = true, .truncated = true, .otherFilesystem = true
      };
      tree.children.push_back(i);
    }
    tree.root = 0;
    const auto before = std::chrono::steady_clock::now();
    TEST_CHECK(notCountedText(tree, 0, 10) == kGeneral);
    TEST_CHECK(notCountedText(tree, 0) == kGeneral);
    TEST_CHECK(notCountedText(tree, 0, 100000) == kGeneral);
    // Room for all of them reads and names them.
    TEST_CHECK(notCountedText(tree, 0, 100001) == "Not counted: m, m, m and 99997 more are on another filesystem.");
    // A small cap stops at the cap, not at the fan-out: five thousand such calls read ten nodes each. Queueing every
    // child first would cost five hundred million pushes, far beyond the guard.
    for (int call = 0; call < 5000; ++call) {
      TEST_CHECK(notCountedText(tree, 0, 10) == kGeneral);
    }
    // A hang guard, not a speed claim.
    TEST_CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));
  }
} // namespace

int main() {
  mountsAreNamedOnce();
  unreadableFoldersAreCounted();
  moreThanThreeNamesTheFirstThree();
  kindsAreToldTogether();
  foldersBelowAreFound();
  aMountPointNamesItself();
  whatTheSnapshotCannotPlaceIsToldGenerally();
  anExactAmountHasNoSentence();
  notMeasuredHasTheSameReason();
  severalNodesAreSummed();
  theCapGivesTheGeneralSentence();
  exactSubtreesAreNotVisited();
  aFolderAboveTheOpenOneSaysSo();
  aFlaggedFolderIsFollowedBelow();
  aWideFolderGivesTheGeneralSentence();
  return 0;
}
