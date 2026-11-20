#include "test_check.h"
#include "workbench/flow_layout.h"
#include "workbench_flow_fixture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <print>
#include <utility>
#include <vector>

using namespace luciole;
using namespace luciole::workbench;

namespace {
  using test::childNamed;
  using test::flowTree;
  using test::placeWide;
  using test::wideFrom;
  using test::wideTree;

  constexpr double kEdge = 12.0;
  constexpr double kMinBar = 14.0;
  constexpr double kLine = 15.0;

  void capacityFollowsTheBand() {
    std::size_t previous = 0;
    for (const double height : {90.0, 120.0, 150.0, 180.0, 200.0, 220.0, 240.0, 300.0, 420.0}) {
      const auto capacity = flowCapacity(height);
      TEST_CHECK(capacity >= previous && capacity >= 3 && capacity <= 12);
      previous = capacity;
    }
    TEST_CHECK(flowCapacity(240) == 12);
    TEST_CHECK(flowCapacity(380) == 12);
    TEST_CHECK(flowCapacity(180) >= 8);
    const auto options = flowOptions(180);
    TEST_CHECK(options.firstMax + 2 <= options.perColumn && options.firstMax >= 1 && options.firstMax <= 8);
    TEST_CHECK(flowOptions(300).firstMax == 8);
    TEST_CHECK(flowOptions(300).budget == 60000);

    TEST_CHECK(flowCapacity(90) == 4);
    TEST_CHECK(flowCapacity(120) == 6);
    TEST_CHECK(flowCapacity(179) == 9);
    TEST_CHECK(flowCapacity(200) == 11);
    TEST_CHECK(flowOptions(90).firstMax == 2);
    TEST_CHECK(flowOptions(150).firstMax == 5);
    TEST_CHECK(flowOptions(150).perColumn == 7);
    TEST_CHECK(flowOptions(0).firstMax == 1);
  }

  void capacityFitsEveryIntegerHeight() {
    std::size_t passes = 0;
    for (int height = 90; height <= 420; ++height) {
      const auto band = static_cast<double>(height);
      const auto count = static_cast<double>(flowCapacity(band));
      if (count > 3) {
        TEST_CHECK(count * kLine + (count - 1) <= band - 2 * kEdge);
      }
      TEST_CHECK(count * kMinBar + (count - 1) * 2 <= std::max(band - 2 * kEdge, 3 * kMinBar + 4));
      ++passes;
    }
    TEST_CHECK(passes == 331);
    for (const double bad :
         {std::nan(""), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
      TEST_CHECK(flowCapacity(bad) == flowCapacity(0));
    }
    TEST_CHECK(flowCapacity(-5) == 3);
  }

  void closeLabelsAreSpreadInsideTheBox() {
    const std::array<FlowLabelBox, 4> boxes{{{20, 15}, {22, 15}, {24, 15}, {26, 31}}};
    const auto out = flowSpreadLabels(boxes, 10, 120);
    TEST_CHECK(out.size() == 4);
    for (std::size_t i = 1; i < out.size(); ++i) {
      TEST_CHECK(out[i] > out[i - 1]);
    }
    TEST_CHECK(out[0] - 7.5 >= 10 - 1e-9 && out[3] + 15.5 <= 120 + 1e-9);
    TEST_NEAR(out[0], 20.0, 1e-9);
    TEST_NEAR(out[1], 36.0, 1e-9);
    TEST_NEAR(out[2], 52.0, 1e-9);
    TEST_NEAR(out[3], 76.0, 1e-9);
    TEST_CHECK(flowSpreadLabels({}, 0, 10).empty());
  }

  const FlowRecord& recordOf(const FlowLayout& layout, std::size_t column, std::size_t row) {
    return layout.topology.records[layout.topology.columns[column][row]];
  }

  void barsStayInsideTheBand(const FlowLayout& layout, double height) {
    for (std::size_t column = 1; column < kFlowColumns; ++column) {
      const auto& records = layout.topology.columns[column];
      for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& record = layout.topology.records[records[i]];
        TEST_CHECK(record.height >= kMinBar - 1e-9);
        TEST_CHECK(record.y >= kEdge - 1e-6 && record.y + record.height <= height - kEdge + 1e-6);
        if (i > 0) {
          const auto& above = layout.topology.records[records[i - 1]];
          TEST_CHECK(record.y >= above.y + above.height - 1e-6);
        }
      }
    }
  }

