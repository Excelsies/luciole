#include "test_check.h"
#include "workbench/ring_geometry.h"
#include "workbench_ring_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <print>
#include <string>
#include <vector>

namespace {
  using namespace luciole::workbench::ring;
  using namespace luciole::test::ring;

  void testSkewedIsDeterministic() {
    const auto weights = skewed(5, 1, 1);
    TEST_NEAR(weights[0], 0.7049832636792213, 1e-15);
    TEST_NEAR(weights[4], 0.05064362863302231, 1e-15);
  }

  void testFitCountMoreArcIsAFixed24Degrees() {
    struct Case {
      std::size_t count;
      std::uint32_t seed;
      double power;
    };
    for (const auto [count, seed, power] : {Case{60, 1, 1}, Case{500, 3, 3}, Case{2500, 4, 6}, Case{5000, 5, 12}}) {
      const auto weights = skewed(count, seed, power);
      for (const std::size_t rows : {6U, 12U, 25U}) {
        const auto fit = fitCount(weights, rows, 1.5);
        TEST_CHECK(fit.more);
        TEST_NEAR(sum(fit.angles), 360.0, kClose);
        TEST_NEAR(fit.angles.back(), 24.0, kClose);
        TEST_NEAR(sum(std::vector(fit.angles.begin(), fit.angles.end() - 1)), 336.0, kClose);
        TEST_CHECK(fit.split <= rows && fit.angles.size() - fit.split <= rows);
        if (count >= 500) {
          TEST_CHECK(fit.angles.size() >= 2 * rows - 2);
        }
      }
    }
    const auto fit = fitCount(skewed(60, 1, 1), 6, 1.5);
    TEST_CHECK(fit.shown == 11 && fit.split == 6 && fit.angles.size() == 12);
    TEST_NEAR(fit.angles[0], 32.69800623084145, 1e-9);
  }

  void testFitCountBalancesColumnsAndKeepsArcsOnTheirSide() {
    const auto fit = fitCount(skewed(5000, 6, 4), 25, 1.5);
    TEST_CHECK(fit.split >= 20 && fit.split <= 30);
    TEST_CHECK(fit.shown == 49 && fit.split == 25);
    TEST_NEAR(fit.angles[0], 6.974202083591616, 1e-9);
    for (const double power : {1.0, 3.0, 8.0, 30.0}) {
      const auto uneven = fitCount(skewed(400, 12, power), 20, 1.5);
      const auto middles = mids(uneven.angles);
      for (std::size_t i = 0; i < middles.size(); ++i) {
        TEST_CHECK(i < uneven.split ? middles[i] <= 210 + 1e-9 : middles[i] >= 150 - 1e-9);
      }
      TEST_CHECK(uneven.split <= 20 && uneven.angles.size() - uneven.split <= 20);
    }
    const auto steep = fitCount(skewed(400, 12, 30), 20, 1.5);
    TEST_CHECK(steep.shown == 31 && steep.split == 12);
  }

  void testFitCountUsesEveryRowForASmallFolder() {
    const auto fit = fitCount(std::vector{5.0, 4.0, 3.0}, 12, 1.5);
    TEST_CHECK(fit.shown == 3);
    TEST_CHECK(!fit.more);
    TEST_NEAR(sum(fit.angles), 360.0, kClose);
    const auto empty = fitCount(std::vector<double>{}, 12, 1.5);
    TEST_CHECK(empty.shown == 0 && !empty.more && empty.angles.empty() && empty.split == 0);
    TEST_CHECK(fitCount(std::vector{7.0}, 12, 1.5).angles[0] == 360);
  }

