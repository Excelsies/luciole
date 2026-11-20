#include "test_check.h"
#include "workbench/flame_scene.h"
#include "workbench_flame_fixture.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <print>
#include <string>
#include <tuple>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::flame;
  using luciole::WorkbenchMeasure;

  void testHitLookup() {
    const auto bands = flameBands(260, 3);
    TEST_CHECK(!flameRowAt(bands.rows, 2).has_value());
    const auto& row = bands.rows[1];
    TEST_CHECK(flameRowAt(bands.rows, row.y + 1) == 1U);
    TEST_CHECK(flameRowAt(bands.rows, row.y + row.h + 0.2) == 1U);
    TEST_CHECK(flameRowAt(bands.rows, row.y + row.h + 0.8) == 2U);
    TEST_CHECK(!flameRowAt(bands.rows, kNan).has_value());
    TEST_CHECK(!flameRowAt({}, 10).has_value());
    std::vector<FlameItem> items(3);
    items[0].x = 0;
    items[0].w = 10;
    items[1].x = 10;
    items[1].w = 5;
    items[2].x = 15;
    items[2].w = 40;
    TEST_CHECK(flameItemAt(items, 12) == 1U);
    TEST_CHECK(flameItemAt(items, 0) == 0U);
    TEST_CHECK(flameItemAt(items, 54.9) == 2U);
    TEST_CHECK(!flameItemAt(items, 55).has_value());
    TEST_CHECK(!flameItemAt(items, -1).has_value());
    TEST_CHECK(!flameItemAt(items, kNan).has_value());
    TEST_CHECK(!flameItemAt({}, 3).has_value());
  }

  void testFirstLevelAncestor() {
    const auto tree = wideTree();
    const auto d = childAt(tree, 0, 0);
    const auto e = childAt(tree, d, 0);
    TEST_CHECK(flameFirstLevel(tree, e, 0) == d);
    TEST_CHECK(flameFirstLevel(tree, d, 0) == d);
    TEST_CHECK(flameFirstLevel(tree, 0, 0) == kNoNode);
    TEST_CHECK(flameFirstLevel(tree, e, d + 1) == kNoNode);
    TEST_CHECK(flameFirstLevel(tree, kNoNode, 0) == kNoNode);
    // A parent cycle ends after one step per node.
    auto cycle = buildTree({{.parent = -1, .name = "a"}, {.parent = 0, .name = "b"}});
    cycle.nodes[0].parent = 1;
    TEST_CHECK(flameFirstLevel(cycle, 1, 7) == kNoNode);
  }

  void testBoxPixelAligned() {
    const auto box = flameBox(10.4, 100.2, 24, false);
    TEST_CHECK(box.x == 10 && box.w == 100);
    TEST_CHECK(flameBox(10, 3, 24, false).w == 3);
    TEST_CHECK(flameBox(10, 0.2, 24, false).w >= 1);
    TEST_CHECK(
        flameBox(0, 50, 24, false).r == 3 && flameBox(0, 50, 10, false).r == 2 && flameBox(0, 50, 4, false).r == 1
    );
    TEST_CHECK(flameBox(8, 100, 26, true).w == 100);
    TEST_CHECK(flameBox(2.5, 10, 24, false).x == 3 && flameBox(-2.5, 10, 24, false).x == -2);
  }

  void checkSceneRow(
      const Tree& tree, const FlameSceneRow& row, FlameRowKind kind, std::size_t level, double y, double h,
      const std::vector<Expected>& items
  ) {
    TEST_CHECK(row.row.kind == kind && row.row.level == level && row.row.y == y && row.row.h == h);
    TEST_CHECK(row.items.size() == items.size());
    checkItems(tree, row.items, items);
  }

  void testChainScenesCarryTheClickedSegment() {
    using enum FlameRowKind;
    auto tree = chainTree();
    const auto fonts = sevenPixelFonts();
    const auto start = flameScene(tree, {}, 1000, 266, fonts);
    TEST_CHECK(start.rows.size() == 4 && !start.empty && !start.truncated);
    checkSceneRow(tree, start.rows[0], Bar, 0, 8, 26, {{"bar", 8, 980}});
    checkSceneRow(
        tree, start.rows[1], Ahead, 1, 35, 64,
        {{"a", 8, 676.7459279462958}, {"a2", 684.7459279462958, 303.25407205370425}}
    );
    checkSceneRow(
        tree, start.rows[2], Ahead, 2, 100, 49,
        {{"b", 8, 499.4085140443887}, {"b2", 507.4085140443887, 177.33741390190704}}
    );
    checkSceneRow(tree, start.rows[3], Ahead, 3, 150, 34, {{"c", 8, 499.4085140443887}});
    TEST_CHECK(start.rows[2].items[0].level == 1);
    // Every stratum ahead is at least 34 px tall here and every segment at least 90 px wide: name over value.
    for (const std::size_t row : {1U, 2U, 3U}) {
      for (const auto& item : start.rows[row].items) {
        TEST_CHECK(item.label == FlameLabel::StackedNameValue);
      }
    }
    enter(tree, 2);
    const auto end = flameScene(tree, {}, 1000, 266, fonts);
    TEST_CHECK(end.rows.size() == 6);
    checkSceneRow(
        tree, end.rows[0], Ancestor, 2, 8, 16,
        {{"a", 8, 676.7459279462958}, {"a2", 684.7459279462958, 303.25407205370425}}
    );
    checkSceneRow(
        tree, end.rows[1], Ancestor, 1, 25, 24,
        {{"b", 8, 723.1965846454856}, {"b2", 731.1965846454856, 256.8034153545144}}
    );
    checkSceneRow(tree, end.rows[2], Bar, 0, 53, 26, {{"bar", 8, 980}});
    checkSceneRow(tree, end.rows[3], Ahead, 1, 80, 64, {{"c", 8, 980}});
    checkSceneRow(
        tree, end.rows[4], Ahead, 2, 145, 49, {{"d", 8, 620.5417554678834}, {"e", 628.5417554678834, 359.4582445321166}}
    );
    checkSceneRow(tree, end.rows[5], Ahead, 3, 195, 34, {});
    TEST_CHECK(end.rows[2].items[0].bar && end.rows[2].items[0].node == 2);
    TEST_CHECK(end.rows[3].row.y < start.rows[3].row.y);
    TEST_CHECK(end.rows[0].items[0].pinned && end.rows[1].items[0].pinned && !end.rows[1].items[1].pinned);
    TEST_NEAR(end.rows[0].items[0].strength, 0.85, 1e-12);
    TEST_NEAR(end.rows[0].items[1].strength, 0.68, 1e-12);
    TEST_NEAR(end.rows[1].items[0].strength, 0.85, 1e-12);
    TEST_NEAR(end.rows[1].items[1].strength, 0.8, 1e-12);
    TEST_NEAR(end.rows[4].items[0].strength, 0.8, 1e-12);
    TEST_NEAR(start.rows[3].items[0].strength, 0.62, 1e-12);
  }

  void testWideScenes() {
    using enum FlameRowKind;
    auto tree = wideTree();
    const auto fonts = sevenPixelFonts();
    const auto root = flameScene(tree, {}, 1116, 281, fonts);
    TEST_CHECK(root.rows.size() == 4 && !root.empty && !root.truncated);
    TEST_CHECK(root.x0 == 8 && root.x1 == 1104);
    TEST_CHECK(root.rows[1].items.size() == 13 && root.rows[2].items.size() == 32 && root.rows[3].items.empty());
    checkItems(
        tree, root.rows[1].items, {{"d0", 8, 175.79139372584348}, {"d1", 183.79139372584348, 124.30334872934381}}
    );
    checkItems(
        tree, root.rows[2].items,
        {{"e0", 8, 52.27596553643377},
         {"e1", 60.27596553643377, 36.964874545024934},
         {"e2", 97.24084008145871, 30.18184458550823},
         {"e3", 127.42268466696694, 30}}
    );
    enter(tree, childAt(tree, childAt(tree, 0, 30), 9));
    const auto deep = flameScene(tree, {}, 1116, 224, fonts);
    TEST_CHECK(deep.empty && deep.rows.size() == 6);
    TEST_CHECK(deep.rows[0].row.kind == Ancestor && deep.rows[0].row.level == 2 && deep.rows[0].items.size() == 14);
    TEST_CHECK(deep.rows[1].items.size() == 10);
    TEST_CHECK(deep.rows[3].row.h == 57 && deep.rows[4].row.h == 44 && deep.rows[5].row.h == 31);
    checkItems(
        tree, deep.rows[0].items, {{"d0", 8, 144.63939086364007}, {"d1", 152.63939086364007, 102.27554524405458}}
    );
    checkItems(
        tree, deep.rows[1].items, {{"e0", 8, 207.44581092881543}, {"e1", 215.44581092881543, 146.6870730590382}}
    );
    TEST_NEAR(deep.rows[0].items[0].strength, 0.68, 1e-12);
  }

  // R0 > R > P > Q (the current folder). Q holds x and y, heavy, and 60 light folders; x > x1 > aa, bb;
  // y > c0..c9; c0 > k, m; c1 > long1, long2.
  Tree labelTree() {
    std::vector<Row> rows{{.parent = -1, .name = "R0"}};
    const auto add = [&rows](int parent, std::string name, double bytes) {
      rows.push_back({.parent = parent, .name = std::move(name), .bytes = bytes});
      return static_cast<int>(rows.size() - 1);
    };
    const int r = add(0, "R", 10);
    add(0, "r2", 10);
    const int p = add(r, "P", 10);
    add(r, "p2", 10);
    const int q = add(p, "Q", 10);
    add(p, "q2", 10);
    const int x = add(q, "x", 999999);
    const int y = add(q, "y", 999999);
    for (int i = 0; i < 60; ++i) {
      add(q, std::format("z{:02}", i), 2899);
    }
    const int x1 = add(x, "x1", 100);
    add(x1, "aa", 10);
    add(x1, "bb", 10);
    std::vector<int> cs;
    cs.reserve(10);
    for (int i = 0; i < 10; ++i) {
      cs.push_back(add(y, std::format("c{}", i), 10));
    }
    add(cs[0], "k", 10);
    add(cs[0], "m", 10);
    add(cs[1], "long1", 10);
    add(cs[1], "long2", 10);
    auto tree = buildTree(rows);
    enter(tree, static_cast<NodeId>(q));
    return tree;
  }

  void testSceneLabels() {
    using enum FlameLabel;
    using enum FlameLabelStyle;
    const auto tree = labelTree();
    const auto scene = flameScene(tree, {}, 1012, 224, sevenPixelFonts());
    TEST_CHECK(scene.rows.size() == 7);
    const auto& anc3 = scene.rows[0].items;
    TEST_CHECK(anc3[0].label == None && anc3[1].label == None);
    TEST_NEAR(anc3[1].strength, 0.58, 1e-12);
    for (const std::size_t row : {1U, 2U}) {
      for (const auto& item : scene.rows[row].items) {
        TEST_CHECK(item.label == Name && item.style == Ancestor);
      }
    }
    TEST_CHECK(scene.rows[3].items[0].bar && scene.rows[3].items[0].label == None);
    const auto& first = scene.rows[4].items;
    TEST_CHECK(first.size() == 13);
    checkItems(tree, first, {{"x", 8, 261.4}, {"y", 269.4, 261.4}, {"z00", 530.8, 37}});
    TEST_CHECK(first[0].label == StackedNameValue && first[0].style == FirstAhead);
    TEST_CHECK(first[2].label == Name && first[2].style == FirstAhead);
    TEST_CHECK(first[12].more && first[12].list.size() == 50 && first[12].label == Count && first[12].style == Ahead);
    TEST_NEAR(first[12].x, 900.8, kEps);
    TEST_NEAR(first[12].w, 99.2, kEps);
    TEST_NEAR(first[12].strength, 0.9, 1e-12);
    const auto& second = scene.rows[5].items;
    TEST_CHECK(second.size() == 11);
    checkItems(tree, second, {{"x1", 8, 261.4}, {"c0", 269.4, 30}});
    TEST_CHECK(second[0].label == StackedNameValue && second[0].style == Ahead);
    TEST_CHECK(second[1].label == Name);
    TEST_CHECK(itemName(tree, second[7]) == "c6" && second[7].label == None);
    TEST_NEAR(second[7].w, 20.35, kEps);
    const auto& third = scene.rows[6].items;
    checkItems(tree, third, {{"aa", 8, 130.7}, {"bb", 138.7, 130.7}});
    TEST_CHECK(third.size() == 2 && third[0].label == NameValue && third[1].label == NameValue);
  }

  // A segment narrower than 40 px shows a name only when the name fits whole.
  void testSceneNarrowSegmentsNeedTheWholeName() {
    std::vector<Row> rows{{.parent = -1, .name = "flat"}};
    for (int i = 0; i < 30; ++i) {
      rows.push_back({.parent = 0, .name = std::format("f{:02}", i), .bytes = 10});
    }
    const auto tree = buildTree(rows);
    const auto scene = flameScene(tree, {}, 1012, 224, sevenPixelFonts());
    const auto& first = scene.rows[1];
    TEST_CHECK(first.row.h == 64 && first.items.size() == 30);
    for (const auto& [index, x, w] :
         {std::tuple{0U, 8.0, 37.0}, {15U, 563.0, 37.0}, {16U, 600.0, 28.571428571428573}}) {
      TEST_CHECK(itemName(tree, first.items[index]) == std::format("f{:02}", index));
      TEST_NEAR(first.items[index].x, x, kEps);
      TEST_NEAR(first.items[index].w, w, kEps);
    }
    for (std::size_t i = 0; i < 30; ++i) {
      TEST_CHECK(first.items[i].label == (i < 16 ? FlameLabel::Name : FlameLabel::None));
    }
  }

  // Twelve equal folders get about 82 px each: wide enough for a name, not for the value beside it.
  void testSceneValueNeedsNinetyPixels() {
    std::vector<Row> rows{{.parent = -1, .name = "flat"}};
    for (int i = 0; i < 12; ++i) {
      rows.push_back({.parent = 0, .name = std::format("f{:02}", i), .bytes = 10});
    }
    const auto tree = buildTree(rows);
    const auto scene = flameScene(tree, {}, 1012, 224, sevenPixelFonts());
    const auto& first = scene.rows[1];
    TEST_CHECK(first.items.size() == 12);
    checkItems(tree, first.items, {{"f00", 8, 82.66666666666667}, {"f01", 90.66666666666667, 82.66666666666667}});
    TEST_CHECK(std::ranges::all_of(first.items, [](const FlameItem& item) { return item.label == FlameLabel::Name; }));
  }

  void testSceneDeepAncestorsAndFarLine() {
    std::vector<Row> rows{{.parent = -1, .name = "n0"}};
    int chain = 0;
    for (int i = 1; i <= 11; ++i) {
      rows.push_back({.parent = chain, .name = std::format("n{}", i), .bytes = 1});
      rows.push_back({.parent = chain, .name = std::format("s{}", i), .bytes = 1});
      chain = static_cast<int>(rows.size()) - 2;
    }
    auto tree = buildTree(rows);
    // n11 is the last chain node: row 21.
    enter(tree, 21);
    TEST_CHECK(tree.ancestors.size() == 11);
    const auto scene = flameScene(tree, {}, 1000, 281, sevenPixelFonts());
    TEST_CHECK(scene.bands.far && scene.rows[0].row.kind == FlameRowKind::Far);
    const auto& far = scene.rows[0].items;
    TEST_CHECK(far.size() == 6 && far[0].node == 0 && far[0].far && far[0].label == FlameLabel::None);
    TEST_NEAR(far[0].strength, 0.6, 1e-12);
    const auto& sixth = scene.rows[1];
    TEST_CHECK(sixth.row.level == 6);
    const auto pinned = std::ranges::find_if(sixth.items, &FlameItem::pinned);
    TEST_CHECK(pinned != sixth.items.end() && pinned->accentFill);
    TEST_NEAR(pinned->strength, 0.85, 1e-12);
    const auto other = std::ranges::find_if(sixth.items, [](const FlameItem& item) { return !item.pinned; });
    TEST_CHECK(other != sixth.items.end() && !other->accentFill);
    TEST_NEAR(other->strength, 0.44, 1e-12);
    TEST_CHECK(scene.empty);
  }

  void testSceneHitAndDegenerateHosts() {
    auto tree = chainTree();
    const auto scene = flameScene(tree, {}, 1000, 266, sevenPixelFonts());
    const auto hit = flameHitAt(scene, 700, 40);
    TEST_CHECK(hit.has_value() && hit->row == 1 && itemName(tree, scene.rows[1].items[hit->item]) == "a2");
    TEST_CHECK(!flameHitAt(scene, 700, 2).has_value());
    TEST_CHECK(!flameHitAt(scene, 995, 40).has_value());
    for (const double width : {0.0, -5.0, kNan}) {
      const auto narrow = flameScene(tree, {}, width, 266, {});
      TEST_CHECK(narrow.x0 == 8 && narrow.x1 == 48);
    }
    // A band height that is not a number lays out as a 0 px band.
    const auto flat = flameScene(tree, {}, 1000, kNan, {});
    const auto zero = flameBands(0, tree.ancestors.size());
    TEST_CHECK(flat.rows.size() == zero.rows.size() && flat.bands.bottom == zero.bottom);
    for (std::size_t i = 0; i < zero.rows.size(); ++i) {
      const auto& row = flat.rows[i].row;
      TEST_CHECK(row.kind == zero.rows[i].kind && row.level == zero.rows[i].level);
      TEST_CHECK(row.y == zero.rows[i].y && row.h == zero.rows[i].h);
    }
    Tree empty;
    const auto nothing = flameScene(empty, {}, 1000, 266, {});
    TEST_CHECK(nothing.empty && nothing.rows.size() == 4 && nothing.rows[0].items[0].bar);
    const auto ahead = flameAhead(empty, {}, kNoNode, 0, 100, {});
    TEST_CHECK(ahead.rows[0].empty() && ahead.spent == 0);
    const auto levels = flameAhead(tree, {}, 0, 0, 1000, {.levels = 9});
    TEST_CHECK(!levels.rows[2].empty());
    const auto one = flameAhead(tree, {}, 0, 0, 1000, {.levels = 1});
    TEST_CHECK(!one.rows[0].empty() && one.rows[1].empty());
    const auto sized = flameAhead(tree, {.sizeBy = WorkbenchMeasure::Equal}, 0, 0, 1000, {});
    TEST_NEAR(sized.rows[0][0].w, 500.0, kEps);
  }
} // namespace

int main() {
  testHitLookup();
  testFirstLevelAncestor();
  testBoxPixelAligned();
  testChainScenesCarryTheClickedSegment();
  testWideScenes();
  testSceneLabels();
  testSceneNarrowSegmentsNeedTheWholeName();
  testSceneValueNeedsNinetyPixels();
  testSceneDeepAncestorsAndFarLine();
  testSceneHitAndDegenerateHosts();
  std::println("workbench flame scene checks passed");
  return 0;
}