  void labelsDoNotOverlap(const FlowLayout& layout, double height, bool insideBottom) {
    for (std::size_t column = 0; column < kFlowColumns; ++column) {
      std::vector<FlowLabel> labels;
      for (const auto& label : layout.labels) {
        if (layout.topology.records[label.record].column == column) {
          labels.push_back(label);
        }
      }
      std::ranges::sort(labels, {}, &FlowLabel::centerY);
      for (std::size_t i = 1; i < labels.size(); ++i) {
        TEST_CHECK(labels[i].centerY - labels[i].height / 2 >= labels[i - 1].centerY + labels[i - 1].height / 2 - 1e-6);
      }
      for (const auto& label : labels) {
        TEST_CHECK(label.centerY - label.height / 2 >= kEdge - 1e-6);
        if (insideBottom) {
          TEST_CHECK(label.centerY + label.height / 2 <= height - kEdge + 1e-6);
        }
      }
    }
  }

  void barsAreAtLeastFourteenAndMoreIsCapped(const Tree& wide) {
    const auto layout = placeWide(wide, 1120, 240);
    barsStayInsideTheBand(layout, 240);
    double real = 0;
    const FlowRecord* more = nullptr;
    for (const auto index : layout.topology.columns[1]) {
      const auto& record = layout.topology.records[index];
      if (record.kind == FlowRecordKind::More) {
        more = &record;
      } else {
        real += record.height;
      }
    }
    TEST_CHECK(more != nullptr);
    TEST_CHECK(more->weight > real);
    TEST_CHECK(more->height <= real * 0.1 / 0.9 + kMinBar + 1e-6);
  }

  void heightsFollowWeight(const Tree& wide) {
    const auto layout = placeWide(wide);
    for (std::size_t i = 1; i < 8; ++i) {
      TEST_CHECK(recordOf(layout, 1, i).height <= recordOf(layout, 1, i - 1).height + 1e-9);
    }
    TEST_CHECK(recordOf(layout, 1, 0).height > recordOf(layout, 1, 7).height);
  }