  void testFitCountShowsFewerArcsWhenNoSplitWorks() {
    const auto two = fitCount(std::vector{1000.0, 1.0}, 12, 1.5);
    TEST_CHECK(two.shown == 2 && !two.more && two.split == 1);
    TEST_CHECK((two.angles == std::vector{358.5, 1.5}));
    const auto dominant = fitCount(std::vector{100.0, 1.0, 1.0, 1.0, 1.0, 1.0}, 2, 1.5);
    TEST_CHECK(dominant.shown == 2 && dominant.more && dominant.split == 1);
    TEST_NEAR(dominant.angles[0], 332.6732673267327, 1e-9);
    TEST_NEAR(dominant.angles[2], 24.0, 1e-12);
    const auto single = fitCount(std::vector{1.0, 1.0, 1.0, 1.0, 1.0}, 1, 1.5);
    TEST_CHECK(single.shown == 1 && single.more && single.split == 1);
    TEST_CHECK((single.angles == std::vector{336.0, 24.0}));
    const auto noRows = fitCount(std::vector{1.0, 1.0}, 0, 1.5);
    TEST_CHECK(noRows.shown == 1 && noRows.more && noRows.split == 1);
    TEST_CHECK((noRows.angles == std::vector{336.0, 24.0}));
    const auto lone = fitCount(std::vector{7.0}, 0, 1.5);
    TEST_CHECK(lone.shown == 1 && !lone.more && lone.split == 1);
    TEST_CHECK((lone.angles == std::vector{360.0}));
  }

  void testFitCountTreatsHugeRowCountsAsEnoughRows() {
    const std::vector weights{3.0, 2.0, 1.0};
    const auto enough = fitCount(weights, 3, 1.5);
    TEST_CHECK(enough.shown == 3 && !enough.more && enough.split == 1);
    for (const std::size_t rows :
         {std::numeric_limits<std::size_t>::max() / 2 + 1, std::numeric_limits<std::size_t>::max()}) {
      const auto huge = fitCount(weights, rows, 1.5);
      TEST_CHECK(huge.shown == enough.shown && huge.more == enough.more && huge.split == enough.split);
      TEST_CHECK(huge.angles == enough.angles);
    }
  }

  std::vector<Arc> arcsOf(const std::vector<RingSegment>& segments) {
    std::vector<Arc> arcs;
    arcs.reserve(segments.size());
    for (const auto& segment : segments) {
      arcs.push_back(segment.arc);
    }
    return arcs;
  }

  const RingSegment* entrySegment(const RingSplit& split, std::size_t entry) {
    const auto found = std::ranges::find_if(split.segments, [entry](const RingSegment& segment) {
      return !segment.merged && segment.entry == entry;
    });
    return found == split.segments.end() ? nullptr : &*found;
  }

  void testRingSegmentsCenterTheOnPathSegmentAt12() {
    const std::vector weights{50.0, 30.0, 10.0, 5.0, 3.0, 1.0, 1.0};
    for (std::size_t path = 0; path < weights.size(); ++path) {
      const auto split = ringSegments(weights, path, 2);
      const auto* onPath = entrySegment(split, path);
      TEST_CHECK(onPath != nullptr);
      TEST_NEAR((onPath->arc.a0 + onPath->arc.a1) / 2, 0.0, kClose);
      TEST_NEAR(split.segments.back().arc.a1 - split.segments.front().arc.a0, 360.0, kClose);
      for (std::size_t i = 1; i < split.segments.size(); ++i) {
        TEST_NEAR(split.segments[i].arc.a0, split.segments[i - 1].arc.a1, kClose);
      }
    }
    const auto merged = ringSegments(weights, 2, 5);
    TEST_CHECK(merged.segments.size() == 6);
    TEST_CHECK(merged.segments.back().merged);
    TEST_CHECK((merged.merged == std::vector<std::size_t>{5, 6}));
    TEST_NEAR(merged.segments[0].arc.a0, -306.0, 1e-9);
    TEST_NEAR(merged.segments[2].arc.a1, 18.0, 1e-9);
    TEST_NEAR(merged.segments[5].arc.a0, 46.80000000000001, 1e-9);
    TEST_NEAR(merged.segments[5].arc.a1, 54.0, 1e-9);
  }

  void testRingSegmentsMergeSmallEntriesNeverThePath() {
    std::vector weights(301, 1.0);
    weights[0] = 1000;
    const auto split = ringSegments(weights, 200, 2);
    const auto rest = std::ranges::count_if(split.segments, [](const RingSegment& segment) { return segment.merged; });
    TEST_CHECK(rest == 1);
    TEST_CHECK(entrySegment(split, 200) != nullptr);
    TEST_CHECK(std::ranges::all_of(split.segments, [](const RingSegment& segment) {
      return segment.merged || segment.arc.a1 - segment.arc.a0 >= 2 - 1e-9;
    }));
    TEST_CHECK(split.merged.size() > 100);
  }

