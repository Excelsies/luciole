#include "test_check.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <format>
#include <print>
#include <string>
#include <vector>

// Counts, sizes and values in the flame graph, the ladder and the flow diagram show whole or not at all, while names
// may be cut.
namespace {
  using namespace luciole::workbench;
  using luciole::test::WorkbenchRow;

  const LayoutSettings kSettings{};
  const ColorPalette kPalette{};

  // top > here; here holds one large folder and `count` small folders of 10 kB.
  Tree crowded(std::size_t count) {
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "top", .folder = true, .bytes = 9e6, .items = 2},
        {.parent = 0, .name = "here", .folder = true, .bytes = 8e6, .items = static_cast<double>(count) + 1},
        {.parent = 0, .name = "other", .folder = true, .bytes = 1e6},
        {.parent = 1, .name = "big", .folder = true, .bytes = 5e6},
    };
    std::deque<std::string> names;
    for (std::size_t i = 0; i < count; ++i) {
      names.push_back(std::format("s{:03}", i));
      rows.push_back({.parent = 1, .name = names.back(), .folder = true, .bytes = 1e4});
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, tree.root);
    return tree;
  }

  const ShapeLabel* labelWhere(const SceneShapes& out, NodeId node, auto pick) {
    const auto found = std::ranges::find_if(out.labels, [&](const ShapeLabel& l) { return l.node == node && pick(l); });
    return found == out.labels.end() ? nullptr : &*found;
  }

  // The current folder's bar shows its size and item count whole, else its size alone; a merged piece its count.
  void flameCountsAreWhole() {
    const auto tree = crowded(60);
    LabelFont font;
    font.ascii.fill(7);
    font.other = 7;
    const auto scene =
        flameScene(tree, kSettings, 1000, 266, {.firstAhead = font, .ahead = font, .ancestor = font, .value = font});
    const auto out = flameShapes(tree, kSettings, scene, {}, kPalette);
    const auto* facts = labelWhere(out, tree.root, [](const ShapeLabel& l) { return l.text.ends_with("61 items"); });
    TEST_CHECK(facts != nullptr);
    if (facts != nullptr) {
      TEST_CHECK(facts->fit == LabelFit::Whole && facts->shortText == "7.6 MiB");
      TEST_CHECK(facts->text == "7.6 MiB  ·  61 items");
    }
    std::size_t merged = 0;
    for (const auto& row : scene.rows) {
      for (const auto& item : row.items) {
        if (!item.more) {
          continue;
        }
        const auto count = std::format("+{}", item.list.size());
        const auto* label = labelWhere(out, item.owner, [&](const ShapeLabel& l) { return l.text == count; });
        TEST_CHECK(label == nullptr || label->fit == LabelFit::Whole);
        merged += label != nullptr ? 1 : 0;
      }
    }
    TEST_CHECK(merged > 0);
    const auto* name = labelWhere(out, 3, [](const ShapeLabel& l) { return l.text == "big"; });
    TEST_CHECK(name != nullptr && name->fit == LabelFit::Shorten);
  }

  // The remainder of a crowded bar is labeled with its count, whole: in "big", the bar of "here" has 300 more.
  void ladderRestIsWhole() {
    auto tree = crowded(300);
    tree.root = 3;
    tree.ancestors = parentChain(tree, tree.root);
    const auto scene = ladderScene(tree, kSettings, 544, 100);
    const auto out = ladderShapes(tree, kSettings, scene, {}, kPalette);
    std::size_t rests = 0;
    for (const auto& row : scene.rows) {
      if (!row.rest || !row.restLabeled) {
        continue;
      }
      const auto count = std::format("+{}", row.rest->count);
      const auto* label = labelWhere(out, row.folder, [&](const ShapeLabel& l) { return l.text == count; });
      TEST_CHECK(label != nullptr && label->fit == LabelFit::Whole);
      ++rests;
    }
    TEST_CHECK(rests > 0);
  }

  // A record's value under its name is whole; the name may be cut.
  void flowValuesAreWhole() {
    const auto tree = crowded(60);
    const auto layout = flowLayout(tree, kSettings, 900, 300);
    const auto out = flowShapes(tree, kSettings, layout, {}, kPalette, LabelFont{});
    std::size_t values = 0;
    for (const auto& label : layout.labels) {
      if (label.lines != 2) {
        continue;
      }
      const auto node = layout.topology.records[label.record].node;
      const auto value = flowRecordValue(tree, layout.topology, label.record, kSettings.sizeBy);
      const auto name = flowRecordName(tree, layout.topology, label.record);
      const auto* shown = labelWhere(out, node, [&](const ShapeLabel& l) { return l.text == value; });
      const auto* named = labelWhere(out, node, [&](const ShapeLabel& l) { return l.text == name; });
      TEST_CHECK(shown != nullptr && shown->fit == LabelFit::Whole);
      TEST_CHECK(named != nullptr && named->fit == LabelFit::Shorten);
      ++values;
    }
    TEST_CHECK(values > 0);
    // A merged record is named by its count, whole.
    std::size_t merged = 0;
    for (std::size_t i = 0; i < layout.topology.records.size(); ++i) {
      const auto& record = layout.topology.records[i];
      const auto name = flowRecordName(tree, layout.topology, i);
      const auto* shown = labelWhere(out, record.node, [&](const ShapeLabel& l) { return l.text == name; });
      if (record.kind == FlowRecordKind::More && shown != nullptr) {
        TEST_CHECK(shown->fit == LabelFit::Whole);
        ++merged;
      }
    }
    TEST_CHECK(merged > 0);
  }
} // namespace

int main() {
  flameCountsAreWhole();
  ladderRestIsWhole();
  flowValuesAreWhole();
  std::println("workbench whole label checks passed");
  return 0;
}