  void placementMatchesTheWorkedExample(const Tree& wide) {
    const auto layout = placeWide(wide);
    TEST_CHECK((layout.columnX == std::array<double, 4>{12, 312, 613, 913}));
    TEST_NEAR(layout.topology.records[0].y, 48.863636363636374, 1e-9);
    TEST_NEAR(layout.topology.records[0].height, 197.27272727272725, 1e-9);
    TEST_NEAR(recordOf(layout, 1, 0).height, 40.61484136934325, 1e-9);
    TEST_NEAR(recordOf(layout, 1, 1).y, 58.61484136934325, 1e-9);
    TEST_NEAR(recordOf(layout, 1, 7).height, 14.359565132703121, 1e-9);
    TEST_NEAR(recordOf(layout, 1, 8).y, 237.54545454545453, 1e-9);
    TEST_NEAR(recordOf(layout, 1, 8).height, 19.727272727272723, 1e-9);
    TEST_NEAR(recordOf(layout, 1, 9).y, 263.27272727272725, 1e-9);
    TEST_NEAR(recordOf(layout, 2, 0).height, 25.687196698200644, 1e-9);
    TEST_NEAR(recordOf(layout, 2, 2).y, 63.85087848999066, 1e-9);
    TEST_NEAR(recordOf(layout, 2, 8).y, 174.54438574107252, 1e-9);
    TEST_NEAR(recordOf(layout, 2, 10).y, 217.36567197910296, 1e-9);
    TEST_NEAR(recordOf(layout, 2, 11).y, 269.0, 1e-9);
    TEST_NEAR(recordOf(layout, 3, 0).height, 16.246740700889962, 1e-9);
    TEST_NEAR(recordOf(layout, 3, 7).y, 158.7402221026699, 1e-9);
    TEST_NEAR(recordOf(layout, 3, 9).y, 269.0, 1e-9);

    const auto& ribbon = layout.ribbons[1];
    TEST_NEAR(ribbon.x0, 28.0, 0.0);
    TEST_NEAR(ribbon.x1, 312.0, 0.0);
    TEST_NEAR(ribbon.parentTop, 89.47847773297963, 1e-9);
    TEST_NEAR(ribbon.parentBottom, 118.19752184155509, 1e-9);
    TEST_NEAR(ribbon.childTop, 58.61484136934325, 1e-9);
    TEST_NEAR(ribbon.childBottom, 87.33388547791871, 1e-9);
    TEST_NEAR(ribbon.top[0].x, 28.0, 0.0);
    TEST_NEAR(ribbon.top[0].y, 89.47847773297963, 1e-9);
    TEST_NEAR(ribbon.top[1].x, 170.0, 0.0);
    TEST_NEAR(ribbon.top[1].y, 89.47847773297963, 1e-9);
    TEST_NEAR(ribbon.top[2].x, 170.0, 0.0);
    TEST_NEAR(ribbon.top[2].y, 58.61484136934325, 1e-9);
    TEST_NEAR(ribbon.top[3].x, 312.0, 0.0);
    TEST_NEAR(ribbon.bottom[0].y, 87.33388547791871, 1e-9);
    TEST_NEAR(ribbon.bottom[1].x, 170.0, 0.0);
    TEST_NEAR(ribbon.bottom[2].y, 118.19752184155509, 1e-9);
    TEST_NEAR(ribbon.bottom[3].x, 28.0, 0.0);
    TEST_CHECK(!ribbon.muted);

    TEST_NEAR(layout.labels[0].x, 34.0, 0.0);
    TEST_NEAR(layout.labels[0].centerY, 147.5, 1e-9);
    TEST_NEAR(layout.labels[0].height, 31.0, 0.0);
    TEST_CHECK(layout.labels[0].lines == 2);
    TEST_NEAR(layout.labels[0].maxWidth, 272.0, 0.0);
    TEST_NEAR(layout.labels[1].centerY, 32.30742068467163, 1e-9);
    TEST_NEAR(layout.labels[2].centerY, 72.97436342363099, 1e-9);
    TEST_CHECK(layout.labels[2].lines == 1);
    TEST_NEAR(layout.labels[2].maxWidth, 273.0, 0.0);
    TEST_CHECK(layout.labels.size() == layout.topology.records.size());
  }

  void ribbonsKeepTheirParentsOrder(const Tree& wide) {
    const auto layout = placeWide(wide);
    for (std::size_t column = 1; column < kFlowColumns; ++column) {
      std::vector<FlowRibbon> ribbons;
      for (const auto& ribbon : layout.ribbons) {
        if (layout.topology.records[ribbon.record].column == column) {
          ribbons.push_back(ribbon);
        }
      }
      std::ranges::sort(ribbons, {}, &FlowRibbon::childTop);
      for (std::size_t i = 1; i < ribbons.size(); ++i) {
        TEST_CHECK(ribbons[i].parentTop >= ribbons[i - 1].parentTop - 1e-6);
        TEST_CHECK(ribbons[i].parentTop >= ribbons[i - 1].parentBottom - 1e-6);
      }
    }
  }