  void testRingSegmentsDegenerateInputs() {
    TEST_CHECK(ringSegments(std::vector<double>{}, 0, 2).segments.empty());
    TEST_CHECK(ringSegments(std::vector{0.0, 0.0}, 0, 2).segments.empty());
    TEST_CHECK(ringSegments(std::vector{1.0, 2.0}, 2, 2).segments.empty());
    TEST_CHECK(ringSegments(std::vector{kNan, kInfinity}, 0, 2).segments.empty());
    const auto lone = ringSegments(std::vector{0.0, 3.0}, 0, 2);
    TEST_CHECK(lone.segments.size() == 2);
    TEST_NEAR(lone.segments[0].arc.a0, -1.0, 1e-12);
    TEST_NEAR(lone.segments[0].arc.a1, 1.0, 1e-12);
  }

  void testLocateFindsTheSegmentAcrossTheWrap() {
    const auto split = ringSegments(std::vector{60.0, 30.0, 10.0}, 1, 2);
    const auto arcs = arcsOf(split.segments);
    const auto* onPath = entrySegment(split, 1);
    TEST_CHECK(split.segments[locate(arcs, 0).value()].entry == 1);
    TEST_CHECK(split.segments[locate(arcs, 360 + (onPath->arc.a0 + onPath->arc.a1) / 2).value()].entry == 1);
    TEST_CHECK(split.segments[locate(arcs, -290).value()].entry == 2);
    TEST_CHECK(!locate(std::vector<Arc>{}, 10).has_value());
    TEST_CHECK(!locate(arcs, kNan).has_value());
    TEST_CHECK(!locate(std::vector{Arc{.a0 = 0, .a1 = 90}}, 180).has_value());
    TEST_NEAR(wrapDegrees(-90), 270.0, 1e-12);
    TEST_NEAR(wrapDegrees(725), 5.0, 1e-12);
  }

  void testComposeFillsTheSpanExactly() {
    const auto pieces = compose(std::vector{50.0, 20.0, 1.0, 1.0, 1.0}, Arc{.a0 = 30, .a1 = 90}, 3);
    TEST_NEAR(pieces.front().arc.a0, 30.0, kClose);
    TEST_NEAR(pieces.back().arc.a1, 90.0, kClose);
    TEST_CHECK(pieces.back().merged);
    TEST_CHECK(pieces.size() == 3 && pieces[0].entry == 0 && pieces[1].entry == 1);
    TEST_NEAR(pieces[0].arc.a1, 70.71428571428572, 1e-9);
    TEST_NEAR(pieces[1].arc.a1, 87.0, 1e-9);
    TEST_CHECK(compose(std::vector<double>{}, Arc{.a0 = 0, .a1 = 10}, 1).empty());
    TEST_CHECK(compose(std::vector{1.0}, Arc{.a0 = 10, .a1 = 10}, 1).empty());
    TEST_CHECK(compose(std::vector{1.0}, Arc{.a0 = 10, .a1 = kNan}, 1).empty());
  }

  double outerRadius(const Dial& dial) {
    double radius = dial.rOut;
    for (const auto thickness : dial.thickness) {
      radius += thickness + dial.gap;
    }
    return radius;
  }

  void testDialReservesContextRingsAndKeepsColumnsWide() {
    for (const auto [width, height] : {Point{1100, 605}, Point{1400, 805}, Point{2560, 1250}}) {
      const auto dial = fullDial(width, height).value();
      TEST_CHECK(dial.rOut > dial.rIn && dial.rIn > dial.rHub && dial.rHub > 40);
      TEST_CHECK(dial.right.x1 - dial.right.x0 >= 250);
      TEST_CHECK(dial.left.x1 - dial.left.x0 >= 250);
      TEST_CHECK(dial.stackBottom - kStackRow * static_cast<double>(kStackRows) >= 4);
      TEST_CHECK(dial.center.y + dial.rTotal + dial.lane + 6 <= height - 4);
      TEST_CHECK(dial.yMin - 12 >= 0 && dial.yMax + 12 <= height);
      const auto bands = ringBands(dial.rOut);
      TEST_CHECK(bands.size() == 4);
      TEST_NEAR(bands[3].r1, dial.rTotal, kClose);
    }
  }

