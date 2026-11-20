#include "test_check.h"
#include "workbench/measure.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <format>
#include <limits>
#include <numeric>
#include <print>
#include <string>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using luciole::WorkbenchMeasure;
  using luciole::test::referenceTree;
  using luciole::test::workbenchNames;
  using Names = std::vector<std::string>;

  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

  Names entryNames(const Tree& tree, NodeId folder, const LayoutSettings& settings) {
    return workbenchNames(tree, entries(tree, folder, settings).ids);
  }

  double shareSum(const Tree& tree, NodeId folder, const LayoutSettings& settings) {
    double sum = 0;
    for (const auto id : entries(tree, folder, settings).ids) {
      sum += share(tree, id, settings);
    }
    return sum;
  }

  void testEntriesHeaviestFirstHiddenDroppedTiesByName() {
    const auto tree = referenceTree();
    LayoutSettings settings;
    TEST_CHECK((entryNames(tree, 0, settings) == Names{"a", "b.txt", "c"}));
    settings.showHidden = true;
    TEST_CHECK((entryNames(tree, 0, settings) == Names{"a", "b.txt", ".hidden", "c"}));
    settings.sizeBy = WorkbenchMeasure::Equal;
    TEST_CHECK((entryNames(tree, 0, settings) == Names{".hidden", "a", "b.txt", "c"}));
  }

  void testMeasureChangesOrder() {
    const auto tree = referenceTree();
    LayoutSettings settings;
    TEST_CHECK((entryNames(tree, 5, settings) == Names{"deep", "f.log"}));
    settings.sizeBy = WorkbenchMeasure::Modified;
    TEST_CHECK((entryNames(tree, 5, settings) == Names{"deep", "f.log"}));
    settings.sizeBy = WorkbenchMeasure::Items;
    TEST_CHECK((entryNames(tree, 5, settings) == Names{"deep", "f.log"}));
    settings.sizeBy = WorkbenchMeasure::Visits;
    TEST_CHECK((entryNames(tree, 0, settings) == Names{"a", "b.txt", "c"}));
    settings.sizeBy = WorkbenchMeasure::Files;
    settings.emphasis = 1;
    TEST_CHECK((entryNames(tree, 0, settings) == Names{"a", "b.txt", "c"}));
    TEST_CHECK(weight(tree, 1, settings) == 4);
    TEST_CHECK(weight(tree, 2, settings) == 1);
  }

  void testSplit() {
    const auto tree = referenceTree();
    const LayoutSettings settings;
    const auto two = splitEntries(tree, 0, settings, 2);
    TEST_CHECK((workbenchNames(tree, two.shown) == Names{"a", "b.txt"}));
    TEST_CHECK((workbenchNames(tree, two.rest) == Names{"c"}));
    const auto none = splitEntries(tree, 0, settings, 0);
    TEST_CHECK(none.shown.empty());
    TEST_CHECK(none.rest.size() == 3);
    const auto all = splitEntries(tree, 0, settings, 99);
    TEST_CHECK(all.shown.size() == 3);
    TEST_CHECK(all.rest.empty());
  }

  void testWeightAtEmphasis() {
    const auto tree = referenceTree();
    LayoutSettings settings;
    settings.emphasis = 0;
    TEST_CHECK(weight(tree, 1, settings) == 1);
    TEST_CHECK(weight(tree, 2, settings) == 1);
    settings.emphasis = 1;
    TEST_CHECK(weight(tree, 1, settings) == 601);
    TEST_CHECK(weight(tree, 2, settings) == 301);
    settings.emphasis = 0.5;
    TEST_NEAR(weight(tree, 1, settings), std::sqrt(601.0), 1e-9);
    TEST_CHECK(weight(tree, 4, settings) > 0);
  }

  void testWeightsForEachMeasure() {
    const auto tree = referenceTree();
    LayoutSettings settings;
    settings.sizeBy = WorkbenchMeasure::Visits;
    TEST_NEAR(weight(tree, 1, settings), 2.449489742783178, 1e-15);
    TEST_NEAR(weight(tree, 2, settings), 1.7320508075688772, 1e-15);
    settings.sizeBy = WorkbenchMeasure::Modified;
    TEST_NEAR(weight(tree, 1, settings), 1.000114488434634, 1e-15);
    TEST_NEAR(weight(tree, 2, settings), 0.9997295474030431, 1e-15);
    TEST_NEAR(weight(tree, 4, settings), 0.9994411335357738, 1e-15);
    settings.emphasis = 1;
    TEST_NEAR(weight(tree, 1, settings), 1.0002289899768697, 1e-15);
    settings.sizeBy = WorkbenchMeasure::Items;
    TEST_CHECK(weight(tree, 1, settings) == 3);
    TEST_CHECK(weight(tree, 2, settings) == 1);
    settings.sizeBy = WorkbenchMeasure::Equal;
    TEST_CHECK(weight(tree, 1, settings) == 1);
    TEST_CHECK(weight(tree, 4, settings) == 1);
  }

  void testShares() {
    const auto tree = referenceTree();
    LayoutSettings settings;
    for (const double emphasis : {0.0, 0.5, 1.0}) {
      settings.emphasis = emphasis;
      TEST_NEAR(shareSum(tree, 0, settings), 1.0, 1e-12);
    }
    TEST_CHECK(share(tree, 0, settings) == 1);
    settings.emphasis = 0.5;
    TEST_NEAR(share(tree, 1, settings), 0.5719234771745537, 1e-15);
    TEST_NEAR(share(tree, 2, settings), 0.40474727758619056, 1e-15);
    TEST_NEAR(share(tree, 4, settings), 0.02332924523925563, 1e-15);
    settings.sizeBy = WorkbenchMeasure::Visits;
    settings.emphasis = 1;
    TEST_NEAR(share(tree, 1, settings), 0.6, 1e-15);
    TEST_NEAR(share(tree, 4, settings), 0.1, 1e-15);
    const auto listed = entries(tree, 0, settings);
    TEST_NEAR(listed.total, 10.0, 1e-12);
    TEST_CHECK((listed.weights == std::vector<double>{6, 3, 1}));
  }

  // A share divides by the weights of the siblings, so it is as sure as the least sure of them and the node itself.
  void testShareState() {
    auto tree = referenceTree();
    const LayoutSettings settings;
    TEST_CHECK(shareState(tree, 1, settings) == MeasureState::Done);
    tree.nodes[4].lowerBound = true;
    TEST_CHECK(shareState(tree, 1, settings) == MeasureState::LowerBound);
    TEST_CHECK(shareState(tree, 4, settings) == MeasureState::LowerBound);
    tree.nodes[2].partial = true;
    TEST_CHECK(shareState(tree, 1, settings) == MeasureState::Measuring);
    // The root has no siblings and a share of one; a hidden sibling is not part of the total.
    TEST_CHECK(shareState(tree, 0, settings) == MeasureState::Done);
    tree.nodes[2].partial = false;
    tree.nodes[3].hidden = true;
    tree.nodes[4].lowerBound = false;
    tree.nodes[3].lowerBound = true;
    TEST_CHECK(shareState(tree, 1, settings) == MeasureState::Done);
    TEST_CHECK(shareState(tree, kNoNode, settings) == MeasureState::Done);
    TEST_CHECK(approximateText(MeasureState::Done, "12%") == "12%");
    TEST_CHECK(approximateText(MeasureState::LowerBound, "12%") == "about 12%");
    TEST_CHECK(approximateText(MeasureState::Measuring, "12%") == "about 12%");
    TEST_CHECK(approximateText(MeasureState::Measuring, "").empty());
  }

  void testFolderShares() {
    const auto tree = referenceTree();
    LayoutSettings settings;
    // Children of r in row order: a, b.txt, .hidden, c; the hidden entry has no share.
    const auto all = shares(tree, 0, settings);
    TEST_CHECK(all.size() == 4);
    TEST_NEAR(all[0], 0.5719234771745537, 1e-15);
    TEST_NEAR(all[1], 0.40474727758619056, 1e-15);
    TEST_CHECK(all[2] == 0);
    TEST_NEAR(all[3], 0.02332924523925563, 1e-15);
    settings.showHidden = true;
    const auto shown = shares(tree, 0, settings);
    TEST_CHECK(shown[2] > 0);
    TEST_CHECK(shown[2] == share(tree, 3, settings));
    TEST_CHECK(shares(tree, 99, settings).empty());
    TEST_CHECK(shares(tree, 9, settings).empty()); // a file has no children
  }

  // One folder of 10,000 entries in 37 sizes, so many weights tie and are ordered by name.
  Tree wideFolder() {
    constexpr std::uint32_t kCount = 10'000;
    Tree tree;
    tree.nodes.reserve(kCount + 1);
    Node root;
    root.name = "r";
    root.childCount = kCount;
    root.folder = true;
    tree.nodes.push_back(std::move(root));
    for (std::uint32_t i = 0; i < kCount; ++i) {
      Node node;
      node.name = std::format("{}entry{}", i % 5 == 0 ? "." : "", i);
      node.parent = 0;
      node.bytes = 100.0 + static_cast<double>((i * 7919U) % 37U) * 1000.0;
      node.hidden = i % 5 == 0;
      tree.nodes.push_back(std::move(node));
      tree.children.push_back(i + 1);
    }
    tree.root = 0;
    return tree;
  }

  void testFolderSharesOfAWideFolderAreExactAndCheap() {
    const auto tree = wideFolder();
    const LayoutSettings settings;
    const std::clock_t start = std::clock();
    const auto all = shares(tree, 0, settings);
    const double seconds = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
    TEST_CHECK(all.size() == 10'000);
    // Shares sum to one over the visible entries and match share() bit for bit.
    TEST_NEAR(std::accumulate(all.begin(), all.end(), 0.0), 1.0, 1e-9);
    for (std::size_t i = 0; i < all.size(); i += 97) {
      TEST_CHECK(all[i] == share(tree, tree.children[i], settings));
    }
    TEST_CHECK(all[0] == 0 && all[5] == 0);
    // One sort for the folder; a sort per entry takes seconds here.
    TEST_CHECK(seconds < 0.05);
  }

  void testVisitsChangeOrderAndShares() {
    auto tree = referenceTree();
    LayoutSettings settings;
    settings.sizeBy = WorkbenchMeasure::Visits;
    const double before = share(tree, 2, settings);
    tree.nodes[2].visits = 6;
    TEST_CHECK((entryNames(tree, 0, settings) == Names{"b.txt", "a", "c"}));
    TEST_CHECK(share(tree, 2, settings) > before);
    TEST_NEAR(share(tree, 1, settings) + share(tree, 2, settings) + share(tree, 4, settings), 1.0, 1e-12);
  }

  void testMeasureValueAndLabel() {
    const auto tree = referenceTree();
    TEST_CHECK(measureValue(tree, 1, WorkbenchMeasure::Bytes) == 600);
    TEST_CHECK(measureValue(tree, 1, WorkbenchMeasure::Items) == 2);
    TEST_CHECK(measureValue(tree, 1, WorkbenchMeasure::Files) == 3);
    TEST_CHECK(measureValue(tree, 1, WorkbenchMeasure::Modified) == 4000);
    TEST_CHECK(measureValue(tree, 1, WorkbenchMeasure::Visits) == 5);
    TEST_CHECK(measureValue(tree, 1, WorkbenchMeasure::Equal) == 1);
    TEST_CHECK(age(tree, 1) == 2000);
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Bytes) == "600 bytes");
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Equal) == "600 bytes");
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Visits) == "5 opens");
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Items) == "2 items");
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Files) == "3 files");
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Modified) == "33 min ago");
    TEST_CHECK(measureLabel(tree, 0, WorkbenchMeasure::Modified) == "16 min ago");
    TEST_CHECK(measureLabel(tree, 2, WorkbenchMeasure::Modified) == "1 h ago");
    TEST_CHECK(measureLabel(tree, 2, WorkbenchMeasure::Items) == "–");
    TEST_CHECK(measureLabel(tree, 2, WorkbenchMeasure::Files) == "–");
    TEST_CHECK(measureLabel(tree, 7, WorkbenchMeasure::Items) == "1 item");
    TEST_CHECK(measureLabel(tree, 7, WorkbenchMeasure::Files) == "1 file");
    TEST_CHECK(measureLabel(tree, 3, WorkbenchMeasure::Visits) == "0 opens");
    TEST_CHECK(measureLabel(tree, 99, WorkbenchMeasure::Bytes).empty());
  }

  void testNameComparison() {
    Names names{"file10", "z",  "x-1", "file2", "a b",  "File2", "10",  "file02", "a",
                "B",      "_x", ".x",  "9",     "a\tb", "x_1",   "007", "7"};
    std::ranges::sort(names, [](const std::string& left, const std::string& right) {
      const auto order = compareNames(left, right);
      return order != 0 ? order < 0 : left < right;
    });
    TEST_CHECK(
        (names
         == Names{
             "_x", ".x", "007", "7", "9", "10", "a", "a\tb", "a b", "B", "File2", "file02", "file2", "file10", "x_1",
             "x-1", "z"
         })
    );
    TEST_CHECK(compareNames("file2", "FILE02") == 0);
    TEST_CHECK(compareNames("z", "é") < 0);
    TEST_CHECK(compareNames("", "a") < 0);
    TEST_CHECK(compareNames("", "") == 0);
    const std::string longRun(5000, '9');
    TEST_CHECK(compareNames(longRun, "1" + longRun) < 0);
    TEST_CHECK(compareNames("v" + longRun + "1", "v" + longRun + "2") < 0);
  }

  void testDegenerateInputs() {
    auto tree = referenceTree();
    LayoutSettings settings;
    TEST_CHECK(entries(tree, 4, settings).ids.empty());
    TEST_CHECK(entries(tree, 4, settings).total == 0);
    TEST_CHECK(entries(tree, kNoNode, settings).ids.empty());
    TEST_CHECK(splitEntries(tree, 99, settings, 3).shown.empty());
    TEST_CHECK(share(tree, 99, settings) == 0);
    TEST_CHECK(share(tree, 3, settings) == 0);
    TEST_CHECK(weight(tree, 99, settings) == 0);

    tree.nodes[1].bytes = kNan;
    tree.nodes[2].bytes = -50;
    tree.nodes[4].bytes = kInfinity;
    settings.emphasis = 1;
    TEST_CHECK(measureValue(tree, 1, WorkbenchMeasure::Bytes) == 0);
    TEST_CHECK(measureValue(tree, 2, WorkbenchMeasure::Bytes) == 0);
    TEST_CHECK(measureValue(tree, 4, WorkbenchMeasure::Bytes) == 0);
    TEST_CHECK(weight(tree, 1, settings) == 1);
    TEST_CHECK((entryNames(tree, 0, settings) == Names{"a", "b.txt", "c"}));
    TEST_NEAR(shareSum(tree, 0, settings), 1.0, 1e-12);
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Bytes) == "0 bytes");

    tree = referenceTree();
    tree.nodes[1].bytes = 1e300;
    tree.nodes[2].bytes = 1e300;
    TEST_CHECK(std::isfinite(entries(tree, 0, settings).total));
    TEST_NEAR(shareSum(tree, 0, settings), 1.0, 1e-12);

    tree = referenceTree();
    settings.emphasis = kNan;
    TEST_CHECK(weight(tree, 1, settings) == 1);
    settings.emphasis = 7;
    TEST_CHECK(weight(tree, 1, settings) == 601);
    settings.emphasis = -3;
    TEST_CHECK(weight(tree, 1, settings) == 1);

    settings = {};
    settings.sizeBy = WorkbenchMeasure::Modified;
    tree.nodes[1].modified = 9000;
    TEST_CHECK(age(tree, 1) == 0);
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Modified) == "just now");
    tree.now = kNan;
    TEST_CHECK(age(tree, 1) == 0);
    TEST_CHECK(std::isfinite(weight(tree, 1, settings)));
  }
} // namespace

int main() {
  testEntriesHeaviestFirstHiddenDroppedTiesByName();
  testMeasureChangesOrder();
  testSplit();
  testWeightAtEmphasis();
  testWeightsForEachMeasure();
  testShares();
  testShareState();
  testFolderShares();
  testFolderSharesOfAWideFolderAreExactAndCheap();
  testVisitsChangeOrderAndShares();
  testMeasureValueAndLabel();
  testNameComparison();
  testDegenerateInputs();
  std::println("workbench measure checks passed");
  return 0;
}