  void ribbonsMatchTheirBars(const Tree& wide) {
    const auto layout = placeWide(wide);
    TEST_CHECK(!layout.ribbons.empty());
    for (const auto& ribbon : layout.ribbons) {
      const auto& record = layout.topology.records[ribbon.record];
      const auto& parent = layout.topology.records[record.parent];
      TEST_CHECK(std::abs(ribbon.childBottom - ribbon.childTop - record.height) < 1e-6);
      TEST_CHECK(ribbon.parentTop >= parent.y - 1e-6 && ribbon.parentBottom <= parent.y + parent.height + 1e-6);
      TEST_CHECK(ribbon.parentBottom - ribbon.parentTop <= record.height + 1e-6);
      TEST_CHECK(ribbon.muted == (record.kind == FlowRecordKind::More));
      if (record.column == 1) {
        TEST_CHECK(std::abs(ribbon.parentBottom - ribbon.parentTop - record.height) < 1e-6);
      }
    }
  }

  void labelsSitRightOfTheirBars(const Tree& wide) {
    const auto layout = placeWide(wide);
    labelsDoNotOverlap(layout, 295, false);
    for (const auto& label : layout.labels) {
      const auto& record = layout.topology.records[label.record];
      TEST_CHECK(label.x > record.x + 16 && label.maxWidth > 60);
    }
  }

  void oneFolderChainAndEmptyFolder() {
    const auto chain = flowTree(
        {{.parent = -1, .name = "c", .folder = true},
         {.parent = 0, .name = "a", .folder = true, .bytes = 10, .items = 1},
         {.parent = 1, .name = "b", .folder = true, .bytes = 10, .items = 1},
         {.parent = 2, .name = "d", .folder = true, .bytes = 10, .items = 1},
         {.parent = 3, .name = "f", .bytes = 10}},
        2, 1
    );
    const auto layout = placeFlow(buildFlowTopology(chain, 0, {}), 1120, 295, 0);
    for (const auto& column : layout.topology.columns) {
      TEST_CHECK(column.size() == 1);
    }
    TEST_CHECK(layout.ribbons.size() == 3);

    const auto empty = placeFlow(buildFlowTopology(chain, 3, {}), 1120, 295, 0);
    TEST_CHECK(empty.topology.empty == FlowEmpty::NoFolders);
    const auto& current = empty.topology.records[0];
    TEST_CHECK(current.height > 0 && current.x > 0);
    TEST_NEAR(current.height, 56.0, 0.0);
    TEST_NEAR(current.y, 119.5, 1e-9);
    TEST_NEAR(current.x, 12.0, 0.0);
    TEST_CHECK(empty.ribbons.empty());
  }

  void filesHaveNoParentRibbonOrPath() {
    // a holds a file and a folder, b is a lighter folder: the files of column 2 belong to a, never to b.
    const auto tree = flowTree(
        {{.parent = -1, .name = "r", .folder = true},
         {.parent = 0, .name = "a", .folder = true, .bytes = 900, .items = 2},
         {.parent = 0, .name = "b", .folder = true, .bytes = 100, .items = 1},
         {.parent = 1, .name = "a1", .folder = true, .bytes = 500, .items = 1},
         {.parent = 1, .name = "a.txt", .bytes = 400},
         {.parent = 2, .name = "b1", .folder = true, .bytes = 50, .items = 1},
         {.parent = 3, .name = "deep.txt", .bytes = 5}},
        2, 1
    );
    const auto layout = placeFlow(buildFlowTopology(tree, 0, {}), 1120, 295, 0);
    const auto& records = layout.topology.records;
    std::vector<std::size_t> files;
    for (std::size_t i = 0; i < records.size(); ++i) {
      if (records[i].kind == FlowRecordKind::Files) {
        files.push_back(i);
      }
    }
    TEST_CHECK(!files.empty());
    for (const auto file : files) {
      TEST_CHECK(records[file].parent == kNoRecord);
      TEST_CHECK(std::ranges::none_of(records, [file](const FlowRecord& record) {
        return std::ranges::contains(record.kids, file);
      }));
      TEST_CHECK(std::ranges::none_of(layout.ribbons, [file](const FlowRibbon& ribbon) {
        return ribbon.record == file;
      }));
      for (std::size_t i = 0; i < records.size(); ++i) {
        if (records[i].kind != FlowRecordKind::Files) {
          TEST_CHECK(!flowFocus(layout.topology, i)[file]);
        }
      }
    }
    const auto b = flowRecordFor(tree, layout.topology, 2).value();
    TEST_CHECK(!flowFocus(layout.topology, b)[files[0]]);
  }