  void testFullDialMetrics() {
    const auto dial = fullDial(1400, 737).value();
    TEST_CHECK((dial.center == Point{700, 409}));
    TEST_CHECK(dial.rTotal == 298 && dial.rOut == 241 && dial.rIn == 121 && dial.rHub == 113 && dial.rBase == 304);
    TEST_CHECK(dial.right.x0 == 1036 && dial.right.x1 == 1388 && dial.left.x0 == 12 && dial.left.x1 == 364);
    TEST_CHECK(dial.yMin == 117 && dial.yMax == 701 && dial.rowsFit == 25 && dial.stackBottom == 85);
    TEST_CHECK(dial.lane == 18 && dial.gap == 3 && dial.stackRows == 5 && dial.stackRow == 15);
    TEST_CHECK((dial.thickness == std::vector{22.0, 12.0, 7.0, 4.0}));
    TEST_CHECK(!dial.compact && !dial.legend && dial.readoutHeight == 0);
    const auto small = fullDial(240, 220).value();
    TEST_CHECK(small.rTotal == 110 && small.rOut == 53 && small.rIn == 27 && small.rowsFit == 9);
    TEST_CHECK((ringBands(100) == std::vector<Band>{{103, 125}, {128, 140}, {143, 150}, {153, 157}}));
    const auto east = polar(Point{10, 20}, 5, 90);
    TEST_NEAR(east.x, 15.0, 1e-12);
    TEST_NEAR(east.y, 20.0, 1e-12);
  }

  void testDialsRefuseHostsThatCannotBeLaidOut() {
    for (const auto [width, height] :
         {Point{0, 500}, Point{500, 0}, Point{-1, 500}, Point{kNan, 500}, Point{500, kInfinity}}) {
      TEST_CHECK(!fullDial(width, height).has_value());
      TEST_CHECK(!compactDial(width, height).has_value());
    }
    const auto huge = fullDial(1e12, 1e12).value();
    TEST_CHECK(huge.width == kMaxHostSize && huge.height == kMaxHostSize);
    TEST_CHECK(compactDial(500, 500, 99).value().thickness.size() == 4);
  }

  void testChooseCompactWidthHasADeadBand() {
    bool compact = chooseCompact(1000, 600);
    int flips = 0;
    TEST_CHECK(!compact);
    for (int width = 1000; width >= 700; width -= 2) {
      const bool next = chooseCompact(width, 600, compact);
      flips += next != compact ? 1 : 0;
      compact = next;
    }
    TEST_CHECK(compact);
    TEST_CHECK(flips == 1);
    for (int width = 700; width <= 1000; width += 2) {
      const bool next = chooseCompact(width, 600, compact);
      flips += next != compact ? 1 : 0;
      compact = next;
    }
    TEST_CHECK(!compact);
    TEST_CHECK(flips == 2);
    TEST_CHECK(!chooseCompact(1100, 537));
    TEST_CHECK(!chooseCompact(1400, 737));
    TEST_CHECK(chooseCompact(340, 380));
    TEST_CHECK(chooseCompact(880, 500, true));
    TEST_CHECK(!chooseCompact(880, 500, false));
  }

  void testChooseCompactHeightHasTheSameDeadBand() {
    bool compact = chooseCompact(1000, 600);
    int flips = 0;
    TEST_CHECK(!compact);
    for (int height = 600; height >= 400; --height) {
      const bool next = chooseCompact(1000, height, compact);
      flips += next != compact ? 1 : 0;
      compact = next;
    }
    TEST_CHECK(compact);
    TEST_CHECK(flips == 1);
    for (int height = 400; height <= 600; ++height) {
      const bool next = chooseCompact(1000, height, compact);
      flips += next != compact ? 1 : 0;
      compact = next;
    }
    TEST_CHECK(!compact);
    TEST_CHECK(flips == 2);
    TEST_CHECK(chooseCompact(1000, 459, false));
    TEST_CHECK(!chooseCompact(1000, 460, false));
    TEST_CHECK(chooseCompact(870, 470, true));
    TEST_CHECK(!chooseCompact(870, 470, false));
    TEST_CHECK(!chooseCompact(900, 490, true));
    TEST_CHECK(chooseCompact(899, 490, true));
    TEST_CHECK(chooseCompact(900, 489, true));
    TEST_CHECK(chooseCompact(899, 500, true));
    TEST_CHECK(chooseCompact(860, 459, false));
    TEST_CHECK(chooseCompact(700, 400, true));
    TEST_CHECK(chooseCompact(880, 480, true));
    TEST_CHECK(!chooseCompact(880, 480, false));
  }

