#include "test_check.h"
#include "workbench/flow_layout.h"
#include "workbench/flow_topology.h"
#include "workbench/format.h"
#include "workbench_flow_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <print>
#include <ranges>
#include <string>
#include <vector>

using namespace luciole;
using namespace luciole::workbench;

namespace {
  using test::childNamed;
  using test::FlowSpec;
  using test::flowTree;
  using test::wideTree;

  // One word per record: a relative path, more(owner,count,collapsed) or files(count).
  std::string describe(const Tree& tree, const FlowTopology& topology, std::size_t column) {
    std::string out;
    for (const auto index : topology.columns[column]) {
      const auto& record = topology.records[index];
      const auto path = relativePath(tree, record.node, topology.folder).value_or("?");
      if (!out.empty()) {
        out += ' ';
      }
      switch (record.kind) {
      case FlowRecordKind::Current:
        out += "cur";
        break;
      case FlowRecordKind::Folder:
        out += path;
        break;
      case FlowRecordKind::More:
        out += std::format("more({},{},{})", path, record.count, record.collapsed ? 1 : 0);
        break;
      case FlowRecordKind::Files:
        out += std::format("files({})", record.count);
        break;
      }
    }
    return out;
  }

  std::size_t countKind(const FlowTopology& topology, std::size_t column, FlowRecordKind kind) {
    return static_cast<std::size_t>(std::ranges::count_if(topology.columns[column], [&](std::size_t index) {
      return topology.records[index].kind == kind;
    }));
  }

  void firstColumnHoldsEightFoldersMoreAndFiles(const Tree& wide) {
    const auto topology = buildFlowTopology(wide, 0, {});
    const auto& first = topology.columns[1];
    TEST_CHECK(countKind(topology, 1, FlowRecordKind::Folder) == 8);
    TEST_CHECK(countKind(topology, 1, FlowRecordKind::More) == 1);
    TEST_CHECK(topology.records[first.back()].kind == FlowRecordKind::Files);
    for (const auto index : first) {
      const auto& record = topology.records[index];
      if (record.kind == FlowRecordKind::More) {
        TEST_CHECK(record.count == 14 + 3000 - 8);
        TEST_CHECK(record.folders.size() == record.count);
      }
    }
    for (const auto& record : topology.records) {
      if (record.kind == FlowRecordKind::Folder) {
        TEST_CHECK(wide.nodes[record.node].folder);
      }
    }
    for (std::size_t i = 1; i < 8; ++i) {
      TEST_CHECK(topology.records[first[i]].weight <= topology.records[first[i - 1]].weight);
    }
    TEST_CHECK(describe(wide, topology, 0) == "cur");
    TEST_CHECK(describe(wide, topology, 1) == "d0 d1 d2 d3 d4 d5 d6 d7 more(,3006,0) files(2000)");
    TEST_CHECK(
        describe(wide, topology, 2)
        == "d0/e0 d0/e1 d0/e2 more(d0,3,0) more(d1,6,1) more(d2,6,1) more(d3,6,1) more(d4,6,1) more(d5,6,1) "
           "more(d6,6,1) more(d7,6,1) files(8)"
    );
    TEST_CHECK(
        describe(wide, topology, 3)
        == "d0/e0/g0 d0/e0/g1 more(d0/e0,2,0) d0/e1/g0 d0/e1/g1 more(d0/e1,2,0) d0/e2/g0 d0/e2/g1 more(d0/e2,2,0) "
           "files(9)"
    );
    TEST_NEAR(topology.records[first[0]].weight, 1000.000499999875, 1e-9);
    TEST_NEAR(topology.records[first[8]].weight, 11734.24343469113, 1e-7);
    TEST_NEAR(topology.records[first[9]].weight, 774.5973147384387, 1e-9);
    TEST_NEAR(topology.records[first[9]].totals.bytes, 600000.0, 1e-6);
    TEST_NEAR(topology.records[first[9]].totals.items, 2000.0, 0.0);
  }