  void filesBarClosesItsColumn(const Tree& wide) {
    const auto layout = placeWide(wide, 1120, 295);
    for (std::size_t column = 1; column < kFlowColumns; ++column) {
      const auto& indexes = layout.topology.columns[column];
      const auto& last = layout.topology.records[indexes.back()];
      TEST_CHECK(last.kind == FlowRecordKind::Files);
      for (const auto index : indexes) {
        const auto& record = layout.topology.records[index];
        if (index != indexes.back()) {
          TEST_CHECK(record.y + record.height <= last.y + 1e-6);
        }
      }
      TEST_CHECK(last.y + last.height <= 295 - kEdge + 1e-6);
    }
    const auto& second = layout.topology.records[layout.topology.columns[2].back()];
    TEST_CHECK(std::abs(second.y + second.height - (295 - kEdge)) < 1e-6);
  }

  void sparseColumnsStayCalm() {
    const auto tree = flowTree(
        {{.parent = -1, .name = "x", .folder = true},
         {.parent = 0, .name = "only", .folder = true, .bytes = 10, .items = 1},
         {.parent = 1, .name = "inner", .folder = true, .bytes = 10, .items = 1},
         {.parent = 2, .name = "leaf", .folder = true, .bytes = 10, .items = 1},
         {.parent = 3, .name = "z", .bytes = 10}},
        2, 1
    );
    const auto layout = placeFlow(buildFlowTopology(tree, 0, {}), 1120, 275, 0);
    const double band = 275 - 2 * kEdge;
    for (std::size_t column = 1; column < kFlowColumns; ++column) {
      double total = 0;
      for (const auto index : layout.topology.columns[column]) {
        total += layout.topology.records[index].height;
        TEST_CHECK(layout.topology.records[index].height >= kMinBar - 1e-9);
      }
      TEST_CHECK(total <= band * 0.5 + 1e-6 || layout.topology.columns[column].size() > 2);
    }
    TEST_CHECK(layout.topology.records[0].height <= band * 0.5 + 1e-6);
  }

  void everyIntegerHeightLaysOut(const Tree& wide) {
    std::size_t passes = 0;
    for (int height = 180; height <= 420; ++height) {
      const auto band = static_cast<double>(height);
      const auto options = flowOptions(band);
      const auto layout = placeFlow(buildFlowTopology(wide, 0, {}, options), 900, band, 168);
      for (std::size_t column = 1; column < kFlowColumns; ++column) {
        TEST_CHECK(layout.topology.columns[column].size() <= options.perColumn);
      }
      barsStayInsideTheBand(layout, band);
      labelsDoNotOverlap(layout, band, true);
      for (const auto& ribbon : layout.ribbons) {
        const auto& parent = layout.topology.records[layout.topology.records[ribbon.record].parent];
        TEST_CHECK(ribbon.parentTop >= parent.y - 1e-6 && ribbon.parentBottom <= parent.y + parent.height + 1e-6);
      }
      ++passes;
    }
    TEST_CHECK(passes == 241);
  }

  void unusableSizesGiveAnEmptyLayout(const Tree& wide) {
    const double nan = std::nan("");
    const double inf = std::numeric_limits<double>::infinity();
    for (const auto [width, height] : std::array<std::pair<double, double>, 6>{
             {{0, 295}, {1120, 0}, {-5, 295}, {nan, 295}, {1120, inf}, {inf, 295}}
         }) {
      const auto layout = placeFlow(buildFlowTopology(wide, 0, {}), width, height, 0);
      TEST_CHECK(layout.topology.records.empty() && layout.ribbons.empty() && layout.labels.empty());
    }
    const auto unknown = placeFlow(buildFlowTopology(wide, 999999, {}), 1120, 295, 0);
    TEST_CHECK(unknown.topology.records.empty() && unknown.labels.empty());
    for (const double stem : {nan, -1.0, inf, -inf}) {
      const auto badStem = placeFlow(buildFlowTopology(wide, 0, {}), 1120, 295, stem);
      TEST_CHECK(badStem.topology.records.empty() && badStem.ribbons.empty() && badStem.labels.empty());
    }
  }