  void testCompactDialFitsItsHostWithReadableRings() {
    for (const auto [width, height] :
         {Point{240, 220}, Point{260, 240}, Point{340, 380}, Point{600, 240}, Point{500, 500}, Point{850, 450}}) {
      for (const std::size_t rings : {0U, 1U, 3U, 4U}) {
        const auto dial = compactDial(width, height, rings).value();
        const double outer = outerRadius(dial);
        TEST_CHECK(dial.center.x - outer >= 0 && dial.center.x + outer <= width);
        TEST_CHECK(dial.center.y - outer >= 0 && dial.center.y + outer <= height - dial.readoutHeight + 1);
        TEST_CHECK(dial.rOut > dial.rIn && dial.rIn > dial.rHub && dial.rHub > 8);
        TEST_CHECK(dial.stackBottom - dial.stackRow * static_cast<double>(dial.stackRows) >= 0);
        for (std::size_t k = 0; k < std::min<std::size_t>(3, rings); ++k) {
          TEST_CHECK(dial.thickness[k] >= 12);
        }
        if (dial.legend) {
          TEST_CHECK(dial.right.x1 - dial.right.x0 >= 150 && dial.right.x0 >= dial.center.x + dial.rTotal);
        }
      }
    }
    TEST_CHECK(compactDial(600, 240).value().legend);
    TEST_CHECK(!compactDial(340, 380).value().legend);
    TEST_CHECK(compactDial(406, 240).value().legend);
    TEST_CHECK(!compactDial(405, 240).value().legend);
  }

  void testCompactDialMetrics() {
    const auto narrow = compactDial(340, 380, 2).value();
    TEST_CHECK((narrow.center == Point{170, 203}));
    TEST_CHECK(narrow.rTotal == 151 && narrow.rOut == 123 && narrow.rIn == 57 && narrow.rHub == 52);
    TEST_CHECK(narrow.rBase == 155 && narrow.rowsFit == 12 && narrow.yMin == 119 && narrow.yMax == 342);
    TEST_CHECK(narrow.stackBottom == 49 && narrow.stackRows == 3 && narrow.stackRow == 14);
    TEST_CHECK((narrow.thickness == std::vector{12.0, 12.0}));
    TEST_CHECK(narrow.compact && !narrow.legend && narrow.readoutHeight == 22 && narrow.lane == 0);
    const auto wide = compactDial(600, 240, 4).value();
    TEST_CHECK((wide.center == Point{112, 110}));
    TEST_CHECK(wide.rTotal == 104 && wide.rOut == 55 && wide.rIn == 25 && wide.rHub == 20);
    TEST_CHECK(wide.right.x0 == 228 && wide.right.x1 == 592 && wide.left.x0 == 8 && wide.left.x1 == 8);
    TEST_CHECK(wide.yMin == 26 && wide.yMax == 202 && wide.rowsFit == 8 && wide.stackRows == 0);
    TEST_CHECK(wide.legend);
  }

  void testPlaceRowsKeepsOrderSpacingAndBounds() {
    const auto rows = placeRows(std::vector{10.0, 11.0, 12.0, 13.0, 300.0, 301.0, 999.0, 999.0, 999.0}, 24, 20, 400);
    TEST_CHECK(rows.size() == 9);
    for (std::size_t i = 1; i < rows.size(); ++i) {
      TEST_CHECK(rows[i] - rows[i - 1] >= 24 - 1e-9);
    }
    TEST_CHECK(rows.front() >= 20 && rows.back() <= 400);
    TEST_CHECK((rows == std::vector{20.0, 44.0, 68.0, 92.0, 288.5, 312.5, 352.0, 376.0, 400.0}));
    TEST_CHECK((placeRows(std::vector{100.0}, 24, 20, 400) == std::vector{100.0}));
    TEST_CHECK(placeRows(std::vector<double>{}, 24, 20, 400).empty());
    TEST_CHECK((placeRows(std::vector{50.0, 50.0}, 24, 20, 400) == std::vector{38.0, 62.0}));
  }

