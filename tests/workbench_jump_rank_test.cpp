#include "cpu_budget.h"
#include "test_check.h"
#include "workbench/jump_rank.h"
#include "workbench_fixture.h"

#include <cstdint>
#include <ctime>
#include <format>
#include <limits>
#include <print>
#include <string>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using luciole::WorkbenchMeasure;
  using luciole::test::referenceTree;
  using luciole::test::workbenchNames;
  using luciole::test::workbenchTree;
  using Names = std::vector<std::string>;

  std::vector<NodeId> rowIds(const JumpList& list) {
    std::vector<NodeId> ids;
    ids.reserve(list.rows.size());
    for (const auto& row : list.rows) {
      ids.push_back(row.id);
    }
    return ids;
  }

  Names rowNames(const Tree& tree, const JumpList& list) { return workbenchNames(tree, rowIds(list)); }

  Names ranked(const Tree& tree, std::size_t count = kJumpRows) {
    return rowNames(tree, jumpAhead(tree, LayoutSettings{}, {.count = count}));
  }

  // Ids: 0 r, 1 a, 2 b.txt, 3 .hidden, 4 c, 5 x, 6 y.png, 7 deep, 8 f.log, 9 g.c, 10 z.
  Tree treeWithZ(double zVisits) {
    auto tree = referenceTree();
    const auto z = static_cast<NodeId>(tree.nodes.size());
    Node node;
    node.name = "z";
    node.parent = 1;
    node.folder = true;
    node.bytes = 100;
    node.visits = zVisits;
    tree.nodes.push_back(node);
    // The children of a move to the end of the list, with z after them.
    const auto kids = children(tree, 1);
    std::vector<NodeId> moved(kids.begin(), kids.end());
    moved.push_back(z);
    tree.nodes[1].firstChild = static_cast<std::uint32_t>(tree.children.size());
    tree.nodes[1].childCount = static_cast<std::uint32_t>(moved.size());
    tree.children.insert(tree.children.end(), moved.begin(), moved.end());
    return tree;
  }

  void testDeepFoldersByVisitsThenSize() {
    auto tree = referenceTree();
    TEST_CHECK((ranked(tree, 10) == Names{"x", "deep"})); // both 5 opens, x is larger
    TEST_CHECK((ranked(tree, 1) == Names{"x"}));
    tree.root = 1;
    TEST_CHECK((ranked(tree, 10) == Names{"deep"}));
    tree.root = 5;
    TEST_CHECK(ranked(tree, 10).empty());
  }

  void testVisitsChangeTheRanking() {
    TEST_CHECK((ranked(treeWithZ(0)) == Names{"x", "deep", "z"}));
    TEST_CHECK((ranked(treeWithZ(6)) == Names{"z", "x", "deep"})); // 6 opens outrank 5
  }

  void testRankingIgnoresTheSizeMeasure() {
    const auto tree = referenceTree();
    for (const auto measure : {WorkbenchMeasure::Items, WorkbenchMeasure::Modified, WorkbenchMeasure::Equal}) {
      LayoutSettings settings;
      settings.sizeBy = measure;
      TEST_CHECK((rowNames(tree, jumpAhead(tree, settings)) == Names{"x", "deep"}));
    }
  }

  // r > a > {.cache > {inner}, kept > {deeper}}
  Tree hiddenTree() {
    return workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true},
            {.parent = 0, .name = "a", .folder = true},
            {.parent = 1, .name = ".cache", .folder = true, .bytes = 900},
            {.parent = 1, .name = "kept", .folder = true, .bytes = 10},
            {.parent = 2, .name = "inner", .folder = true, .bytes = 800},
            {.parent = 3, .name = "deeper", .folder = true, .bytes = 5},
        },
        0
    );
  }

  void testHiddenFoldersAndWhatIsBelowThem() {
    const auto tree = hiddenTree();
    TEST_CHECK((ranked(tree) == Names{"kept", "deeper"}));
    LayoutSettings settings;
    settings.showHidden = true;
    TEST_CHECK((rowNames(tree, jumpAhead(tree, settings)) == Names{".cache", "inner", "kept", "deeper"}));
  }

  void testTiesFallToThePathInByteOrder() {
    const auto tree = workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true},
            {.parent = 0, .name = "a", .folder = true},
            {.parent = 1, .name = "apple", .folder = true},
            {.parent = 1, .name = "Zed", .folder = true},
            {.parent = 1, .name = "a b", .folder = true},
        },
        0
    );
    TEST_CHECK((ranked(tree) == Names{"Zed", "a b", "apple"}));
  }

  void testTiesCompareTheWholePath() {
    const auto tree = workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true},
            {.parent = 0, .name = "a", .folder = true},
            {.parent = 0, .name = "a b", .folder = true},
            {.parent = 1, .name = "z", .folder = true},
            {.parent = 1, .name = "xa", .folder = true},
            {.parent = 1, .name = "x", .folder = true},
            {.parent = 2, .name = "x", .folder = true},
            {.parent = 5, .name = "deep", .folder = true},
        },
        0
    );
    const auto list = jumpAhead(tree, LayoutSettings{});
    TEST_CHECK(list.rows.size() == 5);
    TEST_CHECK(list.rows[0].path == "a b/x"); // ' ' sorts before '/'
    TEST_CHECK(list.rows[1].path == "a/x");   // a folder before what is below it
    TEST_CHECK(list.rows[2].path == "a/x/deep");
    TEST_CHECK(list.rows[3].path == "a/xa"); // '/' sorts before 'a'
    TEST_CHECK(list.rows[4].path == "a/z");
  }

  void testTiesBetweenSameNamedFolders() {
    const auto tree = workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true},
            {.parent = 0, .name = "a", .folder = true},
            {.parent = 1, .name = "dup", .folder = true},
            {.parent = 1, .name = "dup", .folder = true},
            {.parent = 2, .name = "q", .folder = true},
            {.parent = 3, .name = "p", .folder = true},
        },
        0
    );
    TEST_CHECK((ranked(tree) == Names{"dup", "dup", "p", "q"}));
  }

  constexpr std::size_t kTiedTop = 2'000;
  constexpr std::size_t kTiedEach = 99;

  // kTiedTop folders of kTiedEach folders each, none with visits or bytes, so every pair of candidates ties. Names
  // come in descending order, so each candidate read displaces the rows ranked so far.
  Tree tiedTree() {
    Tree tree;
    tree.root = 0;
    tree.nodes.reserve(1 + kTiedTop + (kTiedTop * kTiedEach));
    tree.nodes.push_back({.name = "r", .folder = true});
    tree.nodes[0].childCount = kTiedTop;
    for (std::size_t top = 0; top < kTiedTop; ++top) {
      tree.children.push_back(static_cast<NodeId>(top + 1));
      tree.nodes.push_back({.name = std::format("level-one-{:04}", kTiedTop - 1 - top), .parent = 0, .folder = true});
    }
    for (std::size_t top = 0; top < kTiedTop; ++top) {
      const auto parent = static_cast<NodeId>(top + 1);
      tree.nodes[parent].firstChild = static_cast<std::uint32_t>(tree.children.size());
      tree.nodes[parent].childCount = kTiedEach;
      for (std::size_t each = 0; each < kTiedEach; ++each) {
        tree.children.push_back(static_cast<NodeId>(tree.nodes.size()));
        tree.nodes.push_back(
            {.name = std::format("folder-{:02}", kTiedEach - 1 - each), .parent = parent, .folder = true}
        );
      }
    }
    return tree;
  }

  void testManyTiesRankQuickly() {
    const auto tree = tiedTree();
    // Processor time, so other tests running in parallel do not count.
    const std::clock_t start = std::clock();
    const auto list = jumpAhead(tree, LayoutSettings{});
    const double elapsed = 1000.0 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
    std::println("  {} tied candidates {:.0f} ms", kTiedTop * kTiedEach, elapsed);
    TEST_CHECK(::test::withinBudget(elapsed, 400)); // a debug build takes about a third of this
    TEST_CHECK(list.rows.size() == kJumpRows);
    TEST_CHECK(!list.cut);
    TEST_CHECK(list.rows[0].path == "level-one-0000/folder-00");
    TEST_CHECK(list.rows[1].path == "level-one-0000/folder-01");
    TEST_CHECK(list.rows[23].path == "level-one-0000/folder-23");
  }

  void testFilesAndRemaindersAreNotCandidates() {
    const auto tree = workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true},
            {.parent = 0, .name = "a", .folder = true},
            {.parent = 1, .name = "big.iso", .bytes = 9000},
            {.parent = 1, .name = "", .folder = true, .bytes = 8000, .items = 40},
            {.parent = 1, .name = "small", .folder = true, .bytes = 1},
        },
        0
    );
    TEST_CHECK((ranked(tree) == Names{"small"}));
  }

  void testRowShowsPathLabelAndShare() {
    const auto tree = referenceTree();
    const auto list = jumpAhead(tree, LayoutSettings{});
    TEST_CHECK(list.rows.size() == 2);
    const auto& x = list.rows[0];
    TEST_CHECK(x.path == "a/x");
    TEST_CHECK((x.split == PathSplit{.prefix = "a/", .leaf = "x"}));
    TEST_CHECK(x.label == "500 bytes");
    TEST_NEAR(x.share, 0.6901333491395494, 1e-12); // sqrt(501) against sqrt(101) for y.png
    TEST_CHECK(!x.partial);
    TEST_CHECK(list.rows[1].path == "a/x/deep");
    TEST_NEAR(list.rows[1].share, 0.665837991734601, 1e-12); // sqrt(401) against sqrt(101) for f.log
    LayoutSettings visits;
    visits.sizeBy = WorkbenchMeasure::Visits;
    TEST_CHECK(jumpAhead(tree, visits).rows[0].label == "5 opens");
  }

  void testLongPathsKeepTheLeaf() {
    const std::string longName(30, 'n');
    const auto tree = workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true},
            {.parent = 0, .name = "first-level-folder", .folder = true},
            {.parent = 1, .name = longName, .folder = true},
        },
        0
    );
    const auto list = jumpAhead(tree, LayoutSettings{});
    TEST_CHECK(list.rows.size() == 1);
    TEST_CHECK(list.rows[0].path == "first-level-folder/" + longName);
    TEST_CHECK(list.rows[0].split.leaf == longName);
    TEST_CHECK(codePointCount(list.rows[0].split.prefix) == kJumpPathChars - longName.size());
    TEST_CHECK(list.rows[0].split.prefix.ends_with('/'));
  }

  void testPartialMeasures() {
    auto tree = referenceTree();
    TEST_CHECK(!jumpAhead(tree, LayoutSettings{}).partial);
    tree.nodes[7].partial = true; // deep: its bytes so far rank it, and the row says so
    tree.nodes[7].bytes = 600;
    auto list = jumpAhead(tree, LayoutSettings{});
    TEST_CHECK((rowNames(tree, list) == Names{"deep", "x"}));
    TEST_CHECK(list.rows[0].partial);
    TEST_CHECK(!list.rows[1].partial);
    TEST_CHECK(list.partial);
    tree.nodes[7].partial = false;
    tree.nodes[1].partial = true; // a is still being measured: what is below it may change
    list = jumpAhead(tree, LayoutSettings{});
    TEST_CHECK(list.partial);
    TEST_CHECK(!list.rows[0].partial);
    tree.nodes[1].partial = false;
    tree.nodes[0].partial = true;
    TEST_CHECK(jumpAhead(tree, LayoutSettings{}).partial);
  }

  // r > a > {b > {d}, c}
  Tree budgetTree() {
    return workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true},
            {.parent = 0, .name = "a", .folder = true},
            {.parent = 1, .name = "b", .folder = true, .bytes = 2},
            {.parent = 1, .name = "c", .folder = true, .bytes = 1},
            {.parent = 2, .name = "d", .folder = true, .bytes = 3},
        },
        0
    );
  }

  void testNodeBudgetReadsNearerLevelsFirst() {
    const auto tree = budgetTree();
    auto list = jumpAhead(tree, LayoutSettings{}, {.nodeBudget = 3});
    TEST_CHECK((rowNames(tree, list) == Names{"b", "c"}));
    TEST_CHECK(list.cut);
    list = jumpAhead(tree, LayoutSettings{}, {.nodeBudget = 4});
    TEST_CHECK((rowNames(tree, list) == Names{"d", "b", "c"}));
    TEST_CHECK(!list.cut);
    list = jumpAhead(tree, LayoutSettings{}, {.nodeBudget = 0});
    TEST_CHECK(list.rows.empty());
    TEST_CHECK(list.cut);
  }

  void testDegenerateInput() {
    TEST_CHECK(jumpAhead(Tree{}, LayoutSettings{}).rows.empty());
    auto tree = referenceTree();
    tree.root = kNoNode;
    TEST_CHECK(jumpAhead(tree, LayoutSettings{}).rows.empty());
    tree.root = 0;
    TEST_CHECK(jumpAhead(tree, LayoutSettings{}, {.count = 0}).rows.empty());
    tree.nodes[5].visits = std::numeric_limits<double>::quiet_NaN(); // reads as no visits
    tree.nodes[7].visits = -3;
    tree.nodes[7].bytes = 1e40; // clamped, still above x
    TEST_CHECK((ranked(tree) == Names{"deep", "x"}));
    tree.nodes[7].bytes = std::numeric_limits<double>::infinity(); // reads as nothing
    TEST_CHECK((ranked(tree) == Names{"x", "deep"}));
    // A child list that leads back up never repeats a node and ends.
    auto cycle = budgetTree();
    cycle.children.push_back(1);
    cycle.nodes[4].firstChild = static_cast<std::uint32_t>(cycle.children.size() - 1);
    cycle.nodes[4].childCount = 1;
    TEST_CHECK((ranked(cycle) == Names{"d", "b", "c"}));
    // A child range outside the list is no children.
    cycle.nodes[1].firstChild = 1000;
    TEST_CHECK(ranked(cycle).empty());
  }
} // namespace