  // The current folder and one folder below it, valid until a test breaks it.
  FlowTopology pairTopology() {
    FlowTopology topology;
    topology.folder = 0;
    topology.records.resize(2);
    topology.records[0].kind = FlowRecordKind::Current;
    topology.records[0].weight = 1;
    topology.records[0].kids = {1};
    topology.records[1].column = 1;
    topology.records[1].parent = 0;
    topology.records[1].weight = 1;
    topology.columns[0] = {0};
    topology.columns[1] = {1};
    return topology;
  }

  void brokenTopologyGivesAnEmptyLayout(const Tree& wide) {
    const auto valid = placeFlow(pairTopology(), 900, 280, 0);
    TEST_CHECK(valid.topology.records.size() == 2 && valid.ribbons.size() == 1 && valid.labels.size() == 2);

    std::vector<FlowTopology> broken(9, pairTopology());
    broken[0].records[1].parent = 7;
    broken[1].columns[1] = {5};
    broken[2].columns[2] = {1};
    broken[3].records[1].parent = 1;
    broken[4].records[0].kind = FlowRecordKind::Folder;
    broken[5].columns[0] = {0, 1};
    broken[6].columns[0].clear();
    broken[7].columns[0] = {1};
    // Listed in column 2 under a column 1 parent, but the record says column 1.
    broken[8].records.push_back(broken[8].records[1]);
    broken[8].records[2].parent = 1;
    broken[8].columns[2] = {2};
    for (auto& topology : broken) {
      const auto layout = placeFlow(std::move(topology), 900, 280, 0);
      TEST_CHECK(layout.topology.records.empty() && layout.ribbons.empty() && layout.labels.empty());
    }

    auto dangling = pairTopology();
    dangling.columns[1] = {9};
    TEST_CHECK(!flowRecordFor(wide, dangling, childNamed(wide, 0, "t7")).has_value());
  }

  void hugeStemRoundsColumnsToTheNearestPixel(const Tree& wide) {
    // The first bar starts at 2^52 + 1, where adding a half before flooring would land on 2^52 + 2.
    const auto layout = placeFlow(buildFlowTopology(wide, 0, {}), 1120, 295, 4503599627370477.0);
    TEST_NEAR(layout.columnX[0], 4503599627370497.0, 0.0);
  }