  void testInSectorHonorsRadiiAnglesAndPadding() {
    const Point center{100, 100};
    const Arc east{.a0 = 60, .a1 = 120};
    const Band band{.r0 = 20, .r1 = 60};
    TEST_CHECK(inSector(center, east, band, Point{140, 100}));
    TEST_CHECK(!inSector(center, east, band, Point{60, 100}));
    TEST_CHECK(!inSector(center, east, band, Point{110, 100}));
    TEST_CHECK(!inSector(center, east, band, Point{159.5, 100}));
    TEST_CHECK(inSector(center, east, band, Point{159.5, 100}, 0));
    TEST_CHECK(!inSector(center, east, band, Point{kNan, 100}));
    TEST_CHECK(!inSector(center, east, Band{.r0 = 0, .r1 = 60}, center, 0));
    const Arc acrossNoon{.a0 = -30, .a1 = 30};
    TEST_CHECK(inSector(center, acrossNoon, band, Point{95, 60}));
    TEST_CHECK(!inSector(center, acrossNoon, band, Point{140, 100}));
  }

  void testLabelSpotFitsAWideArcAndRefusesANarrowOne() {
    const auto dial = compactDial(340, 380, 1).value();
    const Arc wide{.a0 = 60, .a1 = 120};
    const Arc thin{.a0 = 10, .a1 = 14};
    const auto spot = labelSpot(dial, wide, 40, 13);
    TEST_CHECK(spot.has_value());
    for (const auto [x, y] : {Point{-1, -1}, Point{1, -1}, Point{-1, 1}, Point{1, 1}}) {
      TEST_CHECK(inSector(
          dial.center, wide, Band{.r0 = dial.rIn, .r1 = dial.rOut}, Point{spot->x + x * 20, spot->y + y * 6.5}, 0
      ));
    }
    TEST_NEAR(spot->x, 270.0, 1e-9);
    TEST_NEAR(spot->y, 203.0, 1e-9);
    TEST_CHECK(!labelSpot(dial, thin, 40, 13).has_value());
    TEST_CHECK(!labelSpot(dial, wide, 400, 13).has_value());
    TEST_CHECK(!labelSpot(dial, wide, kNan, 13).has_value());
  }

  void testLabelSpotMovesAlongTheRadius() {
    const auto dial = compactDial(340, 380, 1).value();
    const auto middle = labelSpot(dial, Arc{.a0 = -20, .a1 = 20}, 60, 13);
    TEST_CHECK(middle.has_value());
    TEST_NEAR(middle->y, 103.0, 1e-9);
    const auto outward = labelSpot(dial, Arc{.a0 = -7, .a1 = 7}, 20, 13);
    TEST_CHECK(outward.has_value());
    TEST_NEAR(outward->y, 89.68, 1e-9);
    const auto farther = labelSpot(dial, Arc{.a0 = -6, .a1 = 6}, 20, 13);
    TEST_CHECK(farther.has_value());
    TEST_NEAR(farther->y, 80.8, 1e-9);
    TEST_NEAR(farther->x, 170.0, 1e-9);
  }

  void testFitCompactFewArcsAndAFixed30DegreeMoreArc() {
    const auto fit = fitCompact(skewed(500, 21, 3), 7, 1.5);
    TEST_CHECK(fit.shown == 7);
    TEST_CHECK(fit.more);
    TEST_CHECK(fit.split == 8);
    TEST_NEAR(fit.angles[7], 30.0, kClose);
    TEST_NEAR(sum(fit.angles), 360.0, kClose);
    TEST_NEAR(fit.angles[0], 48.07322026205261, 1e-9);
    const auto small = fitCompact(std::vector{5.0, 3.0}, 7, 1.5);
    TEST_CHECK(!small.more);
    TEST_NEAR(sum(small.angles), 360.0, kClose);
    TEST_CHECK(fitCompact(std::vector<double>{}, 7, 1.5).angles.empty());
    const auto none = fitCompact(std::vector{5.0, 3.0}, 0, 1.5);
    TEST_CHECK(none.shown == 1 && none.more);
    TEST_CHECK((none.angles == std::vector{330.0, 30.0}));
  }

