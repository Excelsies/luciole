#include "test_check.h"
#include "workbench/column_plan.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <optional>
#include <print>
#include <tuple>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using luciole::test::workbenchTree;

  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

  const ColumnWidths kDefaults{.columns = {260, 190, 140}, .list = std::nullopt};

  ColumnWidths widthsOf(double a, double b, double c, std::optional<double> list = std::nullopt) {
    return {.columns = {a, b, c}, .list = list};
  }

  PlanOptions optionsOf(const ColumnWidths& widths, double listMin) {
    return {
        .strips = true, .widths = widths, .listMin = listMin, .maxColumns = std::nullopt, .stripsWidth = std::nullopt
    };
  }

  double clampWidth(double value, double lo, double hi) { return std::max(lo, std::min(hi, value)); }

  // A strip pane as wide as the bands of `ancestors` need, at least the narrowest pane; none without ancestors.
  ColumnPlan planOf(double viewWidth, std::size_t ancestors, PlanOptions options) {
    options.strips = options.strips && ancestors > 0;
    options.stripsWidth = std::max(kStripsMinWidth, stripBandsWidth(ancestors));
    return planColumns(viewWidth, ancestors, options);
  }

  void testColumnCount() {
    const std::array<double, 3> widths{260, 190, 140};
    TEST_CHECK(columnCount(920, widths, 320, 470) == 1);  // A leaves 659, A + B leave 468 < 470
    TEST_CHECK(columnCount(1000, widths, 320, 470) == 2); // A + B leave 548, with C 407 < 470
    TEST_CHECK(columnCount(1100, widths, 320, 470) == 3); // with C 507 >= 470
    TEST_CHECK(columnCount(1000, widths, 320, 320) == 3); // a user-chosen narrower list floor
    TEST_CHECK(columnCount(570, widths, 320, 470) == 0);  // not even A leaves the list its minimum
    TEST_CHECK(columnCount(100, widths, 320, 470) == 0);
    TEST_CHECK(columnCount(kNan, widths, 320, 470) == 0);
  }

  void testPlanTiles() {
    for (const auto& [width, ancestors, listMin] : std::array<std::tuple<double, std::size_t, double>, 7>{{
             {1100, 0, 420},
             {1100, 11, 420},
             {1400, 5, 420},
             {2560, 11, 320},
             {1000, 3, 320},
             {600, 3, 320},
             {360, 4, 320},
         }}) {
      const auto plan = planOf(width, ancestors, optionsOf(kDefaults, listMin));
      double x = plan.margin;
      for (const auto& strip : plan.strips) {
        TEST_CHECK(strip.x == x);
        x += strip.width + 1;
      }
      TEST_CHECK(plan.list.x == x);
      TEST_CHECK(plan.list.width >= std::min(listMin, width - plan.stripsWidth)); // the pane stops at its minimum
      x += plan.list.width;
      for (const auto& column : plan.columns) {
        TEST_CHECK(column.x == x);
        x += column.width + 1;
      }
      TEST_CHECK(x - 1 + plan.margin <= width + 1);
    }
    TEST_CHECK(planOf(1100, 0, optionsOf(kDefaults, 420)).columns.size() == 3); // the list keeps 470 first
    // one rule: one pixel changes nothing
    TEST_CHECK(planOf(1100, 0, optionsOf(widthsOf(261, 190, 140), 420)).columns.size() == 3);
    TEST_CHECK(planOf(1100, 4, optionsOf(kDefaults, 420)).columns.size() == 2);
  }

  void testListKeepsPreferredWidth() {
    const auto columnsAt = [](double width, std::size_t ancestors, const ColumnWidths& widths) {
      return planOf(width, ancestors, optionsOf(widths, 320)).columns.size();
    };
    TEST_CHECK(columnsAt(1000, 2, kDefaults) == 1); // strips 80: A leaves 659, A + B leave 468 < 470
    TEST_CHECK(columnsAt(1050, 2, kDefaults) == 2);
    TEST_CHECK(columnsAt(1150, 2, kDefaults) == 3);
    TEST_CHECK(columnsAt(1400, 2, kDefaults) == 3);
    for (const double width : {1000.0, 1050.0, 1150.0, 1400.0}) {
      for (const std::size_t ancestors : {0U, 2U}) {
        const auto plan = planOf(width, ancestors, optionsOf(kDefaults, 320));
        TEST_CHECK(plan.columns.size() <= 1 || plan.list.width >= kListPreferred);
      }
    }
    TEST_CHECK(columnsAt(1000, 2, widthsOf(261, 190, 140)) == 1);      // one pixel does not change the rule
    TEST_CHECK(columnsAt(1000, 2, widthsOf(261, 190, 140, 320)) == 3); // a list dragged narrower is remembered
    TEST_CHECK(columnsAt(600, 0, kDefaults) == 1);                     // the first column needs only the list's minimum
    TEST_CHECK(listFloor(kDefaults, 320) == 470);
    TEST_CHECK(listFloor(widthsOf(260, 190, 140, 400), 320) == 400);
    TEST_CHECK(listFloor(widthsOf(260, 190, 140, 400), 420) == 420);
  }

  void testNoStrips() {
    auto options = optionsOf(kDefaults, 320);
    options.strips = false;
    const auto plan = planOf(1400, 6, options);
    TEST_CHECK(plan.strips.empty());
    TEST_CHECK(plan.list.x == 0);
  }

  void testWideViewCentered() {
    const auto plan = planOf(2560, 3, optionsOf(kDefaults, 320));
    TEST_CHECK(plan.list.width == kListMax);
    TEST_CHECK(plan.margin > 100);
    const auto odd = planOf(2561, 3, optionsOf(kDefaults, 320)); // 589 px to share: the margin is whole
    TEST_CHECK(odd.margin == 294);
    TEST_CHECK(odd.strips.front().x == 294);
  }

  void testCustomWidths() {
    TEST_CHECK(normalizeWidths(widthsOf(300, 100, kNan)) == widthsOf(300, 190, 140));
    TEST_CHECK(normalizeWidths(ColumnWidths{}) == kDefaults);
    TEST_CHECK(normalizeWidths(widthsOf(641, 140, 640)) == widthsOf(260, 140, 640));
    TEST_CHECK(normalizeWidths(widthsOf(300.5, 200.4, kInfinity)) == widthsOf(301, 200, 140));
    const auto plan = planOf(1700, 0, optionsOf(widthsOf(400, 300, 200), 320));
    TEST_CHECK(plan.columns.size() == 3);
    TEST_CHECK(plan.columns[0].width == 400 && plan.columns[1].width == 300 && plan.columns[2].width == 200);
    TEST_CHECK(plan.columns[2].index == 2);
  }

  void testMaxColumnWidth() {
    const auto widths = widthsOf(261, 190, 140, 320);
    const auto plan = planOf(1000, 0, optionsOf(widths, 320));
    TEST_CHECK(plan.columns.size() == 3);
    TEST_CHECK(plan.spare == 1000 - 594 - 320);
    TEST_NEAR(maxColumnWidth(plan, 0), 261 + plan.spare, 0);
    TEST_NEAR(maxColumnWidth(planOf(2560, 0, optionsOf(widths, 320)), 0), 640, 0);
    TEST_CHECK(!maxColumnWidth(plan, 3).has_value());
  }

  void testResizeSweepHasNoCliff() {
    std::size_t steps = 0;
    double total = 0;
    for (const double viewWidth : {900.0, 1000.0, 1100.0, 1150.0, 1300.0, 1600.0}) {
      for (const std::size_t ancestors : {0U, 2U}) {
        for (const double listMin : {320.0, 420.0}) {
          for (std::size_t i = 0; i < 3; ++i) {
            for (const double dir : {1.0, -1.0}) {
              auto widths = kDefaults;
              for (std::size_t k = 0; k < 700; ++k) { // a width moves 1 px per pass and spans at most 500 px
                const auto p = planOf(viewWidth, ancestors, optionsOf(widths, listMin));
                if (i >= p.columns.size()) {
                  break;
                }
                const auto next = resizeColumn(widths, p, i, p.columns[i].width + dir, listMin);
                if (next == widths) {
                  break;
                }
                const auto q = planOf(viewWidth, ancestors, optionsOf(next, listMin));
                const auto countStep =
                    std::abs(static_cast<long>(q.columns.size()) - static_cast<long>(p.columns.size()));
                TEST_CHECK(countStep <= 1);
                TEST_CHECK(std::abs(q.list.width - p.list.width) <= 2 + (countStep == 0 ? 0 : 641));
                TEST_CHECK(q.list.width >= std::min(listMin, viewWidth)); // the list keeps its minimum
                total += static_cast<double>(k % 13 + 1)
                    * (static_cast<double>(q.columns.size())
                       + 3 * q.list.width
                       + 5 * q.spare
                       + 7 * next.list.value_or(0));
                widths = next;
                ++steps;
              }
            }
          }
        }
      }
    }
    TEST_CHECK(steps == 18241);
    TEST_NEAR(total, 460892529.0, 1e-6);
  }

  void testResizeRemembersNarrowList() {
    const auto plan = planOf(1050, 2, optionsOf(kDefaults, 320)); // 2 columns, list 518
    TEST_CHECK(plan.columns.size() == 2);
    const auto narrower = resizeColumn(kDefaults, plan, 1, 250, 320); // B +60: the list goes from 518 to 458
    TEST_CHECK(narrower.list == 458.0);
    const auto again = planOf(1050, 2, optionsOf(narrower, 320));
    TEST_CHECK(again.columns.size() == 2);
    const auto back = resizeColumn(narrower, again, 1, 190, 320);
    TEST_CHECK(!back.list.has_value()); // the list is 518 again, at least 470: the remembered floor goes
    TEST_CHECK(!normalizeWidths(widthsOf(300, 200, 150, 100)).list.has_value());
    TEST_CHECK(normalizeWidths(widthsOf(300, 200, 150, 400)).list == 400.0);
    TEST_CHECK(!normalizeWidths(widthsOf(300, 200, 150, 470)).list.has_value());
    TEST_CHECK(!normalizeWidths(widthsOf(300, 200, 150, kNan)).list.has_value());
    // kept when at least 320 and under 470, then rounded
    TEST_CHECK(!normalizeWidths(widthsOf(300, 200, 150, 319.9)).list.has_value());
    TEST_CHECK(normalizeWidths(widthsOf(300, 200, 150, 320)).list == 320.0);
    TEST_CHECK(normalizeWidths(widthsOf(300, 200, 150, 469.4)).list == 469.0);
    TEST_CHECK(normalizeWidths(widthsOf(300, 200, 150, 469.5)).list == 470.0);
    // with a list minimum under 320, the remembered list width still stays at least 320
    const auto low = planOf(1000, 0, optionsOf(kDefaults, 200));
    TEST_CHECK(low.columns.size() == 2 && low.spare == 348);
    TEST_CHECK(resizeColumn(kDefaults, low, 0, 640, 200) == widthsOf(608, 190, 140, 320));
    TEST_CHECK(resizeColumn(kDefaults, low, 0, 400, 200) == widthsOf(400, 190, 140, 408));
  }

  void testColumnFont() {
    for (const double list : {12.0, 13.0, 14.0, 16.0}) {
      for (std::size_t i = 0; i < 3; ++i) {
        const double font = columnFont(list, i);
        TEST_CHECK(font >= 11 && font < list);
        TEST_CHECK(i == 0 || font <= columnFont(list, i - 1));
      }
    }
    TEST_CHECK(columnFont(13, 0) == 12);
    TEST_CHECK(columnFont(13, 1) == 11.5);
    TEST_CHECK(columnFont(13, 2) == 11);
    TEST_CHECK(columnFont(kNan, 0) == 11);
  }

  // Ids: 0 root, 1 a, 2 f.txt, 3 b, 4 a1, 5 a2, 6 x, 7 f, 8 g, 9 .h (hidden).
  std::vector<luciole::test::WorkbenchRow> chainRows() {
    return {
        {.parent = -1, .name = "root", .folder = true, .bytes = 1000},
        {.parent = 0, .name = "a", .folder = true, .bytes = 300},
        {.parent = 0, .name = "f.txt", .bytes = 200},
        {.parent = 0, .name = "b", .folder = true, .bytes = 100},
        {.parent = 1, .name = "a1", .folder = true, .bytes = 200},
        {.parent = 1, .name = "a2", .folder = true, .bytes = 60},
        {.parent = 4, .name = "x", .folder = true, .bytes = 100},
        {.parent = 1, .name = "f", .bytes = 40},
        {.parent = 4, .name = "g", .bytes = 50},
        {.parent = 0, .name = ".h", .folder = true, .bytes = 500},
    };
  }

  Tree chainTree() { return workbenchTree(chainRows(), 0); }

  ColumnChain chainOf(const Tree& tree, const ChainFocus& focus, const std::vector<NodeId>& remembered = {}) {
    return columnChain(tree, LayoutSettings{}, focus, remembered);
  }

  void testChainFollowsTrail() {
    const auto tree = chainTree();
    TEST_CHECK((chainOf(tree, {}) == ColumnChain{.a = 1, .file = kNoNode, .b = 4, .c = 6}));
    TEST_CHECK((chainOf(tree, {}, {3}) == ColumnChain{.a = 3, .file = kNoNode, .b = kNoNode, .c = kNoNode}));
    const auto trail = chainOf(tree, {}, {1, 5});
    TEST_CHECK(trail.a == 1);
    TEST_CHECK(trail.b == 5);
  }

  void testChainCursorAndPointer() {
    const auto tree = chainTree();
    TEST_CHECK(chainOf(tree, {.cursor = 1}, {3}).a == 1);
    TEST_CHECK(chainOf(tree, {.cursor = 3, .pointer = 1}, {3}).a == 1);
    // with a cursor the later columns take the heaviest folder, not the remembered one
    TEST_CHECK(chainOf(tree, {.cursor = 1}, {5}).b == 4);
    TEST_CHECK(chainOf(tree, {}, {5}).b == 5);
  }

  void testChainFileUnderCursor() {
    const auto tree = chainTree();
    TEST_CHECK((chainOf(tree, {.cursor = 2}) == ColumnChain{.a = kNoNode, .file = 2, .b = kNoNode, .c = kNoNode}));
  }

  void testChainSteering() {
    const auto tree = chainTree();
    TEST_CHECK(chainOf(tree, {.cursor = 1, .steerB = 5}).b == 5);
    TEST_CHECK(chainOf(tree, {.cursor = 1, .steerB = 3}).b == 4); // not a child of A: ignored
    TEST_CHECK(chainOf(tree, {.cursor = 1, .steerB = 4, .steerC = 3}).c == 6);
  }

  void testChainFileRowFillsNextSlot() {
    const auto tree = chainTree();
    TEST_CHECK(
        (chainOf(tree, {.cursor = 1, .steerB = 7}) == ColumnChain{.a = 1, .file = kNoNode, .b = 7, .c = kNoNode})
    );
    const auto chain = chainOf(tree, {.cursor = 1, .steerB = 4, .steerC = 8});
    TEST_CHECK(chain.c == 8);
    TEST_CHECK(!tree.nodes[chain.c].folder);
    // a file in b ends the chain, even in a tree that lists a node below that file
    auto rows = chainRows();
    rows.push_back({.parent = 7, .name = "inside", .folder = true, .bytes = 10});
    const auto odd = workbenchTree(rows, 0);
    TEST_CHECK(
        (chainOf(odd, {.cursor = 1, .steerB = 7, .steerC = 10})
         == ColumnChain{.a = 1, .file = kNoNode, .b = 7, .c = kNoNode})
    );
  }

  void testChainEndsWithoutSubfolders() {
    auto tree = chainTree();
    tree.root = 3;
    TEST_CHECK((chainOf(tree, {}) == ColumnChain{}));
  }

  void testChainDegenerate() {
    const auto tree = chainTree();
    TEST_CHECK(chainOf(tree, {}, {9}).a == 1);  // a hidden remembered folder is not followed
    TEST_CHECK(chainOf(tree, {}, {2}).a == 1);  // nor a remembered file
    TEST_CHECK(chainOf(tree, {}, {99}).a == 1); // nor an unknown id
    TEST_CHECK(chainOf(tree, {}, {6, 3}).a == 3);
    TEST_CHECK((chainOf(tree, {.cursor = 99, .pointer = kNoNode}) == chainOf(tree, {})));
    LayoutSettings hidden;
    hidden.showHidden = true;
    TEST_CHECK(columnChain(tree, hidden, {}, {}).a == 9);
    const std::vector<NodeId> remembered{9};
    TEST_CHECK(columnChain(tree, hidden, {}, remembered).a == 9);
    TEST_CHECK((columnChain(Tree{}, {}, {}, {}) == ColumnChain{}));
  }

  struct DragRun {
    ColumnWidths saved;
    std::vector<double> trace;
  };

  // One whole drag, 1 px per frame, the way the dividers run it: the column count is held while the pointer is down
  // and the layout is planned again on release.
  std::optional<DragRun> runDrag(
      double viewWidth, std::size_t ancestors, double listMin, std::size_t i, double dir, double* checksum = nullptr
  ) {
    constexpr std::size_t kFrames = 700; // one pixel per frame covers the widest possible range
    const auto plan0 = planOf(viewWidth, ancestors, optionsOf(kDefaults, listMin));
    const auto drag = startColumnDrag(plan0, i, 1000, kDefaults);
    if (!drag) {
      TEST_CHECK(i >= plan0.columns.size());
      return std::nullopt;
    }
    TEST_CHECK(drag->column == i && drag->startWidth == plan0.columns[i].width);
    TEST_CHECK(drag->startX == 1000 && drag->spare == plan0.spare && drag->widths == kDefaults);
    const double max = clampWidth(drag->startWidth + drag->spare, 140, 640);
    DragRun run{.saved = kDefaults, .trace = {}};
    double previous = drag->startWidth;
    for (std::size_t k = 1; k <= kFrames; ++k) {
      const double travel = dir * static_cast<double>(k);
      run.saved = dragColumn(*drag, drag->startX + travel, listMin);
      auto heldOptions = optionsOf(run.saved, listMin);
      heldOptions.maxColumns = plan0.columns.size();
      const auto held = planOf(viewWidth, ancestors, heldOptions);
      const double width = run.saved.columns[i];
      const double want = clampWidth(drag->startWidth - travel, 140, max);
      if (width != want) {
        std::println(
            stderr, "{}/{}/{} column {} dir {} frame {}: {} not {}", viewWidth, ancestors, listMin, i, dir, k, width,
            want
        );
      }
      TEST_CHECK(width == want);
      TEST_CHECK(std::abs(width - previous) <= 1);
      TEST_CHECK(held.columns.size() == plan0.columns.size()); // the count is held during a drag
      TEST_CHECK(held.columns[i].width == width);
      TEST_CHECK(held.list.width >= std::min(listMin, viewWidth));
      if (checksum != nullptr) {
        const auto& c = run.saved.columns;
        *checksum += static_cast<double>(k % 13 + 1)
            * (c[0] + 2 * c[1] + 3 * c[2] + 5 * run.saved.list.value_or(0) + 7 * held.list.width + 11 * held.margin);
      }
      previous = width;
      run.trace.push_back(width);
    }
    // what is saved is the start width plus the pointer's travel, clamped
    TEST_CHECK(run.saved.columns[i] == clampWidth(drag->startWidth - dir * static_cast<double>(kFrames), 140, max));
    const auto after = planOf(viewWidth, ancestors, optionsOf(run.saved, listMin));
    TEST_CHECK(after.list.width >= std::min(listMin, viewWidth));
    TEST_CHECK(after.columns.size() <= plan0.columns.size() + 1); // the release adds at most one column
    return run;
  }

  void testWholeDragsFollowPointer() {
    std::size_t drags = 0;
    double checksum = 0;
    for (const double viewWidth : {900.0, 1000.0, 1100.0, 1300.0, 1600.0}) {
      for (const double listMin : {320.0, 420.0}) {
        for (const std::size_t ancestors : {0U, 1U, 2U}) {
          for (std::size_t i = 0; i < 3; ++i) {
            for (const double dir : {1.0, -1.0}) {
              if (runDrag(viewWidth, ancestors, listMin, i, dir, &checksum)) {
                ++drags;
              }
            }
          }
        }
      }
    }
    TEST_CHECK(drags == 132);
    TEST_NEAR(checksum, 3964586789.0, 1e-6);
  }

  void testNarrowingDragsDoNotCollapse() {
    for (const auto& [viewWidth, ancestors, i] : std::array<std::tuple<double, std::size_t, std::size_t>, 4>{{
             {1100, 1, 0},
             {1000, 1, 0},
             {900, 1, 0},
             {1100, 1, 1},
         }}) {
      for (const double listMin : {320.0, 420.0}) {
        const auto run = runDrag(viewWidth, ancestors, listMin, i, 1);
        if (!run) {
          continue;
        }
        const double start = planOf(viewWidth, ancestors, optionsOf(kDefaults, listMin)).columns[i].width;
        TEST_CHECK(run->trace[29] == std::max(140.0, start - 30)); // after 30 px
        TEST_CHECK(run->trace[29] > 140 || start - 30 <= 140);
      }
    }
  }

  void testDragBackRestoresStart() {
    const auto plan0 = planOf(1050, 2, optionsOf(kDefaults, 320));
    const ColumnDrag drag{
        .column = 1, .startWidth = plan0.columns[1].width, .startX = 500, .widths = kDefaults, .spare = plan0.spare
    };
    const auto narrower = dragColumn(drag, 440, 320); // B +60: the list goes from 518 to 458
    TEST_CHECK(narrower.list == 458.0);
    TEST_CHECK(dragColumn(drag, 500, 320) == kDefaults);
    const auto q = planOf(1050, 2, optionsOf(narrower, 320));
    const auto wide = resizeColumn(narrower, q, 1, 140, 320); // the list is 568 again, at least its preferred width
    TEST_CHECK(!wide.list.has_value());
  }

  void testDegenerateInputs() {
    for (const double width : {0.0, -5.0, kNan, -kInfinity}) {
      const auto plan = planOf(width, 4, optionsOf(kDefaults, 320));
      TEST_CHECK(plan.stripsWidth == 70 && plan.strips.size() == 2); // the narrowest pane: one band and the stack
      TEST_CHECK(plan.columns.empty());
      TEST_CHECK(plan.list.x == 70 && plan.list.width == 0);
      TEST_CHECK(plan.margin == 0 && plan.spare == -390);
    }
    const auto fallback = planOf(1100, 0, optionsOf(kDefaults, kNan));
    TEST_CHECK(fallback.spare == planOf(1100, 0, optionsOf(kDefaults, 320)).spare);
    TEST_CHECK(planOf(1100, 0, optionsOf(kDefaults, 0)).spare == fallback.spare);
    auto capped = optionsOf(kDefaults, 320);
    capped.maxColumns = 1;
    TEST_CHECK(planOf(1600, 0, capped).columns.size() == 1);
    const auto plan = planOf(1100, 0, optionsOf(kDefaults, 320));
    TEST_CHECK(resizeColumn(kDefaults, plan, 0, kNan, 320) == kDefaults);
    TEST_CHECK(resizeColumn(kDefaults, plan, 5, 300, 320) == kDefaults);
    TEST_CHECK(resizeColumn(kDefaults, plan, 0, kInfinity, 320).columns[0] == *maxColumnWidth(plan, 0));
    TEST_CHECK(resizeColumn(kDefaults, plan, 0, 260, 320) == kDefaults);
    TEST_CHECK(!startColumnDrag(planOf(600, 0, optionsOf(kDefaults, 320)), 1, 0, kDefaults).has_value());
    const auto drag = startColumnDrag(plan, 0, 700, kDefaults);
    TEST_CHECK(drag.has_value());
    TEST_CHECK(dragColumn(*drag, kNan, 320) == kDefaults);
    TEST_CHECK(dragColumn(*drag, -kInfinity, 320).columns[0] == *maxColumnWidth(plan, 0));
    TEST_CHECK(dragColumn(*drag, kInfinity, 320).columns[0] == kColumnMin);
    auto outside = *drag;
    outside.column = 3;
    TEST_CHECK(dragColumn(outside, 600, 320) == kDefaults);
  }
} // namespace

int main() {
  testColumnCount();
  testPlanTiles();
  testListKeepsPreferredWidth();
  testNoStrips();
  testWideViewCentered();
  testCustomWidths();
  testMaxColumnWidth();
  testResizeSweepHasNoCliff();
  testResizeRemembersNarrowList();
  testColumnFont();
  testChainFollowsTrail();
  testChainCursorAndPointer();
  testChainFileUnderCursor();
  testChainSteering();
  testChainFileRowFillsNextSlot();
  testChainEndsWithoutSubfolders();
  testChainDegenerate();
  testWholeDragsFollowPointer();
  testNarrowingDragsDoNotCollapse();
  testDragBackRestoresStart();
  testDegenerateInputs();
  std::println("workbench column plan checks passed");
  return 0;
}