  void layoutPlacesTheStemOnTheCurrentBar(const Tree& wide) {
    const auto tree = wideFrom(wide, 2, {1, 0});
    const auto layout = flowLayout(tree, {}, 900, 280);
    TEST_CHECK((layout.columnX == std::array<double, 4>{178, 361, 545, 728}));
    TEST_CHECK(layout.stem.size() == 2);
    const auto& root = layout.stem[0];
    TEST_CHECK(root.level == 2 && root.node == 0 && root.labeled && root.kind == FlowPillKind::Pill);
    TEST_NEAR(root.x, 12.0, 1e-9);
    TEST_NEAR(root.y, 129.0, 1e-9);
    TEST_NEAR(root.width, 70.0, 1e-9);
    TEST_NEAR(root.height, 22.0, 0.0);
    TEST_NEAR(root.alpha, 0.9, 0.0);
    TEST_NEAR(root.textWidth, 58.0, 1e-9);
    const auto& parent = layout.stem[1];
    TEST_CHECK(parent.level == 1 && parent.node == 1);
    TEST_NEAR(parent.x, 84.0, 1e-9);
    TEST_NEAR(parent.y, 126.0, 1e-9);
    TEST_NEAR(parent.width, 86.0, 1e-9);
    TEST_NEAR(parent.alpha, 1.0, 0.0);
    const auto& current = layout.topology.records[0];
    TEST_CHECK(current.node == 2);
    TEST_NEAR(current.x, 178.0, 0.0);
    TEST_NEAR(current.y, 35.59999999999998, 1e-9);
    TEST_NEAR(current.height, 208.80000000000004, 1e-9);
    TEST_NEAR(recordOf(layout, 1, 0).height, 74.98316341024818, 1e-9);
    TEST_NEAR(recordOf(layout, 1, 1).y, 92.98316341024818, 1e-9);
    TEST_CHECK(layout.links.size() == 2);
    TEST_NEAR(layout.links[0].x0, 82.0, 1e-9);
    TEST_NEAR(layout.links[0].x1, 84.0, 1e-9);
    TEST_NEAR(layout.links[0].centerY, 140.0, 1e-9);
    TEST_NEAR(layout.links[1].x0, 170.0, 1e-9);
    TEST_NEAR(layout.links[1].x1, 178.0, 0.0);
    TEST_NEAR(layout.labels[0].x, 200.0, 0.0);
    TEST_NEAR(layout.labels[0].maxWidth, 155.0, 0.0);
    TEST_NEAR(layout.labels[1].centerY, 49.49158170512409, 1e-9);
    TEST_NEAR(layout.labels[2].centerY, 119.49504043326456, 1e-9);

    const auto deep = wideFrom(wide, 2, {1, 0, 0, 0, 0, 0});
    const auto deepLayout = flowLayout(deep, {}, 900, 280);
    TEST_CHECK(deepLayout.stem.size() == 6 && deepLayout.stem[0].kind == FlowPillKind::Dot);
    TEST_CHECK(!deepLayout.stem[0].labeled);
    TEST_NEAR(deepLayout.stem[0].textWidth, 0.0, 0.0);
    TEST_NEAR(deepLayout.stem[0].alpha, 0.55, 0.0);
    TEST_NEAR(deepLayout.stem[1].alpha, 0.55, 0.0);
    TEST_NEAR(deepLayout.stem[2].alpha, 0.66, 0.0);
  }

  void emptyFolderLayoutHasOneLabel(const Tree& wide) {
    const auto tree = wideFrom(wide, childNamed(wide, 0, "t0"), {0});
    const auto layout = flowLayout(tree, {}, 900, 280);
    TEST_CHECK(layout.topology.empty == FlowEmpty::Empty);
    TEST_CHECK((layout.columnX == std::array<double, 4>{106, 313, 521, 728}));
    const auto& current = layout.topology.records[0];
    TEST_NEAR(current.x, 106.0, 0.0);
    TEST_NEAR(current.y, 112.0, 1e-9);
    TEST_NEAR(current.height, 56.0, 0.0);
    TEST_CHECK(layout.labels.size() == 1);
    TEST_CHECK(layout.labels[0].record == 0 && layout.labels[0].lines == 2);
    TEST_NEAR(layout.labels[0].x, 128.0, 0.0);
    TEST_NEAR(layout.labels[0].centerY, 140.0, 1e-9);
    TEST_NEAR(layout.labels[0].height, 31.0, 0.0);
    TEST_NEAR(layout.labels[0].maxWidth, 160.0, 0.0);
    TEST_CHECK(layout.stem.size() == 1 && layout.links.size() == 1);
    TEST_NEAR(layout.stem[0].x, 12.0, 0.0);
    TEST_NEAR(layout.stem[0].y, 126.0, 1e-9);
    TEST_NEAR(layout.links[0].x0, 98.0, 1e-9);
    TEST_NEAR(layout.links[0].x1, 106.0, 0.0);
  }