  void testArcAnglesSumAndMinimum() {
    const auto angles = arcAngles(std::vector{1000.0, 10.0, 1.0, 1.0, 1.0}, 2);
    TEST_NEAR(sum(angles), 360.0, kClose);
    TEST_CHECK(std::ranges::all_of(angles, [](double angle) { return angle >= 2 - 1e-9; }));
    TEST_CHECK(angles[0] > angles[1]);
    TEST_NEAR(angles[0], 350.4950495049505, 1e-9);
    TEST_NEAR(angles[1], 3.504950495049505, 1e-9);
    TEST_NEAR(angles[4], 2.0, 1e-12);
    const auto second = arcAngles(std::vector{100.0, 6.0, 1.0}, 20);
    TEST_NEAR(second[0], 320.0, 1e-9);
    TEST_NEAR(second[1], 20.0, 1e-12);
    TEST_NEAR(second[2], 20.0, 1e-12);
  }

  void testArcAnglesDegenerateInputs() {
    TEST_CHECK(arcAngles(std::vector<double>{}, 2).empty());
    TEST_CHECK((arcAngles(std::vector{5.0}, 2) == std::vector{360.0}));
    TEST_NEAR(sum(arcAngles(std::vector{0.0, 0.0, 0.0}, 2)), 360.0, kClose);
    const auto crowd = arcAngles(std::vector<double>(400, 1.0), 2);
    TEST_NEAR(sum(crowd), 360.0, kClose);
    TEST_NEAR(crowd[0], 0.9, 1e-12);
    TEST_CHECK((arcAngles(std::vector{kNan, -5.0, kInfinity}, 2) == std::vector{120.0, 120.0, 120.0}));
    TEST_CHECK((arcAngles(std::vector{3.0, kNan, 1.0}, 0) == std::vector{270.0, 0.0, 90.0}));
    TEST_CHECK((arcAngles(std::vector{1.0, 1.0}, 2, kNan) == std::vector{0.0, 0.0}));
    TEST_CHECK((arcAngles(std::vector{1.0, 1.0}, 2, -10) == std::vector{0.0, 0.0}));
    TEST_CHECK((arcStarts(std::vector{10.0, 20.0, 30.0}, 5) == std::vector{5.0, 15.0, 35.0}));
  }
} // namespace

int main() {
  testArcAnglesSumAndMinimum();
  testArcAnglesDegenerateInputs();
  testSkewedIsDeterministic();
  testFitCountMoreArcIsAFixed24Degrees();
  testFitCountBalancesColumnsAndKeepsArcsOnTheirSide();
  testFitCountUsesEveryRowForASmallFolder();
  testFitCountShowsFewerArcsWhenNoSplitWorks();
  testFitCountTreatsHugeRowCountsAsEnoughRows();
  testFitCompactFewArcsAndAFixed30DegreeMoreArc();
  testRingSegmentsCenterTheOnPathSegmentAt12();
  testRingSegmentsMergeSmallEntriesNeverThePath();
  testRingSegmentsDegenerateInputs();
  testLocateFindsTheSegmentAcrossTheWrap();
  testComposeFillsTheSpanExactly();
  testDialReservesContextRingsAndKeepsColumnsWide();
  testFullDialMetrics();
  testDialsRefuseHostsThatCannotBeLaidOut();
  testChooseCompactWidthHasADeadBand();
  testChooseCompactHeightHasTheSameDeadBand();
  testCompactDialFitsItsHostWithReadableRings();
  testCompactDialMetrics();
  testPlaceRowsKeepsOrderSpacingAndBounds();
  testInSectorHonorsRadiiAnglesAndPadding();
  testLabelSpotFitsAWideArcAndRefusesANarrowOne();
  testLabelSpotMovesAlongTheRadius();
  std::println("workbench ring geometry checks passed");
  return 0;
}