  void laterColumnsTakeThreeThenTwoPerParent(const Tree& wide) {
    const auto topology = buildFlowTopology(wide, 0, {});
    for (std::size_t column = 1; column < kFlowColumns; ++column) {
      const auto& records = topology.columns[column];
      TEST_CHECK(records.size() <= 12);
      TEST_CHECK(countKind(topology, column, FlowRecordKind::Files) <= 1);
      for (std::size_t i = 0; i + 1 < records.size(); ++i) {
        TEST_CHECK(topology.records[records[i]].kind != FlowRecordKind::Files);
      }
    }
    const auto mostPerParent = [&topology](std::size_t column) {
      std::size_t most = 0;
      for (const auto index : topology.columns[column - 1]) {
        std::size_t folders = 0;
        for (const auto kid : topology.records[index].kids) {
          folders += topology.records[kid].kind == FlowRecordKind::Folder ? 1 : 0;
        }
        most = std::max(most, folders);
      }
      return most;
    };
    TEST_CHECK(mostPerParent(2) <= 3);
    TEST_CHECK(mostPerParent(3) <= 2);
    TEST_CHECK(!topology.columns[3].empty());
  }

  void foldedGroupListsEveryFolderOfItsParent(const Tree& wide) {
    const auto topology = buildFlowTopology(wide, 0, {});
    std::size_t folded = 0;
    for (const auto index : topology.columns[2]) {
      const auto& record = topology.records[index];
      if (record.kind != FlowRecordKind::More || !record.collapsed) {
        continue;
      }
      ++folded;
      std::size_t folders = 0;
      for (const auto child : children(wide, record.node)) {
        folders += wide.nodes[child].folder ? 1 : 0;
      }
      TEST_CHECK(record.count == folders);
    }
    TEST_CHECK(folded > 0);
  }

  void workIsBounded(const Tree& wide) {
    const auto topology = buildFlowTopology(wide, 0, {});
    TEST_CHECK(!topology.truncated);
    // The current folder and every drawn folder of columns 1 and 2 are read, and nothing else.
    std::size_t drawnEntries = children(wide, 0).size();
    for (std::size_t column = 1; column <= 2; ++column) {
      for (const auto index : topology.columns[column]) {
        const auto& record = topology.records[index];
        if (record.kind == FlowRecordKind::Folder) {
          drawnEntries += children(wide, record.node).size();
        }
      }
    }
    TEST_CHECK(topology.spent == drawnEntries);
    TEST_CHECK(topology.spent == 5091);

    const auto tight = buildFlowTopology(wide, 0, {}, {.budget = 20});
    TEST_CHECK(tight.truncated);
    TEST_CHECK(tight.spent == 5014);
    TEST_CHECK(tight.columns[2].size() < topology.columns[2].size());
    TEST_CHECK(tight.columns[1].size() == 10 && tight.columns[2].empty() && tight.columns[3].empty());
  }

  void emptyFoldersHaveAReasonAndNoColumns(const Tree& wide) {
    const auto empty = buildFlowTopology(wide, childNamed(wide, 0, "t0"), {});
    TEST_CHECK(empty.empty == FlowEmpty::Empty);
    TEST_CHECK(empty.columns[1].empty() && empty.records.size() == 1);

    const auto small = flowTree(
        {{.parent = -1, .name = "x", .folder = true},
         {.parent = 0, .name = "a.txt", .bytes = 5},
         {.parent = 0, .name = ".hide", .folder = true, .bytes = 9}},
        2, 1
    );
    TEST_CHECK(buildFlowTopology(small, 2, {}).empty == FlowEmpty::Empty);
    TEST_CHECK(buildFlowTopology(small, 0, {}).empty == FlowEmpty::NoFolders);
    const auto shown = buildFlowTopology(small, 0, {.showHidden = true});
    TEST_CHECK(shown.empty == FlowEmpty::None && shown.columns[1].size() == 2);
    TEST_CHECK(buildFlowTopology(wide, 0, {}).empty == FlowEmpty::None);

    const auto unknown = buildFlowTopology(wide, 999999, {});
    TEST_CHECK(unknown.records.empty() && unknown.folder == 999999);
    TEST_CHECK(unknown.columns[0].empty());
  }