// A row whose label reads "at least" carries the sentence that says why; an exact row has none.
void testRowsSayWhyTheirLabelIsALowerBound() {
  auto tree = referenceTree();
  for (const auto& row : jumpAhead(tree, LayoutSettings{}, {}).rows) {
    TEST_CHECK(row.notCounted.empty());
  }
  tree.nodes[7].unreadable = true;
  tree.nodes[7].truncated = true;
  tree.nodes[7].lowerBound = true;
  tree.nodes[5].lowerBound = true;
  const auto list = jumpAhead(tree, LayoutSettings{}, {});
  TEST_CHECK(list.rows.size() == 2);
  for (const auto& row : list.rows) {
    TEST_CHECK(row.notCounted == "Not counted: 1 unreadable folder (deep).");
  }
}

int main() {
  testDeepFoldersByVisitsThenSize();
  testVisitsChangeTheRanking();
  testRankingIgnoresTheSizeMeasure();
  testHiddenFoldersAndWhatIsBelowThem();
  testTiesFallToThePathInByteOrder();
  testTiesCompareTheWholePath();
  testTiesBetweenSameNamedFolders();
  testManyTiesRankQuickly();
  testFilesAndRemaindersAreNotCandidates();
  testRowShowsPathLabelAndShare();
  testLongPathsKeepTheLeaf();
  testPartialMeasures();
  testRowsSayWhyTheirLabelIsALowerBound();
  testNodeBudgetReadsNearerLevelsFirst();
  testDegenerateInput();
  return 0;
}
