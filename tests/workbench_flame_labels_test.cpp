#include "test_check.h"
#include "workbench/flame_scene.h"
#include "workbench/scene_shapes.h"
#include "workbench_flame_fixture.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// Name and value labels of the strata ahead: the value is drawn whole or not at all.
namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::flame;

  // 7e7 bytes as the measure writes it.
  constexpr std::string_view kValue = "66.8 MiB";

  // Names 7 px a code point, values 6: the value above is 48 px wide.
  FlameFonts fonts(double value = 6) {
    LabelFont name;
    name.ascii.fill(7);
    name.other = 7;
    LabelFont small;
    small.ascii.fill(value);
    small.other = value;
    return {.firstAhead = name, .ahead = name, .ancestor = name, .value = small};
  }

  // Ten equal 7e7-byte folders: across 980 px each gets 96 px, a 95 px box with 83 px inside its padding.
  Tree tenFolders(std::string_view stem) {
    std::vector<Row> rows{{.parent = -1, .name = "root"}};
    for (int i = 0; i < 10; ++i) {
      rows.push_back({.parent = 0, .name = std::format("{}{}", stem, i), .bytes = 7e7});
    }
    return buildTree(rows);
  }

  std::vector<const ShapeLabel*> labelsOf(const SceneShapes& shapes, NodeId node) {
    std::vector<const ShapeLabel*> out;
    for (const auto& label : shapes.labels) {
      if (label.node == node) {
        out.push_back(&label);
      }
    }
    return out;
  }

  double middleOf(const ShapeLabel& label) { return label.y + label.height / 2; }

  // A 120 px band makes the first stratum 32 px tall: name and value share one line. A short name keeps the value
  // whole at the right end and fits in the room before it.
  void testInlineValueAtTheRightEnd() {
    const auto tree = tenFolders("a");
    const auto scene = flameScene(tree, {}, 980, 120, fonts());
    const auto& row = scene.rows[1];
    TEST_CHECK(row.row.kind == FlameRowKind::Ahead && row.row.h == 32 && row.items.size() == 10);
    const auto& item = row.items[0];
    TEST_CHECK(item.label == FlameLabel::NameValue);
    TEST_NEAR(item.valueWidth, 48.0, 1e-9);
    const auto shapes = flameShapes(tree, {}, scene, {}, {});
    const auto labels = labelsOf(shapes, item.node);
    TEST_CHECK(labels.size() == 2);
    if (labels.size() != 2) {
      return;
    }
    const auto& name = *labels[0];
    const auto& value = *labels[1];
    TEST_CHECK(name.text == "a0" && name.align == LabelAlign::Start);
    // Box 8 to 103: the name from 14 in the 83 px inside, less the value and an 8 px gap.
    TEST_NEAR(name.x, 14.0, 1e-4);
    TEST_NEAR(name.width, 27.0, 1e-4);
    TEST_CHECK(value.text == kValue && value.align == LabelAlign::End && value.fontSize == 11);
    // The view cuts the name in the middle when it must and shows the value whole or not at all.
    TEST_CHECK(name.fit == LabelFit::Shorten && value.fit == LabelFit::Whole);
    TEST_NEAR(value.x, 14.0, 1e-4);
    TEST_NEAR(value.width, 83.0, 1e-4);
    TEST_NEAR(middleOf(name), middleOf(value), 1e-4);
    TEST_CHECK(value.color.a < name.color.a);
  }

  // A longer name gets the room before the value, where the view cuts it in the middle: with 24 px of value,
  // "corefolder0" has 51 px, room for "cor…er0" at 49 px.
  void testInlineNameRoomBeforeTheValue() {
    const auto tree = tenFolders("corefolder");
    const auto scene = flameScene(tree, {}, 980, 120, fonts(3));
    const auto& item = scene.rows[1].items[0];
    TEST_CHECK(item.label == FlameLabel::NameValue);
    const auto shapes = flameShapes(tree, {}, scene, {}, {});
    const auto labels = labelsOf(shapes, item.node);
    TEST_CHECK(labels.size() == 2 && labels[0]->text == "corefolder0" && labels[1]->text == kValue);
    TEST_NEAR(labels[0]->width, 51.0, 1e-4);
  }

  // With 56 px of value the name has 19 px: "a0" fits whole, while "core0" is too short to cut with three code
  // points on each side, so it shows alone across the whole box and no label of the folder holds any part of the
  // value.
  void testInlineValueDroppedForALongName() {
    const auto shortNames = tenFolders("a");
    TEST_CHECK(flameScene(shortNames, {}, 980, 120, fonts(7)).rows[1].items[0].label == FlameLabel::NameValue);
    const auto tree = tenFolders("core");
    const auto scene = flameScene(tree, {}, 980, 120, fonts(7));
    const auto& item = scene.rows[1].items[0];
    TEST_CHECK(item.label == FlameLabel::Name);
    const auto shapes = flameShapes(tree, {}, scene, {}, {});
    const auto labels = labelsOf(shapes, item.node);
    TEST_CHECK(labels.size() == 1);
    if (labels.size() != 1) {
      return;
    }
    TEST_CHECK(labels[0]->text == "core0");
    TEST_NEAR(labels[0]->width, 83.0, 1e-4);
    // Only the current folder's bar writes a size.
    for (const auto& label : shapes.labels) {
      TEST_CHECK(label.node == tree.root || !label.text.contains("MiB"));
    }
  }

  // The fit is decided by the fonts: with a wider value font the same short name loses its value.
  void testInlineValueDroppedWhenTheValueIsWide() {
    const auto tree = tenFolders("a");
    // 8 code points at 8 px: 64 px of value leaves 11 px for the name, under the narrowest fit.
    const auto scene = flameScene(tree, {}, 980, 120, fonts(8));
    TEST_CHECK(scene.rows[1].items[0].label == FlameLabel::Name);
    const auto wide = flameScene(tree, {}, 980, 120, fonts(5.5));
    TEST_CHECK(wide.rows[1].items[0].label == FlameLabel::NameValue);
  }

  // A 224 px band makes the first stratum 64 px tall: the name above, the value below it on its own line.
  void testStackedValueOnItsOwnLine() {
    const auto tree = tenFolders("core");
    const auto scene = flameScene(tree, {}, 980, 224, fonts());
    const auto& row = scene.rows[1];
    TEST_CHECK(row.row.h == 64);
    const auto& item = row.items[0];
    TEST_CHECK(item.label == FlameLabel::StackedNameValue);
    const auto shapes = flameShapes(tree, {}, scene, {}, {});
    const auto labels = labelsOf(shapes, item.node);
    TEST_CHECK(labels.size() == 2);
    if (labels.size() != 2) {
      return;
    }
    const auto& name = *labels[0];
    const auto& value = *labels[1];
    const double middle = row.row.y + row.row.h / 2;
    TEST_CHECK(name.text == "core0" && name.align == LabelAlign::Start && name.fontSize == 13);
    TEST_CHECK(value.text == kValue && value.align == LabelAlign::Start && value.fontSize == 11);
    TEST_CHECK(name.fit == LabelFit::Shorten && value.fit == LabelFit::Whole);
    TEST_NEAR(middleOf(name), middle - 7, 1e-4);
    TEST_NEAR(middleOf(value), middle + 8, 1e-4);
    TEST_NEAR(name.width, 83.0, 1e-4);
    TEST_NEAR(value.width, 83.0, 1e-4);
  }

  // A value wider than the inside of the box is not drawn under the name.
  void testStackedValueDroppedWhenItCannotFit() {
    const auto tree = tenFolders("core");
    // 8 code points at 11 px: 88 px of value in 83.
    const auto scene = flameScene(tree, {}, 980, 224, fonts(11));
    const auto& item = scene.rows[1].items[0];
    TEST_CHECK(item.label == FlameLabel::Name);
    const auto shapes = flameShapes(tree, {}, scene, {}, {});
    const auto labels = labelsOf(shapes, item.node);
    TEST_CHECK(labels.size() == 1 && labels[0]->text == "core0");
  }

  // Every label in a crowded scene holds either a name or a whole value, never the two joined.
  void testNoLabelJoinsNameAndValue() {
    const auto tree = tenFolders("core");
    for (const double band : {120.0, 160.0, 224.0}) {
      const auto scene = flameScene(tree, {}, 980, band, fonts());
      const auto shapes = flameShapes(tree, {}, scene, {}, {});
      for (const auto& label : shapes.labels) {
        const bool value = label.text.contains("MiB");
        TEST_CHECK(!value || label.text == kValue || label.node == tree.root);
      }
    }
  }
} // namespace

int main() {
  testInlineValueAtTheRightEnd();
  testInlineNameRoomBeforeTheValue();
  testInlineValueDroppedForALongName();
  testInlineValueDroppedWhenTheValueIsWide();
  testStackedValueOnItsOwnLine();
  testStackedValueDroppedWhenItCannotFit();
  testNoLabelJoinsNameAndValue();
  std::println("workbench flame label checks passed");
  return 0;
}