  // An unreadable folder says so; one that was not expanded, or lies on another filesystem, says that instead of
  // looking empty. A flag changes nothing while column 1 has folders.
  void emptyFoldersSayWhyTheyHoldNothing() {
    const auto tree = luciole::test::workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true, .items = 5},
            {.parent = 0, .name = "locked", .folder = true, .items = 3, .unreadable = true},
            {.parent = 0, .name = "big", .folder = true, .items = 40, .truncated = true},
            {.parent = 0, .name = "mnt", .folder = true, .items = 5, .otherFilesystem = true},
            {.parent = 0, .name = "void", .folder = true},
            {.parent = 0, .name = "a.txt", .bytes = 5},
        },
        2
    );
    TEST_CHECK(buildFlowTopology(tree, 1, {}).empty == FlowEmpty::Unreadable);
    TEST_CHECK(buildFlowTopology(tree, 2, {}).empty == FlowEmpty::NotExpanded);
    TEST_CHECK(buildFlowTopology(tree, 3, {}).empty == FlowEmpty::NotExpanded);
    TEST_CHECK(buildFlowTopology(tree, 4, {}).empty == FlowEmpty::Empty);
    auto flagged = tree;
    flagged.nodes[0].truncated = true;
    TEST_CHECK(buildFlowTopology(flagged, 0, {}).empty == FlowEmpty::None);
    flagged.nodes[4].unreadable = true;
    flagged.nodes[4].truncated = true;
    TEST_CHECK(buildFlowTopology(flagged, 4, {}).empty == FlowEmpty::Unreadable);
  }

  void recordForFindsTheDrawnRecord(const Tree& wide) {
    const auto topology = buildFlowTopology(wide, 0, {});
    const auto d0 = childNamed(wide, 0, "d0");
    const auto e0 = childNamed(wide, d0, "e0");
    const auto g3 = childNamed(wide, e0, "g3");
    const auto t7 = childNamed(wide, 0, "t7");
    const auto d1 = childNamed(wide, 0, "d1");
    TEST_CHECK(topology.records[flowRecordFor(wide, topology, d0).value()].node == d0);
    TEST_CHECK(topology.records[flowRecordFor(wide, topology, e0).value()].node == e0);
    const auto deep = flowRecordFor(wide, topology, g3);
    TEST_CHECK(deep.has_value());
    TEST_CHECK(topology.records[*deep].kind == FlowRecordKind::Folder && topology.records[*deep].node == e0);
    TEST_CHECK(topology.records[flowRecordFor(wide, topology, t7).value()].kind == FlowRecordKind::More);
    TEST_CHECK(!flowRecordFor(wide, topology, 0).has_value());
    TEST_CHECK(!flowRecordFor(wide, topology, 999999).has_value());
    const auto readme = childNamed(wide, d0, "readme0");
    TEST_CHECK(topology.records[flowRecordFor(wide, topology, readme).value()].node == d0);

    const auto e0Record = flowRecordFor(wide, topology, e0).value();
    const auto focus = flowFocus(topology, e0Record);
    TEST_CHECK(focus.size() == topology.records.size());
    const auto d0Record = flowRecordFor(wide, topology, d0).value();
    const auto d1Record = flowRecordFor(wide, topology, d1).value();
    TEST_CHECK(focus[0] && focus[d0Record] && focus[e0Record]);
    TEST_CHECK(!focus[d1Record]);
    for (const auto kid : topology.records[e0Record].kids) {
      TEST_CHECK(focus[kid]);
    }
    std::size_t lit = 0;
    for (const bool on : focus) {
      lit += on ? 1 : 0;
    }
    TEST_CHECK(lit == 6);
    const auto none = flowFocus(topology, kNoRecord);
    TEST_CHECK(none.size() == topology.records.size() && std::ranges::none_of(none, [](bool on) { return on; }));
  }

  void shortBandKeepsTheHeavyFolders(const Tree& wide) {
    const auto tall = buildFlowTopology(wide, 0, {}, flowOptions(300));
    const auto low = buildFlowTopology(wide, 0, {}, flowOptions(180));
    const auto folders = [&wide](const FlowTopology& topology) {
      std::vector<std::string> names;
      for (const auto index : topology.columns[1]) {
        if (topology.records[index].kind == FlowRecordKind::Folder) {
          names.push_back(wide.nodes[topology.records[index].node].name);
        }
      }
      return names;
    };
    const auto more = [](const FlowTopology& topology) {
      for (const auto index : topology.columns[1]) {
        if (topology.records[index].kind == FlowRecordKind::More) {
          return topology.records[index].count;
        }
      }
      return std::size_t{0};
    };
    const auto tallNames = folders(tall);
    const auto lowNames = folders(low);
    TEST_CHECK(lowNames.size() < tallNames.size());
    TEST_CHECK(std::ranges::equal(lowNames, tallNames | std::views::take(lowNames.size())));
    TEST_CHECK(more(low) > more(tall) && more(low) + lowNames.size() == more(tall) + tallNames.size());
    TEST_CHECK(describe(wide, low, 1) == "d0 d1 d2 d3 d4 d5 d6 more(,3007,0) files(2000)");
    TEST_CHECK(
        describe(wide, low, 2)
        == "more(d0,6,1) more(d1,6,1) more(d2,6,1) more(d3,6,1) more(d4,6,1) more(d5,6,1) more(d6,6,1) files(7)"
    );
    TEST_CHECK(low.columns[3].empty());
  }

  void recordTextNamesEveryKind(const Tree& wide) {
    const auto topology = buildFlowTopology(wide, 0, {});
    const auto& first = topology.columns[1];
    TEST_CHECK(flowRecordName(wide, topology, 0) == "w");
    TEST_CHECK(flowRecordName(wide, topology, first[0]) == "d0");
    TEST_CHECK(flowRecordName(wide, topology, first[8]) == "+3006 more");
    TEST_CHECK(flowRecordName(wide, topology, first[9]) == "files");
    TEST_CHECK(flowRecordName(wide, topology, topology.columns[2][4]) == "6 folders");
    TEST_CHECK(flowRecordName(wide, topology, kNoRecord).empty());
    TEST_CHECK(flowRecordValue(wide, topology, first[0], WorkbenchMeasure::Bytes) == "976.6\u00A0KiB");
    TEST_CHECK(flowRecordValue(wide, topology, first[9], WorkbenchMeasure::Bytes) == "585.9\u00A0KiB");
    TEST_CHECK(flowRecordValue(wide, topology, first[9], WorkbenchMeasure::Items) == "2,000 items");
    TEST_CHECK(flowRecordValue(wide, topology, first[8], WorkbenchMeasure::Items) == "3,006 items");
    TEST_CHECK(flowRecordValue(wide, topology, kNoRecord, WorkbenchMeasure::Bytes).empty());

    const auto rootless = flowTree({{.parent = -1, .name = "", .folder = true}}, 2, 1);
    TEST_CHECK(flowRecordName(rootless, buildFlowTopology(rootless, 0, {}), 0) == "/");
  }

  void filesMeasureCountsFilesOfMoreAndFiles(const Tree& wide) {
    const LayoutSettings byFiles{.sizeBy = WorkbenchMeasure::Files};
    const auto topology = buildFlowTopology(wide, 0, byFiles);
    const auto filesRecord = topology.columns[1].back();
    TEST_CHECK(topology.records[filesRecord].kind == FlowRecordKind::Files);
    TEST_NEAR(topology.records[filesRecord].weight, std::sqrt(2001.0), 1e-9);
    TEST_CHECK(flowRecordValue(wide, topology, filesRecord, WorkbenchMeasure::Files) == "2,000 files");

    // r holds a (7 files below it), b (5), c (3) and four files; b and c fold into More.
    const auto tree = flowTree(
        {{.parent = -1, .name = "r", .folder = true, .files = 19},
         {.parent = 0, .name = "a", .folder = true, .bytes = 70, .items = 7, .files = 7},
         {.parent = 0, .name = "b", .folder = true, .bytes = 50, .items = 5, .files = 5},
         {.parent = 0, .name = "c", .folder = true, .bytes = 30, .items = 3, .files = 3},
         {.parent = 0, .name = "f0", .bytes = 1},
         {.parent = 0, .name = "f1", .bytes = 1},
         {.parent = 0, .name = "f2", .bytes = 1},
         {.parent = 0, .name = "f3", .bytes = 1}},
        2, 1
    );
    const auto small = buildFlowTopology(tree, 0, byFiles, {.firstMax = 1});
    TEST_CHECK(describe(tree, small, 1) == "a more(,2,0) files(4)");
    const auto& first = small.columns[1];
    TEST_CHECK(flowRecordValue(tree, small, first[0], WorkbenchMeasure::Files) == "7 files");
    TEST_CHECK(flowRecordValue(tree, small, first[1], WorkbenchMeasure::Files) == "8 files");
    TEST_CHECK(flowRecordValue(tree, small, first[2], WorkbenchMeasure::Files) == "4 files");
    TEST_NEAR(small.records[first[1]].weight, std::sqrt(6.0) + 2.0, 1e-9);
    TEST_NEAR(small.records[first[2]].weight, std::sqrt(5.0), 1e-9);
  }

  void smallCapDropsTheLightestGroups(const Tree& wide) {
    const auto topology = buildFlowTopology(wide, 0, {}, {.perColumn = 3, .firstMax = 8});
    TEST_CHECK(describe(wide, topology, 1) == "d0 d1 d2 d3 d4 d5 d6 d7 more(,3006,0) files(2000)");
    TEST_CHECK(describe(wide, topology, 2) == "more(d0,6,1) more(d1,6,1) files(8)");
    TEST_CHECK(topology.columns[3].empty());

    const auto none = buildFlowTopology(wide, 0, {}, {.perColumn = 0, .firstMax = 0});
    TEST_CHECK(describe(wide, none, 1) == "more(,3014,0) files(2000)");
    TEST_CHECK(none.columns[2].empty());
  }

  void tightBudgetReadsTheHeaviestParentsFirst(const Tree& wide) {
    const auto topology = buildFlowTopology(wide, 0, {}, {.budget = 5035});
    TEST_CHECK(topology.truncated && topology.spent == 5035);
    TEST_CHECK(
        describe(wide, topology, 2)
        == "d0/e0 d0/e1 d0/e2 more(d0,3,0) d1/e0 d1/e1 d1/e2 more(d1,3,0) more(d2,6,1) files(3)"
    );
    TEST_CHECK(topology.columns[3].empty());
  }

  void tiedParentsFoldAndDropInColumnOrder() {
    // a and b weigh the same; the first one in the column folds first and is dropped first.
    std::vector<FlowSpec> specs{
        {.parent = -1, .name = "r", .folder = true},
        {.parent = 0, .name = "a", .folder = true, .bytes = 100, .items = 4},
        {.parent = 0, .name = "b", .folder = true, .bytes = 100, .items = 4},
    };
    for (int parent = 1; parent <= 2; ++parent) {
      for (int k = 0; k < 4; ++k) {
        specs.push_back(
            {.parent = parent,
             .name = std::format("{}{}", parent == 1 ? "a" : "b", k),
             .folder = true,
             .bytes = 40.0 - k,
             .items = 1}
        );
      }
    }
    const auto tree = flowTree(specs, 2, 1);
    const auto five = buildFlowTopology(tree, 0, {}, {.perColumn = 5});
    TEST_CHECK(describe(tree, five, 2) == "more(a,4,1) b/b0 b/b1 b/b2 more(b,1,0)");
    const auto one = buildFlowTopology(tree, 0, {}, {.perColumn = 1});
    TEST_CHECK(describe(tree, one, 2) == "more(b,4,1)");
  }

  void recordForMissesWhatIsNotDrawn(const Tree& wide) {
    const auto topology = buildFlowTopology(wide, 0, {});
    TEST_CHECK(!flowRecordFor(wide, topology, childNamed(wide, 0, "file0")).has_value());
    TEST_CHECK(!flowRecordFor(wide, buildFlowTopology(wide, 999999, {}), 1).has_value());
  }
} // namespace

int main() {
  const auto wide = wideTree();
  firstColumnHoldsEightFoldersMoreAndFiles(wide);
  laterColumnsTakeThreeThenTwoPerParent(wide);
  foldedGroupListsEveryFolderOfItsParent(wide);
  workIsBounded(wide);
  emptyFoldersHaveAReasonAndNoColumns(wide);
  emptyFoldersSayWhyTheyHoldNothing();
  recordForFindsTheDrawnRecord(wide);
  shortBandKeepsTheHeavyFolders(wide);
  recordTextNamesEveryKind(wide);
  filesMeasureCountsFilesOfMoreAndFiles(wide);
  smallCapDropsTheLightestGroups(wide);
  recordForMissesWhatIsNotDrawn(wide);
  tightBudgetReadsTheHeaviestParentsFirst(wide);
  tiedParentsFoldAndDropInColumnOrder();
  std::println("workbench flow topology checks passed");
  return 0;
}
