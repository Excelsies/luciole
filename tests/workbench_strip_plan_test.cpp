#include "test_check.h"
#include "workbench/column_plan.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <optional>
#include <print>
#include <vector>

// The ancestor strips pane: how many named bands a pane width shows, where they sit, and the pane in the center.
namespace {
  using namespace luciole::workbench;

  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInfinity = std::numeric_limits<double>::infinity();

  // Named bands shown, and the ancestors folded into the "+N levels" band (0 when there is none).
  struct Bands {
    std::size_t named = 0;
    std::size_t folded = 0;
  };

  constexpr std::array<std::size_t, 8> kCounts{0, 1, 3, 4, 5, 7, 12, 40};

  struct Row {
    double width = 0;
    std::array<Bands, kCounts.size()> bands;
  };

  // Named bands are 44, 34, 26, 20, 20, ... px and the "+N levels" band 24 px, each with a 1 px gap after it. All n
  // fit from 45, 80, 107, 128, 149, 170, 191, ... px; k with the folded band from 70, 105, 132, 153, 174, 195, ... px.
  // Each threshold is listed one pixel below and on it.
  constexpr std::array<Row, 35> kTable{{
      {70, {{{0, 0}, {1, 0}, {1, 2}, {1, 3}, {1, 4}, {1, 6}, {1, 11}, {1, 39}}}}, // the narrowest pane
      {104, {{{0, 0}, {1, 0}, {1, 2}, {1, 3}, {1, 4}, {1, 6}, {1, 11}, {1, 39}}}},
      {105, {{{0, 0}, {1, 0}, {2, 1}, {2, 2}, {2, 3}, {2, 5}, {2, 10}, {2, 38}}}},
      {106, {{{0, 0}, {1, 0}, {2, 1}, {2, 2}, {2, 3}, {2, 5}, {2, 10}, {2, 38}}}},
      {107, {{{0, 0}, {1, 0}, {3, 0}, {2, 2}, {2, 3}, {2, 5}, {2, 10}, {2, 38}}}},
      {127, {{{0, 0}, {1, 0}, {3, 0}, {2, 2}, {2, 3}, {2, 5}, {2, 10}, {2, 38}}}},
      {128, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {2, 3}, {2, 5}, {2, 10}, {2, 38}}}},
      {131, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {2, 3}, {2, 5}, {2, 10}, {2, 38}}}},
      {132, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {3, 2}, {3, 4}, {3, 9}, {3, 37}}}},
      {148, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {3, 2}, {3, 4}, {3, 9}, {3, 37}}}},
      {149, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {3, 4}, {3, 9}, {3, 37}}}},
      {152, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {3, 4}, {3, 9}, {3, 37}}}},
      {153, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {4, 3}, {4, 8}, {4, 36}}}}, // the default
      {173, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {4, 3}, {4, 8}, {4, 36}}}},
      {174, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {5, 2}, {5, 7}, {5, 35}}}},
      {190, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {5, 2}, {5, 7}, {5, 35}}}},
      {191, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {5, 7}, {5, 35}}}},
      {194, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {5, 7}, {5, 35}}}},
      {195, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {6, 6}, {6, 34}}}},
      {215, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {6, 6}, {6, 34}}}},
      {216, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {7, 5}, {7, 33}}}},
      {236, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {7, 5}, {7, 33}}}},
      {237, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {8, 4}, {8, 32}}}},
      {257, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {8, 4}, {8, 32}}}},
      {258, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {9, 3}, {9, 31}}}},
      {278, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {9, 3}, {9, 31}}}},
      {279, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {10, 2}, {10, 30}}}},
      {295, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {10, 2}, {10, 30}}}},
      {296, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {12, 0}, {10, 30}}}},
      {299, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {12, 0}, {10, 30}}}},
      {300, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {12, 0}, {11, 29}}}},
      {316, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {12, 0}, {11, 29}}}},
      {317, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {12, 0}, {11, 29}}}},
      {320, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {12, 0}, {11, 29}}}},
      {321, {{{0, 0}, {1, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {12, 0}, {12, 28}}}}, // the widest pane
  }};

  // Named bands nearest first; after the fourth each is as wide as the fourth.
  double namedWidth(std::size_t level) {
    constexpr std::array<double, 4> kFirst{44, 34, 26, 20};
    return level <= kFirst.size() ? kFirst[level - 1] : 20;
  }

  Bands bandsOf(const std::vector<Strip>& strips) {
    Bands out;
    for (const auto& strip : strips) {
      if (strip.kind == StripKind::Stack) {
        out.folded = strip.lastLevel - strip.level + 1;
      } else {
        ++out.named;
      }
    }
    return out;
  }

  // Farthest first: the "+N levels" band, then the named ones up to the nearest, which ends at `right`, the pane's
  // edge beside the list.
  void checkShape(const std::vector<Strip>& strips, double right, std::size_t count) {
    const auto bands = bandsOf(strips);
    const std::size_t stack = bands.folded > 0 ? 1 : 0;
    TEST_CHECK(strips.size() == bands.named + stack);
    if (stack == 1) {
      const auto& far = strips.front();
      TEST_CHECK(far.kind == StripKind::Stack);
      TEST_CHECK(far.width == 24 && far.level == bands.named + 1 && far.lastLevel == count);
    }
    double edge = right;
    for (std::size_t i = strips.size(); i-- > stack;) {
      const auto& strip = strips[i];
      const std::size_t level = strips.size() - i;
      TEST_CHECK(strip.kind == StripKind::Ancestor);
      TEST_CHECK(strip.level == level && strip.lastLevel == level);
      TEST_CHECK(strip.width == namedWidth(level));
      TEST_CHECK(strip.x + strip.width + 1 == edge);
      edge = strip.x;
    }
    if (stack == 1) {
      TEST_CHECK(strips.front().x + 24 + 1 == edge);
    }
  }

  void bandsAtEachWidth() {
    for (const auto& row : kTable) {
      for (std::size_t c = 0; c < kCounts.size(); ++c) {
        const auto strips = ancestorStrips(kCounts[c], row.width);
        const auto got = bandsOf(strips);
        const auto& want = row.bands[c];
        if (got.named != want.named || got.folded != want.folded) {
          std::println(
              stderr, "{} px, {} ancestors: {} named and {} folded, not {} and {}", row.width, kCounts[c], got.named,
              got.folded, want.named, want.folded
          );
        }
        TEST_CHECK(got.named == want.named && got.folded == want.folded);
        checkShape(strips, row.width, kCounts[c]);
        TEST_CHECK(strips.empty() || strips.front().x >= 0); // the bands fit the pane
      }
    }
  }

  void paneLimits() {
    TEST_CHECK(kStripsMinWidth == 70);
    TEST_CHECK(kStripsDefaultWidth == 153);
    TEST_CHECK(kStripsMaxWidth == 321);
    TEST_CHECK(kMaxStrips == 13);
    TEST_CHECK(stripBandsWidth(0) == 0);
    TEST_CHECK(stripBandsWidth(1) == 45);
    TEST_CHECK(stripBandsWidth(4) == 128);
    TEST_CHECK(stripBandsWidth(13) == 317);
    // a width outside the pane's range reads as the nearest limit, NaN as the narrowest
    for (const double low : {69.0, 0.0, -5.0, -kInfinity, kNan}) {
      TEST_CHECK(bandsOf(ancestorStrips(40, low)).named == 1);
      checkShape(ancestorStrips(40, low), 70, 40);
    }
    for (const double high : {322.0, 5000.0, kInfinity}) {
      TEST_CHECK(bandsOf(ancestorStrips(40, high)).named == 12);
      TEST_CHECK(bandsOf(ancestorStrips(1000000000, high)).folded == 1000000000 - 12);
    }
    TEST_CHECK(ancestorStrips(13, 321).size() == 13); // thirteen fit the widest pane without folding
    TEST_CHECK(ancestorStrips(14, 321).size() == 13); // twelve and the band of the other two
    TEST_CHECK(ancestorStrips(1000000000, 321).size() <= kMaxStrips);
  }

  // The pane's width in a center: the saved width or the default, held between the narrowest pane and the widest
  // the center can spare while the list keeps its minimum.
  void paneWidthInTheCenter() {
    TEST_CHECK(stripsPaneWidth(std::nullopt, 1400, 320) == 153);
    TEST_CHECK(stripsPaneWidth(200, 1400, 320) == 200);
    TEST_CHECK(stripsPaneWidth(200.4, 1400, 320) == 200);
    TEST_CHECK(stripsPaneWidth(10, 1400, 320) == 70);
    TEST_CHECK(stripsPaneWidth(900, 1400, 320) == 321);
    TEST_CHECK(stripsPaneWidth(kNan, 1400, 320) == 153);
    TEST_CHECK(stripsPaneWidth(kInfinity, 1400, 320) == 153);
    // a narrow center: the list keeps 320, the pane gives up the rest down to its minimum
    TEST_CHECK(stripsPaneWidth(300, 600, 320) == 280);
    TEST_CHECK(stripsPaneWidth(std::nullopt, 450, 320) == 130);
    TEST_CHECK(stripsPaneWidth(std::nullopt, 380, 320) == 70);
    TEST_CHECK(stripsPaneWidth(300, 0, 320) == 70);
    // the narrowest center the panes allow leaves the list its 320 beside the narrowest pane
    TEST_CHECK(stripsPaneWidth(300, kCenterMinWidth, kListFloor) == kStripsMinWidth);
    TEST_CHECK(kCenterMinWidth - stripsPaneWidth(std::nullopt, kCenterMinWidth, kListFloor) == kListFloor);
    TEST_CHECK(stripsPaneWidth(300, kNan, 320) == 70);
    TEST_CHECK(stripsPaneWidth(300, 600, kNan) == 280); // an invalid list minimum reads as 320
  }

  PlanOptions paneOf(std::optional<double> width, bool strips = true) {
    PlanOptions options;
    options.strips = strips;
    options.stripsWidth = width;
    return options;
  }

  // The list starts where the pane ends, whatever the depth; the pane's room beyond the bands stays empty.
  void listStartsAfterThePane() {
    for (const std::size_t ancestors : {0U, 1U, 3U, 12U, 40U}) {
      const auto plan = planColumns(1400, ancestors, paneOf(200));
      TEST_CHECK(plan.stripsWidth == 200 && plan.list.x == 200 && plan.margin == 0);
      TEST_CHECK(plan.stripsMax == 321);
      TEST_CHECK(plan.strips.size() == ancestorStrips(ancestors, 200).size());
      if (!plan.strips.empty()) {
        TEST_CHECK(plan.strips.back().x + plan.strips.back().width + 1 == 200);
      }
      const auto byDefault = planColumns(1400, ancestors);
      TEST_CHECK(byDefault.stripsWidth == 153 && byDefault.list.x == 153);
    }
    const auto five = planColumns(1400, 5, paneOf(200));
    TEST_CHECK(five.strips.front().x == 200 - 149); // five bands take 149 px; 51 px stay empty
    // a very wide view centers the group, the pane included
    const auto centered = planColumns(2560, 3, paneOf(200));
    TEST_CHECK(centered.margin == 247); // (2560 - 200 - 593 - 1272) / 2, rounded down
    TEST_CHECK(centered.list.x == 447 && centered.strips.back().x == 447 - 45);
    TEST_CHECK(centered.strips.front().x == 447 - 107);
  }

  // The columns ahead give up their room first, from the last, then the list down to its minimum, then the pane
  // down to its own. The saved width is not changed by any of it.
  void narrowViewsNarrowThePane() {
    const auto roomy = planColumns(1100, 6, paneOf(321));
    TEST_CHECK(roomy.stripsWidth == 321 && roomy.columns.size() == 1); // 779 px: A leaves 518, A + B 327 < 470
    const auto tight = planColumns(640, 6, paneOf(321));
    TEST_CHECK(tight.stripsWidth == 320 && tight.stripsMax == 320 && tight.columns.empty());
    TEST_CHECK(tight.list.width == 320);
    const auto narrow = planColumns(500, 7, paneOf(300));
    TEST_CHECK(narrow.stripsWidth == 180 && narrow.list.x == 180 && narrow.list.width == 320);
    TEST_CHECK(bandsOf(narrow.strips).named == 5 && bandsOf(narrow.strips).folded == 2);
    const auto least = planColumns(360, 6, paneOf(321));
    TEST_CHECK(least.stripsWidth == 70 && least.stripsMax == 70);
    TEST_CHECK(least.list.x == 70 && least.list.width == 290); // the pane goes no narrower than its minimum
    TEST_CHECK(bandsOf(least.strips).named == 1 && bandsOf(least.strips).folded == 5);
    TEST_CHECK(planColumns(0, 6, paneOf(321)).stripsWidth == 70);
    TEST_CHECK(planColumns(kNan, 6, paneOf(321)).list.width == 0);
  }

  void stripsOff() {
    const auto off = planColumns(1400, 7, paneOf(300, false));
    TEST_CHECK(off.strips.empty() && off.stripsWidth == 0 && off.stripsMax == 0 && off.list.x == 0);
  }

  // A saved width is kept when it is a pane width, rounded; anything else is the default.
  void savedWidths() {
    TEST_CHECK(!normalizeStripsWidth(std::nullopt).has_value());
    TEST_CHECK(!normalizeStripsWidth(69.4).has_value());
    TEST_CHECK(normalizeStripsWidth(69.5) == 70.0);
    TEST_CHECK(normalizeStripsWidth(70) == 70.0);
    TEST_CHECK(normalizeStripsWidth(200.4) == 200.0);
    TEST_CHECK(normalizeStripsWidth(320.6) == 321.0);
    TEST_CHECK(normalizeStripsWidth(321) == 321.0);
    TEST_CHECK(normalizeStripsWidth(450) == 450.0); // a width of a larger text scale is kept
    TEST_CHECK(normalizeStripsWidth(963) == 963.0);
    TEST_CHECK(!normalizeStripsWidth(963.5).has_value());
    TEST_CHECK(!normalizeStripsWidth(kNan).has_value());
    TEST_CHECK(!normalizeStripsWidth(kInfinity).has_value());
    TEST_CHECK(!normalizeStripsWidth(-kInfinity).has_value());
  }

  // A width asked for by a drag or a key, held between the pane's minimum and the plan's limit, and rounded.
  void resizingThePane() {
    const auto plan = planColumns(1400, 6);
    TEST_CHECK(resizeStrips(240.4, plan.stripsMax) == 240.0);
    TEST_CHECK(resizeStrips(10, plan.stripsMax) == 70.0);
    TEST_CHECK(resizeStrips(900, plan.stripsMax) == 321.0);
    TEST_CHECK(resizeStrips(kInfinity, plan.stripsMax) == 321.0);
    TEST_CHECK(resizeStrips(-kInfinity, plan.stripsMax) == 70.0);
    TEST_CHECK(!resizeStrips(kNan, plan.stripsMax).has_value());
    const auto narrow = planColumns(500, 6);
    TEST_CHECK(resizeStrips(300, narrow.stripsMax) == 180.0); // the list keeps its 320 px
    TEST_CHECK(resizeStrips(200, 0) == 70.0);                 // a limit under the minimum reads as the minimum
    TEST_CHECK(resizeStrips(200, kNan) == 70.0);
    TEST_CHECK(resizeStrips(400, 2000) == 321.0); // never past the widest pane
  }

  // A drag starts at the pane's edge beside the list; moving away from the strips widens the pane.
  void draggingThePane() {
    const auto plan = planColumns(1400, 6, paneOf(200));
    const auto drag = startStripsDrag(plan, 200.0);
    TEST_CHECK(drag.startWidth == 200 && drag.startX == 200 && drag.max == 321 && drag.saved == 200.0);
    TEST_CHECK(dragStrips(drag, 200) == 200.0);
    TEST_CHECK(dragStrips(drag, 260.4) == 260.0);
    TEST_CHECK(dragStrips(drag, 100) == 100.0);
    TEST_CHECK(dragStrips(drag, 0) == 70.0);
    TEST_CHECK(dragStrips(drag, 900) == 321.0);
    TEST_CHECK(!dragStrips(drag, kNan).has_value());
    // on a centered view the edge is past the margin, and the width follows the pointer's travel
    const auto centered = planColumns(2560, 3);
    const auto wide = startStripsDrag(centered, std::nullopt);
    TEST_CHECK(wide.startX == centered.margin + 153 && wide.startWidth == 153 && !wide.saved);
    TEST_CHECK(dragStrips(wide, wide.startX + 40) == 193.0);
    // a narrow view drew the pane narrower than its saved width: the drag starts from the width drawn
    const auto narrow = planColumns(500, 7, paneOf(300));
    const auto squeezed = startStripsDrag(narrow, 300.0);
    TEST_CHECK(squeezed.startWidth == 180 && squeezed.max == 180 && squeezed.saved == 300.0);
    TEST_CHECK(dragStrips(squeezed, 170) == 170.0 && dragStrips(squeezed, 200) == 180.0);
  }

  // A key moves the divider by a step from the saved width, never from a width the window squeezed.
  void steppingThePane() {
    const auto narrow = planColumns(500, 7, paneOf(300));
    const auto squeezed = startStripsDrag(narrow, 300.0);
    TEST_CHECK(squeezed.startWidth == 180 && squeezed.wanted == 300);
    TEST_CHECK(stepStrips(squeezed, squeezed.startX + 16) == 316.0);
    TEST_CHECK(stepStrips(squeezed, squeezed.startX - 16) == 284.0);
    TEST_CHECK(stepStrips(squeezed, squeezed.startX + 100) == 321.0);
    TEST_CHECK(stepStrips(squeezed, squeezed.startX - 400) == 70.0);
    TEST_CHECK(!stepStrips(squeezed, kNan).has_value());
    const auto plan = planColumns(1400, 6);
    const auto unsaved = startStripsDrag(plan, std::nullopt);
    TEST_CHECK(unsaved.wanted == 153 && stepStrips(unsaved, unsaved.startX + 16) == 169.0);
    const auto saved = startStripsDrag(plan, 250.4);
    TEST_CHECK(saved.wanted == 250 && stepStrips(saved, saved.startX - 16) == 234.0);
    const auto hostile = startStripsDrag(plan, kNan);
    TEST_CHECK(hostile.wanted == 153);
  }
} // namespace

int main() {
  bandsAtEachWidth();
  paneLimits();
  paneWidthInTheCenter();
  listStartsAfterThePane();
  narrowViewsNarrowThePane();
  stripsOff();
  savedWidths();
  resizingThePane();
  draggingThePane();
  steppingThePane();
  std::println("workbench strip plan checks passed");
  return 0;
}