  void unreadableFolderLayoutHasOneLabel(const Tree& wide) {
    auto tree = wideFrom(wide, childNamed(wide, 0, "d3"), {0});
    tree.nodes[tree.root].unreadable = true;
    tree.children.clear();
    for (auto& node : tree.nodes) {
      node.childCount = 0;
    }
    const auto layout = flowLayout(tree, {}, 900, 280);
    TEST_CHECK(layout.topology.empty == FlowEmpty::Unreadable);
    TEST_CHECK(layout.labels.size() == 1 && layout.labels[0].record == 0 && layout.labels[0].lines == 2);
    TEST_NEAR(layout.labels[0].maxWidth, 160.0, 0.0);
    TEST_CHECK(layout.ribbons.empty());
  }

  void layoutRefusesUnusableInput(const Tree& wide) {
    const double nan = std::nan("");
    const double inf = std::numeric_limits<double>::infinity();
    for (const auto [width, height] :
         std::array<std::pair<double, double>, 5>{{{0, 280}, {900, -1}, {nan, 280}, {900, nan}, {inf, 280}}}) {
      const auto layout = flowLayout(wide, {}, width, height);
      TEST_CHECK(layout.topology.records.empty() && layout.stem.empty() && layout.links.empty());
    }
    auto unknown = wide;
    unknown.root = kNoNode;
    TEST_CHECK(flowLayout(unknown, {}, 900, 280).topology.records.empty());
    const auto atRoot = flowLayout(wide, {}, 900, 280);
    TEST_CHECK(atRoot.stem.empty() && atRoot.links.empty() && !atRoot.topology.records.empty());
    TEST_NEAR(atRoot.columnX[0], 12.0, 0.0);
    TEST_CHECK(atRoot.topology.columns[1].size() <= flowOptions(280).firstMax + 2);
  }

  void shortBandKeepsBarsAtTheirFloor(const Tree& wide) {
    const auto layout = placeWide(wide, 1120, 60);
    TEST_CHECK(layout.topology.columns[1].size() == 10 && layout.topology.columns[2].size() == 12);
    TEST_NEAR(layout.topology.records[0].y, -33.0, 1e-9);
    TEST_NEAR(layout.topology.records[0].height, 126.0, 1e-9);
    TEST_NEAR(recordOf(layout, 1, 0).y, -49.0, 1e-9);
    TEST_NEAR(recordOf(layout, 1, 1).y, -33.0, 1e-9);
    TEST_NEAR(recordOf(layout, 2, 0).y, -142.0, 1e-9);
    TEST_NEAR(recordOf(layout, 3, 2).y, -78.0, 1e-9);
    for (std::size_t column = 1; column < kFlowColumns; ++column) {
      for (const auto index : layout.topology.columns[column]) {
        TEST_NEAR(layout.topology.records[index].height, kMinBar, 0.0);
      }
    }
  }

} // namespace

int main() {
  capacityFollowsTheBand();
  capacityFitsEveryIntegerHeight();
  closeLabelsAreSpreadInsideTheBox();
  const auto wide = wideTree();
  barsAreAtLeastFourteenAndMoreIsCapped(wide);
  heightsFollowWeight(wide);
  placementMatchesTheWorkedExample(wide);
  ribbonsKeepTheirParentsOrder(wide);
  ribbonsMatchTheirBars(wide);
  labelsSitRightOfTheirBars(wide);
  oneFolderChainAndEmptyFolder();
  filesHaveNoParentRibbonOrPath();
  filesBarClosesItsColumn(wide);
  sparseColumnsStayCalm();
  everyIntegerHeightLaysOut(wide);
  unusableSizesGiveAnEmptyLayout(wide);
  brokenTopologyGivesAnEmptyLayout(wide);
  hugeStemRoundsColumnsToTheNearestPixel(wide);
  layoutPlacesTheStemOnTheCurrentBar(wide);
  emptyFolderLayoutHasOneLabel(wide);
  unreadableFolderLayoutHasOneLabel(wide);
  layoutRefusesUnusableInput(wide);
  shortBandKeepsBarsAtTheirFloor(wide);
  std::println("workbench flow layout checks passed");
  return 0;
}
