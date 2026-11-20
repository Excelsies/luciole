#include "test_check.h"
#include "workbench/flame_scene.h"
#include "workbench/flame_strata.h"
#include "workbench_flame_fixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <print>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::flame;

  const FlameItem& parentOf(const FlameAhead& ahead, std::size_t level, const FlameItem& item) {
    return ahead.rows[level - 1][item.parent];
  }

  void testAheadFoldersOnlyInsideTheirParents() {
    auto tree = wideTree();
    const auto ahead = flameAhead(tree, {}, 0, 8, 1108, {.min = 3});
    for (const auto& row : ahead.rows) {
      for (const auto& item : row) {
        TEST_CHECK(item.more || tree.nodes[item.node].folder);
        TEST_CHECK(item.w >= 3 - kEps);
      }
    }
    TEST_CHECK(std::ranges::any_of(ahead.rows[0], [&](const FlameItem& item) { return itemName(tree, item) == "d0"; }));
    TEST_CHECK(std::ranges::none_of(ahead.rows[0], [&](const FlameItem& item) {
      return !item.more && tree.nodes[item.node].name.starts_with("file");
    }));
    for (const std::size_t level : {1U, 2U}) {
      for (const auto& item : ahead.rows[level]) {
        TEST_CHECK(item.parent != kNoFlameItem);
        const auto& parent = parentOf(ahead, level, item);
        TEST_CHECK(item.x >= parent.x - kEps && item.x + item.w <= parent.x + parent.w + kEps);
        TEST_CHECK(item.top < ahead.rows[0].size() && ahead.rows[0][item.top].x <= item.x + kEps);
      }
    }
    const auto d0 = static_cast<std::size_t>(
        std::ranges::find_if(ahead.rows[0], [&](const FlameItem& item) { return itemName(tree, item) == "d0"; })
        - ahead.rows[0].begin()
    );
    TEST_CHECK(ahead.rows[0][d0].top == d0);
    std::vector<std::size_t> kids;
    double kidsWidth = 0;
    for (std::size_t i = 0; i < ahead.rows[1].size(); ++i) {
      if (ahead.rows[1][i].parent == d0) {
        kids.push_back(i);
        kidsWidth += ahead.rows[1][i].w;
      }
    }
    TEST_CHECK(kids.size() == 10);
    TEST_NEAR(kidsWidth, ahead.rows[0][d0].w, 1e-6);
    // Rows run left to right without overlap, so the binary search finds every item at its center.
    for (const auto& row : ahead.rows) {
      for (std::size_t i = 0; i < row.size(); ++i) {
        TEST_CHECK(i == 0 || row[i].x >= row[i - 1].x + row[i - 1].w - kEps);
        TEST_CHECK(flameItemAt(row, row[i].x + row[i].w / 2) == i);
      }
    }
    TEST_CHECK(std::ranges::none_of(ahead.rows[2], [&](const FlameItem& item) {
      return std::ranges::contains(kids, item.parent);
    }));
  }

  // X (600 px, left) holds a and b at 300 px each; Y (400 px, right) holds c. The wider c is laid out first in the
  // third stratum, yet rows still run left to right and keep their parents.
  void testAheadRowsRunLeftToRight() {
    const auto tree = buildTree({
        {.parent = -1, .name = "r"},
        {.parent = 0, .name = "X", .bytes = 599},
        {.parent = 0, .name = "Y", .bytes = 399},
        {.parent = 1, .name = "a", .bytes = 9},
        {.parent = 1, .name = "b", .bytes = 9},
        {.parent = 2, .name = "c", .bytes = 9},
        {.parent = 3, .name = "a1", .bytes = 1},
        {.parent = 4, .name = "b1", .bytes = 1},
        {.parent = 5, .name = "c1", .bytes = 1},
    });
    LayoutSettings settings;
    settings.emphasis = 1;
    const auto ahead = flameAhead(tree, settings, 0, 0, 1000, {});
    checkItems(tree, ahead.rows[0], {{"X", 0, 600}, {"Y", 600, 400}});
    checkItems(tree, ahead.rows[1], {{"a", 0, 300}, {"b", 300, 300}, {"c", 600, 400}});
    checkItems(tree, ahead.rows[2], {{"a1", 0, 300}, {"b1", 300, 300}, {"c1", 600, 400}});
    TEST_CHECK(ahead.rows[1][0].parent == 0 && ahead.rows[1][2].parent == 1);
    TEST_CHECK(ahead.rows[2][0].parent == 0 && ahead.rows[2][1].parent == 1 && ahead.rows[2][2].parent == 2);
    TEST_CHECK(ahead.rows[2][1].top == 0 && ahead.rows[2][2].top == 1);
    TEST_CHECK(ahead.rows[0][1].top == 1);
  }

  void testAheadMergedPieceCappedAndListed() {
    const auto tree = wideTree();
    const auto ahead = flameAhead(tree, {}, 0, 0, 1000, {.min = 3});
    std::vector<const FlameItem*> more;
    for (const auto& item : ahead.rows[0]) {
      if (item.more) {
        more.push_back(&item);
      }
    }
    TEST_CHECK(more.size() == 1);
    TEST_CHECK(more[0]->w <= 150 + kEps);
    TEST_CHECK(more[0]->list.size() > 1000 && more[0]->owner == 0);
    TEST_CHECK(std::ranges::all_of(more[0]->list, [&](NodeId id) {
      return tree.nodes[id].folder && tree.nodes[id].parent == 0;
    }));
  }

  void testAheadWorkBoundedAndFlagged() {
    const auto tree = wideTree();
    const auto full = flameAhead(tree, {}, 0, 0, 1100, {.min = 3});
    const auto tight = flameAhead(tree, {}, 0, 0, 1100, {.min = 3, .budget = 100});
    TEST_CHECK(!full.truncated);
    TEST_CHECK(tight.truncated);
    TEST_CHECK(tight.rows[1].size() < full.rows[1].size());
    TEST_CHECK(full.spent == 6570 && tight.spent == 5040);
    TEST_CHECK(full.rows[0].size() == 41 && full.rows[1].size() == 138 && full.rows[2].empty());
    TEST_CHECK(tight.rows[1].empty());
    // The budget is the most child entries read: exactly what the full layout reads is enough.
    TEST_CHECK(!flameAhead(tree, {}, 0, 0, 1100, {.min = 3, .budget = 6570}).truncated);
    TEST_CHECK(flameAhead(tree, {}, 0, 0, 1100, {.min = 3, .budget = 6569}).truncated);
    // Child lists are read only for the current folder and drawn folders.
    std::size_t readable = children(tree, 0).size();
    for (const std::size_t level : {0U, 1U}) {
      for (const auto& item : full.rows[level]) {
        if (!item.more) {
          readable += children(tree, item.node).size();
        }
      }
    }
    TEST_CHECK(full.spent <= readable);
    checkItems(tree, full.rows[0], {{"d0", 0, 87.8615415244054}, {"d1", 87.8615415244054, 62.12752288111756}});
    checkItems(tree, full.rows[1], {{"e0", 0, 17.49851659306699}, {"e1", 17.49851659306699, 12.373381609488389}});
    const auto& more = full.rows[0].back();
    TEST_CHECK(more.more && more.list.size() == 2000);
    TEST_NEAR(more.x, 990.0000000000002, kEps);
    TEST_NEAR(more.w, 109.99999999999977, kEps);
  }

  void testAheadNarrowParentGetsNoChildren() {
    const auto tree = wideTree();
    const auto ahead = flameAhead(tree, {}, 0, 0, 1100, {});
    for (const auto& item : ahead.rows[1]) {
      TEST_CHECK(parentOf(ahead, 1, item).w >= 36 - kEps);
    }
  }

  void testNoUnreadableSlivers() {
    const auto tree = wideTree();
    const auto ahead = flameAhead(tree, {}, 0, 8, 1108, {});
    const std::array<double, 3> floors{28, 18, 18};
    for (std::size_t level = 0; level < 3; ++level) {
      for (const auto& item : ahead.rows[level]) {
        TEST_CHECK(item.w >= floors[level] - kEps);
      }
    }
    const auto more = std::ranges::count_if(ahead.rows[0], &FlameItem::more);
    TEST_CHECK(more == 1);
    const auto& piece = ahead.rows[0].back();
    TEST_CHECK(piece.more && piece.w <= 165 + kEps && piece.list.size() > 1000);
    TEST_CHECK(ahead.rows[0].size() - 1 < 40);
    for (const std::size_t level : {1U, 2U}) {
      for (const auto& item : ahead.rows[level]) {
        const auto& parent = parentOf(ahead, level, item);
        TEST_CHECK(item.x >= parent.x - kEps && item.x + item.w <= parent.x + parent.w + kEps);
        TEST_CHECK(item.w <= parent.w + kEps);
      }
    }
    TEST_CHECK(ahead.spent == 5210 && ahead.rows[0].size() == 5 && ahead.rows[1].size() == 28);
    TEST_NEAR(piece.x, 998.0, kEps);
    TEST_NEAR(piece.w, 110.0, kEps);
    TEST_CHECK(piece.list.size() == 2036);
    checkItems(tree, ahead.rows[0], {{"d0", 8, 355.5448636675946}, {"d1", 363.5448636675946, 251.4083098193376}});
    checkItems(tree, ahead.rows[1], {{"e0", 8, 70.81036353930793}, {"e1", 78.81036353930793, 50.07073858624112}});
  }

  void testStratumTinySiblingsMerge() {
    auto tree = wideTree();
    enter(tree, childAt(tree, 0, 3));
    const auto items = flameStratum(tree, {}, tree.root, tree.ancestors, 1, 8, 1108, {});
    TEST_CHECK(std::ranges::all_of(items, [](const FlameItem& item) { return item.w >= 28 - kEps; }));
    const auto more = std::ranges::find_if(items, &FlameItem::more);
    TEST_CHECK(more != items.end() && more->list.size() > 1000 && more->owner == 0);
    TEST_CHECK(items.size() == 5 && more->list.size() == 2036);
    TEST_NEAR(more->x, 943.0, kEps);
    TEST_NEAR(more->w, 165.0, kEps);
    TEST_CHECK(items[3].pinned && items[3].node == tree.root);
    TEST_NEAR(items[3].x, 823.0, kEps);
    TEST_NEAR(items[3].w, 120.0, kEps);
    checkItems(tree, items, {{"d0", 8, 356.75858636011117}, {"d1", 364.75858636011117, 252.26654179481133}});
  }

  void testStratumOnPathAtLeast120Wide() {
    auto tree = wideTree();
    const auto d = childAt(tree, 0, 30);
    const auto e = childAt(tree, d, 9);
    enter(tree, e);
    for (const std::size_t k : {1U, 2U}) {
      const auto items = flameStratum(tree, {}, tree.root, tree.ancestors, k, 8, 1108, {});
      const auto pinned = std::ranges::count_if(items, &FlameItem::pinned);
      TEST_CHECK(pinned == 1);
      const auto found = std::ranges::find_if(items, &FlameItem::pinned);
      TEST_CHECK(found->w >= 120 - kEps);
      TEST_CHECK(found->node == (k == 1 ? tree.root : tree.ancestors[0]));
      double sum = 0;
      for (const auto& item : items) {
        sum += item.w;
        TEST_CHECK(!item.more || item.w <= 165 + kEps);
      }
      TEST_NEAR(sum, 1100.0, kEps);
    }
    const auto first = flameStratum(tree, {}, tree.root, tree.ancestors, 1, 8, 1108, {});
    TEST_CHECK(first.size() == 10);
    checkItems(tree, first, {{"e0", 8, 208.2959986785237}, {"e1", 216.2959986785237, 147.2882495879687}});
    TEST_NEAR(first[9].x, 988.0, kEps);
    const auto second = flameStratum(tree, {}, tree.root, tree.ancestors, 2, 8, 1108, {});
    TEST_CHECK(second.size() == 5 && second[3].pinned && itemName(tree, second[3]) == "d30");
    TEST_CHECK(itemName(tree, second[4]) == "+2036");
  }

  void testStratumHiddenOnPathStillShown() {
    auto tree = buildTree({
        {.parent = -1, .name = "r"},
        {.parent = 0, .name = ".h", .bytes = 5},
        {.parent = 0, .name = "v", .bytes = 50},
        {.parent = 1, .name = "in", .bytes = 5},
    });
    enter(tree, 1);
    const auto items = flameStratum(tree, {}, tree.root, tree.ancestors, 1, 0, 600, {});
    TEST_CHECK(std::ranges::any_of(items, [&](const FlameItem& item) {
      return item.pinned && item.node == tree.root && item.w >= 120;
    }));
    // With two visible siblings the hidden one takes an equal third before the others share the rest.
    auto three = buildTree({
        {.parent = -1, .name = "r"},
        {.parent = 0, .name = ".h", .bytes = 5},
        {.parent = 0, .name = "v", .bytes = 50},
        {.parent = 0, .name = "u", .bytes = 30},
        {.parent = 1, .name = "in", .bytes = 5},
    });
    enter(three, 1);
    const auto thirds = flameStratum(three, {}, three.root, three.ancestors, 1, 0, 600, {});
    TEST_CHECK(thirds.size() == 3 && thirds[0].pinned && thirds[0].node == 1);
    TEST_NEAR(thirds[0].w, 200.0, kEps);
    checkItems(
        three, thirds, {{".h", 0, 200}, {"v", 200, 224.76418591715824}, {"u", 424.7641859171582, 175.23581408284178}}
    );
  }

  void testStratumDegenerateInputs() {
    auto tree = chainTree();
    enter(tree, 2);
    TEST_CHECK(flameStratum(tree, {}, tree.root, tree.ancestors, 0, 0, 600, {}).empty());
    TEST_CHECK(flameStratum(tree, {}, tree.root, tree.ancestors, 3, 0, 600, {}).empty());
    const std::vector<NodeId> unknown{77};
    TEST_CHECK(flameStratum(tree, {}, tree.root, unknown, 1, 0, 600, {}).empty());
    TEST_CHECK(flameStratum(tree, {}, kNoNode, tree.ancestors, 1, 0, 600, {}).empty());
    TEST_CHECK(flameStratum(tree, {}, tree.root, tree.ancestors, 1, 0, kNan, {}).empty());
  }

  void testFarLineOneCellPerLevel() {
    const std::vector<NodeId> ancestors{0, 1, 2, 3};
    const auto cells = flameFarLine(ancestors, 2, 0, 600);
    TEST_CHECK(cells.size() == 3);
    TEST_CHECK(cells[0].node == 3 && cells[1].node == 2 && cells[2].node == 1);
    TEST_CHECK(std::ranges::all_of(cells, [](const FlameItem& cell) { return std::abs(cell.w - 200) < kEps; }));
    TEST_NEAR(cells[2].x + cells[2].w, 600.0, kEps);
    TEST_CHECK(std::ranges::all_of(cells, [](const FlameItem& cell) { return cell.far && cell.level == 3; }));
    TEST_CHECK(flameFarLine(ancestors, 0, 0, 600).empty());
    TEST_CHECK(flameFarLine(ancestors, 5, 0, 600).empty());
  }

} // namespace

int main() {
  testAheadFoldersOnlyInsideTheirParents();
  testAheadRowsRunLeftToRight();
  testAheadMergedPieceCappedAndListed();
  testAheadWorkBoundedAndFlagged();
  testAheadNarrowParentGetsNoChildren();
  testNoUnreadableSlivers();
  testStratumTinySiblingsMerge();
  testStratumOnPathAtLeast120Wide();
  testStratumHiddenOnPathStillShown();
  testStratumDegenerateInputs();
  testFarLineOneCellPerLevel();
  std::println("workbench flame strata checks passed");
  return 0;
}
