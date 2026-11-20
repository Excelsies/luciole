#include "test_check.h"
#include "workbench/ladder_plan.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <limits>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using luciole::WorkbenchMeasure;
  using luciole::test::workbenchTree;
  using Heights = std::vector<double>;
  using Widths = std::vector<double>;

  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInfinity = std::numeric_limits<double>::infinity();

  // Shares of a power-law list of n entries (heaviest first), normalized over all n, first `top` of them.
  std::vector<double> powerShares(std::size_t n, std::size_t top) {
    double total = 0;
    for (std::size_t i = 0; i < n; ++i) {
      total += 1.0 / static_cast<double>(i + 1);
    }
    std::vector<double> shares;
    shares.reserve(std::min(n, top));
    for (std::size_t i = 0; i < std::min(n, top); ++i) {
      shares.push_back(1.0 / static_cast<double>(i + 1) / total);
    }
    return shares;
  }

  Heights heightsOf(const std::vector<LadderBar>& bars) {
    Heights heights;
    for (const auto& bar : bars) {
      heights.push_back(bar.h);
    }
    return heights;
  }

  Widths widthsOf(const LadderSegments& plan) {
    Widths widths;
    for (const auto& seg : plan.segs) {
      widths.push_back(seg.w);
    }
    return widths;
  }

  double segmentSum(const LadderSegments& plan) {
    double sum = 0;
    for (const auto& seg : plan.segs) {
      sum += seg.w;
    }
    return sum;
  }

  const LadderSegment* findSegment(const LadderSegments& plan, std::size_t index) {
    const auto found = std::ranges::find(plan.segs, index, &LadderSegment::index);
    return found == plan.segs.end() ? nullptr : &*found;
  }

  LadderSegmentOptions forced(std::size_t index, double share, double forceMin = 8) {
    LadderSegmentOptions options;
    options.force = LadderForce{.index = index, .share = share};
    options.forceMin = forceMin;
    return options;
  }

  void testSegmentsTileTheBar() {
    for (const double width : {220.0, 300.0, 420.0, 800.0}) {
      for (const std::size_t n : {1U, 2U, 7U, 60U, 5000U}) {
        const auto plan = ladderSegments(powerShares(n, 64), n, width, {});
        double x = 0;
        for (const auto& seg : plan.segs) {
          TEST_CHECK(seg.x == x);
          TEST_CHECK(seg.w >= 3);
          x += seg.w;
        }
        if (plan.rest) {
          TEST_CHECK(plan.rest->x == x);
          x += plan.rest->w;
        }
        TEST_CHECK(x == width);
      }
    }
  }

  void testRemainderCappedAndCounted() {
    const auto plan = ladderSegments(powerShares(5000, 64), 5000, 420, {});
    TEST_CHECK(plan.rest.has_value());
    TEST_CHECK(plan.rest->w <= 63);
    TEST_CHECK(plan.rest->count == 5000 - plan.segs.size());
    const Widths expected{90, 45, 30, 23, 18, 15, 13, 11, 10, 9, 8, 8, 7, 6, 6,
                          6,  5,  5,  5,  4,  4,  5,  4,  3,  4, 3, 4, 3, 3};
    TEST_CHECK(widthsOf(plan) == expected);
    TEST_CHECK(plan.rest->x == 357);
    TEST_CHECK(plan.rest->w == 63);
    TEST_CHECK(plan.rest->count == 4971);
  }

  void testShortListFollowsWeights() {
    const std::vector<double> shares{0.5, 0.3, 0.2};
    const auto plan = ladderSegments(shares, 3, 400, {});
    TEST_CHECK(!plan.rest.has_value());
    TEST_CHECK((widthsOf(plan) == Widths{200, 120, 80}));
  }

  void testAtMostFortySegments() {
    const auto plan = ladderSegments(powerShares(500, 64), 500, 2000, {});
    TEST_CHECK(plan.segs.size() <= 40);
    LadderSegmentOptions few;
    few.maxCount = 5;
    TEST_CHECK(ladderSegments(powerShares(500, 64), 500, 2000, few).segs.size() <= 5);
  }

  // Whatever maxCount says, the first 64 entries are the most a bar shows.
  void testAtMostSixtyFourSegmentsWhateverMaxCount() {
    LadderSegmentOptions unlimited;
    unlimited.maxCount = std::numeric_limits<std::size_t>::max();
    const std::vector<double> even(200, 0.005);
    const auto plan = ladderSegments(even, 200, 2000, unlimited);
    TEST_CHECK(plan.segs.size() == 64);
    TEST_CHECK(plan.segs[0].x == 0 && plan.segs[0].w == 27 && plan.segs[63].x == 1673);
    TEST_CHECK(plan.rest.has_value() && plan.rest->x == 1700 && plan.rest->w == 300 && plan.rest->count == 136);
    const auto many = ladderSegments(powerShares(100000, 100000), 100000, 400, unlimited);
    TEST_CHECK(!many.segs.empty() && many.segs.size() <= 64);
    TEST_CHECK(many.rest.has_value() && many.rest->x + many.rest->w == 400);
  }

  void testForcedTailEntryKeptLast() {
    const auto plan = ladderSegments(powerShares(3000, 64), 3000, 420, forced(2500, 1e-4));
    const auto* seg = findSegment(plan, 2500);
    TEST_CHECK(seg != nullptr && seg->w >= 8);
    TEST_CHECK(plan.segs.back().index == 2500);
    TEST_CHECK(segmentSum(plan) + (plan.rest ? plan.rest->w : 0.0) == 420);
  }

  void testForcedShownEntryLifted() {
    const std::vector<double> shares{0.9, 0.05, 0.03, 0.01, 0.01};
    const auto plan = ladderSegments(shares, 5, 300, forced(4, 0.01));
    const auto* seg = findSegment(plan, 4);
    TEST_CHECK(seg != nullptr && seg->w >= 8);
  }

  void testDominantEntryDrawnDominant() {
    const std::vector<double> nine{0.9, 0.1};
    const auto plan = ladderSegments(nine, 2, 400, {});
    TEST_CHECK(!plan.rest.has_value());
    TEST_NEAR(plan.segs[0].w, 360.0, 1.0);
    TEST_NEAR(plan.segs[1].w, 40.0, 1.0);
    const std::vector<double> seven{0.7, 0.3};
    const auto other = ladderSegments(seven, 2, 400, {});
    TEST_NEAR(other.segs[0].w, 280.0, 1.0);
    TEST_NEAR(other.segs[1].w, 120.0, 1.0);
  }

  void testHiddenOnPathEntryTakesItsOwnWidth() {
    const std::vector<double> shares{0.9, 0.1};
    const auto plan = ladderSegments(shares, 3, 400, forced(2, 0.5));
    TEST_CHECK(!plan.rest.has_value());
    TEST_CHECK(plan.segs.size() == 3);
    TEST_CHECK(plan.segs[0].index == 0 && plan.segs[1].index == 1 && plan.segs[2].index == 2);
    TEST_CHECK(plan.segs[2].w >= 8);
    TEST_NEAR(plan.segs[0].w / plan.segs[1].w, 9.0, 0.6);
    TEST_CHECK(segmentSum(plan) == 400);
    TEST_CHECK((widthsOf(plan) == Widths{353, 39, 8}));
  }

  void testForceMinTwelveTakesWidthInProportion() {
    const std::vector<double> shares{0.6, 0.3, 0.09, 0.01};
    const auto plain = widthsOf(ladderSegments(shares, 4, 400, {}));
    const auto plan = ladderSegments(shares, 4, 400, forced(3, 0.01, 12));
    TEST_CHECK(plan.segs[3].w == 12);
    TEST_CHECK(segmentSum(plan) == 400);
    const auto ratio = [&](std::size_t i) { return plan.segs[i].w / plain[i]; };
    TEST_NEAR(ratio(0), ratio(1), 0.03);
    TEST_NEAR(ratio(1), ratio(2), 0.06);
    TEST_CHECK((widthsOf(plan) == Widths{235, 118, 35, 12}));
  }

  void testForcedTailEntryGetsForceMinTwelve() {
    const auto plan = ladderSegments(powerShares(3000, 64), 3000, 420, forced(2900, 1e-4, 12));
    TEST_CHECK(plan.segs.back().index == 2900);
    TEST_CHECK(plan.segs.back().w == 12);
  }

  void testLiftTakesRoundingFromTheWidestOther() {
    const std::vector<double> shares{0.01, 0.2, 0.79};
    const auto plan = ladderSegments(shares, 3, 400, forced(0, 0.01, 12));
    TEST_CHECK((widthsOf(plan) == Widths{12, 78, 310}));
    TEST_CHECK(plan.segs[2].x == 90 && !plan.rest.has_value());
    // Ten entries held at their 3 px minimum hand their share of the lift to the widest one.
    std::vector<double> crowded{0.01};
    crowded.resize(11, 0.0075);
    crowded.push_back(0.915);
    const auto lifted = ladderSegments(crowded, 12, 400, forced(0, 0.01, 12));
    Widths expected{12};
    expected.resize(11, 3);
    expected.push_back(358);
    TEST_CHECK(widthsOf(lifted) == expected);
  }

  void testForceMinThatIsNotPositiveReadsAsEight() {
    const std::vector<double> shares{0.6, 0.4};
    for (const double forceMin : {0.0, -4.0, kNan}) {
      const auto plan = ladderSegments(shares, 3, 300, forced(2, 0.1, forceMin));
      TEST_CHECK(plan.segs[2].w == 8);
    }
  }

  void testNoEntriesOrNoWidthGiveNothing() {
    const auto none = ladderSegments({}, 0, 300, {});
    TEST_CHECK(none.segs.empty() && !none.rest.has_value());
    const std::vector<double> one{1};
    for (const double width : {0.0, -10.0, kNan}) {
      const auto plan = ladderSegments(one, 1, width, {});
      TEST_CHECK(plan.segs.empty() && !plan.rest.has_value());
    }
  }

  void testSegmentsDegenerateInputs() {
    // Shares that are not finite or negative read as 0; with nothing left the shown entries split evenly.
    const std::vector<double> bad{kNan, -1, kInfinity};
    const auto even = ladderSegments(bad, 3, 300, {});
    TEST_CHECK((widthsOf(even) == Widths{100, 100, 100}));
    // A forced index past the list is ignored.
    const std::vector<double> shares{0.5, 0.3, 0.2};
    const auto ignored = ladderSegments(shares, 3, 400, forced(7, 0.5));
    TEST_CHECK((widthsOf(ignored) == Widths{200, 120, 80}));
    // More entries than shares: the ones without a share stay in the remainder.
    const auto partial = ladderSegments(std::vector<double>{0.4}, 5, 100, {});
    TEST_CHECK(partial.segs.size() == 1 && partial.rest.has_value() && partial.rest->count == 4);
    TEST_CHECK(partial.segs[0].w + partial.rest->w == 100);
    // A list with no shares at all is one remainder.
    const auto unshared = ladderSegments({}, 4, 100, {});
    TEST_CHECK(unshared.segs.empty() && unshared.rest.has_value() && unshared.rest->w == 100);
    // An infinite width is not a layout.
    TEST_CHECK(ladderSegments(shares, 3, kInfinity, {}).segs.empty());
    // A minimum or remainder cap that is not a number reads as 3 px and 15%.
    LadderSegmentOptions unset;
    unset.min = kNan;
    unset.restCap = kNan;
    const auto byDefault = ladderSegments(powerShares(5000, 64), 5000, 420, {});
    const auto sanitized = ladderSegments(powerShares(5000, 64), 5000, 420, unset);
    TEST_CHECK(widthsOf(sanitized) == widthsOf(byDefault));
    TEST_CHECK(sanitized.rest.has_value() && sanitized.rest->w == byDefault.rest->w);
  }

  void testLadderFiveLevels() {
    const auto bars = ladderBars(5);
    TEST_CHECK((heightsOf(bars) == Heights{26, 18, 12, 12, 8}));
    TEST_CHECK(std::ranges::none_of(bars, &LadderBar::merged));
    TEST_CHECK(ladderBars(0).empty());
  }

  void testLadderMergesFromTheNinthLevel() {
    const auto bars = ladderBars(12);
    TEST_CHECK(bars.size() == 9);
    TEST_CHECK((heightsOf(bars) == Heights{26, 18, 12, 12, 8, 5, 4, 3, 3}));
    const auto& merged = bars[8];
    TEST_CHECK(merged.merged && merged.h == 3 && merged.level == 11);
    TEST_CHECK(merged.firstLevel == 8);
    TEST_CHECK(ladderBars(8).size() == 8);
    TEST_CHECK(ladderHeight(bars) < 125);
    TEST_CHECK(ladderHeight(bars) == 91 + 8 * 3);
    TEST_CHECK(ladderHeight({}) == 0);
  }

  void testPlanWithoutAncestorsIsEmpty() { TEST_CHECK(ladderPlan(0, 200).empty()); }

  void testPlanFitsAtEveryHeight() {
    for (const std::size_t count : {1U, 3U, 6U, 11U}) {
      for (int height = 76; height <= 420; ++height) {
        const auto bars = ladderPlan(count, height);
        TEST_CHECK(ladderPlanHeight(bars) <= height);
        for (std::size_t i = 0; i < std::min<std::size_t>(count, 4); ++i) {
          TEST_CHECK(bars[i].h >= 12);
        }
        TEST_CHECK(std::ranges::all_of(bars, [](const LadderBar& bar) { return bar.h >= 3; }));
      }
    }
  }

  void testPlanCoversEveryLevelOnce() {
    for (const std::size_t count : {1U, 3U, 6U, 11U}) {
      for (const double height : {76.0, 90.0, 127.0, 200.0, 320.0}) {
        const auto bars = ladderPlan(count, height);
        std::size_t next = 0;
        for (std::size_t i = 0; i < bars.size(); ++i) {
          TEST_CHECK(bars[i].firstLevel == next);
          TEST_CHECK(bars[i].level >= bars[i].firstLevel);
          TEST_CHECK(bars[i].merged || (bars[i].level == i && bars[i].firstLevel == i));
          TEST_CHECK(!bars[i].merged || i + 1 == bars.size());
          next = bars[i].level + 1;
        }
        TEST_CHECK(next == count);
      }
    }
  }

  void testShortHostMergesSooner() {
    const auto shortPlan = ladderPlan(11, 76);
    const auto tallPlan = ladderPlan(11, 200);
    TEST_CHECK(shortPlan.size() == 5);
    TEST_CHECK(shortPlan[4].merged && shortPlan[4].h == 3);
    TEST_CHECK(tallPlan.size() == 9);
    const auto tiny = ladderPlan(11, 40);
    TEST_CHECK(std::ranges::count(tiny, false, &LadderBar::merged) == 4);
  }

  void testNaturalHeightMatchesLadder() {
    for (const std::size_t count : {1U, 3U, 6U, 11U}) {
      const auto natural = ladderPlanHeight(ladderBars(count));
      if (natural < ladderLimits(count).minH) {
        continue;
      }
      TEST_CHECK(heightsOf(ladderPlan(count, natural)) == heightsOf(ladderBars(count)));
    }
  }

  void testTallerHostThickensToTheLimit() {
    const auto limits = ladderLimits(11);
    const auto at = [](double height) { return heightsOf(ladderPlan(11, height)); };
    const auto pref = at(limits.prefH);
    const auto taller = at(limits.prefH + 40);
    for (std::size_t i = 0; i < pref.size(); ++i) {
      TEST_CHECK(taller[i] >= pref[i]);
    }
    TEST_CHECK((at(limits.maxH) == Heights{40, 28, 20, 20, 14, 10, 6, 4, 3}));
    TEST_CHECK(at(limits.maxH + 100) == at(limits.maxH));
    TEST_CHECK(ladderPlanHeight(ladderPlan(11, limits.maxH + 100)) <= limits.maxH);
  }

  void testNearestFourNeverShrinkAtAnyHeight() {
    for (const std::size_t count : {1U, 3U, 6U, 11U, 17U}) {
      const auto minH = ladderLimits(count).minH;
      auto previous = ladderPlan(count, minH);
      for (int height = static_cast<int>(minH) + 1; height <= 420; ++height) {
        const auto current = ladderPlan(count, height);
        for (std::size_t i = 0; i < std::min(count, kLadderNearLevels); ++i) {
          TEST_CHECK(current[i].h >= previous[i].h);
          TEST_CHECK(current[i].h >= 12);
        }
        TEST_CHECK(ladderPlanHeight(current) <= height);
        previous = current;
      }
    }
  }

  void testNanHeightReadsAsZero() {
    for (const std::size_t count : {1U, 5U, 11U}) {
      TEST_CHECK(heightsOf(ladderPlan(count, kNan)) == heightsOf(ladderPlan(count, 0)));
      TEST_CHECK(heightsOf(ladderPlan(count, -50)) == heightsOf(ladderPlan(count, 0)));
    }
    TEST_CHECK(std::ranges::count(ladderPlan(11, kNan), false, &LadderBar::merged) == 4);
  }

  void testPlanExactHeights() {
    TEST_CHECK((heightsOf(ladderPlan(1, 40)) == Heights{28}));
    TEST_CHECK((heightsOf(ladderPlan(3, 76)) == Heights{27, 19, 12}));
    TEST_CHECK((heightsOf(ladderPlan(3, 90)) == Heights{32, 23, 17}));
    TEST_CHECK((heightsOf(ladderPlan(6, 90)) == Heights{14, 14, 12, 12, 6, 5}));
    TEST_CHECK((heightsOf(ladderPlan(6, 127)) == Heights{30, 21, 15, 15, 11, 8}));
    TEST_CHECK((heightsOf(ladderPlan(11, 90)) == Heights{12, 12, 12, 12, 5, 4, 3}));
    const auto merged = ladderPlan(11, 90).back();
    TEST_CHECK(merged.merged && merged.firstLevel == 6 && merged.level == 10);
    TEST_CHECK((heightsOf(ladderPlan(17, 127)) == Heights{26, 18, 12, 12, 8, 5, 4, 3, 3}));
    // Whole pixels: a fractional height plans like the whole pixels below it.
    TEST_CHECK(heightsOf(ladderPlan(3, 90.5)) == heightsOf(ladderPlan(3, 90)));
    TEST_CHECK(ladderPlanHeight(ladderPlan(1, 40.5)) <= 40.5);
    TEST_CHECK((heightsOf(ladderPlan(11, kInfinity)) == Heights{40, 28, 20, 20, 14, 10, 6, 4, 3}));
  }

  void testLimits() {
    std::vector<LadderLimits> depths;
    for (const std::size_t count : {0U, 1U, 3U, 6U, 11U}) {
      depths.push_back(ladderLimits(count));
    }
    for (const auto& limits : depths) {
      TEST_CHECK(limits.minW == 360 && limits.minH == 76);
      TEST_CHECK(limits.minH <= limits.prefH && limits.prefH <= limits.maxH);
    }
    TEST_CHECK(depths[4].prefH > depths[1].prefH);
    TEST_CHECK(depths[0].prefH == 76);
    TEST_CHECK(depths[0].maxH == 76 && depths[1].maxH == 76);
    TEST_CHECK(depths[2].maxH == 106 && depths[3].prefH == 108 && depths[3].maxH == 159);
    TEST_CHECK(depths[4].prefH == 127 && depths[4].maxH == 181);
    TEST_CHECK(ladderLimits(17).prefH == 127 && ladderLimits(17).maxH == 181);
  }

  // r has p and q; p has c and d; c is the current folder. Every entry weighs the same.
  Tree pathTree(bool hiddenCurrent) {
    auto tree = workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true},
            {.parent = 0, .name = "p", .folder = true},
            {.parent = 0, .name = "q", .folder = true},
            {.parent = 1, .name = hiddenCurrent ? ".c" : "c", .folder = true},
            {.parent = 1, .name = "d", .folder = true},
        },
        0
    );
    tree.root = 3;
    tree.ancestors = parentChain(tree, 3);
    return tree;
  }

  void testSceneGeometry() {
    const auto tree = pathTree(false);
    LayoutSettings settings;
    settings.sizeBy = WorkbenchMeasure::Equal;
    const auto scene = ladderScene(tree, settings, 544, 100);
    TEST_CHECK(!scene.atTop);
    TEST_CHECK(scene.rows.size() == 2);
    const auto& nearest = scene.rows[0];
    TEST_CHECK(nearest.folder == 1 && nearest.onPath == 3 && nearest.index == 0);
    TEST_CHECK(nearest.h == 40 && nearest.y == 54 && nearest.x == 132 && nearest.w == 400);
    TEST_CHECK(nearest.named && nearest.fade == 1 && !nearest.merged);
    TEST_CHECK(nearest.pieces.size() == 2 && !nearest.rest.has_value());
    TEST_CHECK(nearest.pieces[0].node == 3 && nearest.pieces[0].onPath && nearest.pieces[0].labeled);
    TEST_CHECK(nearest.pieces[0].x == 132 && nearest.pieces[0].w == 200);
    TEST_CHECK(nearest.pieces[1].node == 4 && !nearest.pieces[1].onPath && nearest.pieces[1].x == 332);
    const auto& far = scene.rows[1];
    TEST_CHECK(far.folder == 0 && far.onPath == 1 && far.index == 1);
    TEST_CHECK(far.h == 28 && far.y == 23 && far.fade == 0.92 && far.named);
    TEST_CHECK(far.pieces[0].node == 1 && far.pieces[0].onPath);
  }

  void testSceneLabelsOnlyWideSegmentsOfTheTwoNearestBars() {
    auto tree = pathTree(false);
    tree.nodes[3].bytes = 899;
    tree.nodes[4].bytes = 99;
    LayoutSettings settings;
    settings.emphasis = 1;
    const auto scene = ladderScene(tree, settings, 544, 100);
    const auto& nearest = scene.rows[0];
    TEST_CHECK(nearest.pieces[0].w == 360 && nearest.pieces[0].labeled);
    TEST_CHECK(nearest.pieces[1].w == 40 && !nearest.pieces[1].labeled);
  }

  void testSceneHiddenOnPathEntryForcedAfterTheOthers() {
    const auto tree = pathTree(true);
    LayoutSettings settings;
    settings.sizeBy = WorkbenchMeasure::Equal;
    const auto scene = ladderScene(tree, settings, 544, 100);
    const auto& nearest = scene.rows[0];
    TEST_CHECK(nearest.pieces.size() == 2);
    TEST_CHECK(nearest.pieces[0].node == 4 && nearest.pieces[0].w == 388);
    TEST_CHECK(nearest.pieces[1].node == 3 && nearest.pieces[1].onPath && nearest.pieces[1].w == 12);
    TEST_CHECK(nearest.pieces[1].x == 520 && !nearest.pieces[1].labeled);
  }

  // A host that is not finite reads as 0 px tall, for the bar heights and their positions alike.
  void testSceneInfiniteHeightReadsAsZero() {
    const auto tree = pathTree(false);
    for (const double height : {kInfinity, -kInfinity, kNan, 0.0}) {
      const auto scene = ladderScene(tree, {}, 544, height);
      TEST_CHECK(scene.rows.size() == 2);
      TEST_CHECK(scene.rows[0].h == 12 && scene.rows[0].y == -18);
      TEST_CHECK(scene.rows[1].h == 12 && scene.rows[1].y == -33);
    }
  }

  // A hidden on-path entry heavier than all visible ones together counts as half the list.
  void testSceneHeavyHiddenOnPathShareCappedAtHalf() {
    std::vector<std::string> names;
    names.reserve(200);
    for (int i = 0; i < 200; ++i) {
      names.push_back(std::format("e{:03}", i));
    }
    std::vector<luciole::test::WorkbenchRow> rows{
        {.parent = -1, .name = "p", .folder = true},
        {.parent = 0, .name = ".c", .folder = true, .bytes = 999999},
    };
    for (const auto& name : names) {
      rows.push_back({.parent = 0, .name = name});
    }
    auto tree = workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = {0};
    const auto scene = ladderScene(tree, {}, 544, 100);
    const auto& row = scene.rows[0];
    TEST_CHECK(row.pieces.size() == 41);
    const auto& onPath = row.pieces.back();
    TEST_CHECK(onPath.node == 1 && onPath.onPath && onPath.x == 460 && onPath.w == 12);
    TEST_CHECK(row.rest.has_value() && row.rest->x == 472 && row.rest->w == 60 && row.rest->count == 160);
  }

  // A chain l0 > l1 > ... > l10 with a sibling beside each level; the current folder is l10.
  Tree chainTree() {
    auto tree = workbenchTree(
        {
            {.parent = -1, .name = "l0", .folder = true},
            {.parent = 0, .name = "l1", .folder = true},
            {.parent = 1, .name = "l2", .folder = true},
            {.parent = 2, .name = "l3", .folder = true},
            {.parent = 3, .name = "l4", .folder = true},
            {.parent = 4, .name = "l5", .folder = true},
            {.parent = 5, .name = "l6", .folder = true},
            {.parent = 6, .name = "l7", .folder = true},
            {.parent = 7, .name = "l8", .folder = true},
            {.parent = 8, .name = "l9", .folder = true},
            {.parent = 9, .name = "l10", .folder = true},
            {.parent = 0, .name = "s0", .folder = true},
            {.parent = 9, .name = "s9", .folder = true},
        },
        0
    );
    tree.root = 10;
    tree.ancestors = parentChain(tree, 10);
    return tree;
  }

  void testSceneMergedFarLevels() {
    const auto tree = chainTree();
    const auto scene = ladderScene(tree, {}, 600, 76);
    TEST_CHECK(scene.rows.size() == 5);
    const auto& merged = scene.rows[4];
    TEST_CHECK(merged.merged && merged.firstLevel == 4 && merged.level == 9);
    TEST_CHECK(merged.folder == 0 && merged.onPath == 1);
    TEST_CHECK(!merged.named && merged.fade == 0.62 && merged.h == 3);
    TEST_CHECK(std::ranges::any_of(merged.pieces, [](const LadderPiece& piece) { return piece.onPath; }));
    TEST_CHECK(scene.rows[2].named && !scene.rows[3].named);
    TEST_CHECK(std::ranges::none_of(scene.rows[2].pieces, &LadderPiece::labeled));
    // Bars stack upward from the bottom padding with a 3 px gap under each.
    TEST_CHECK(scene.rows[0].y == 76 - 6 - 12);
    for (std::size_t i = 1; i < scene.rows.size(); ++i) {
      TEST_CHECK(scene.rows[i].y == scene.rows[i - 1].y - 3 - scene.rows[i].h);
    }
    TEST_CHECK(scene.rows[4].y == 7);
    // The nearest bar's on-path entry stays at least 12 px wide.
    const auto& nearest = scene.rows[0];
    const auto onPath = std::ranges::find_if(nearest.pieces, &LadderPiece::onPath);
    TEST_CHECK(onPath != nearest.pieces.end() && onPath->node == 10 && onPath->w >= 12);
  }

  void testSceneRemainderAndDegenerateHosts() {
    std::vector<luciole::test::WorkbenchRow> rows{{.parent = -1, .name = "r", .folder = true}};
    static constexpr std::array<std::string_view, 60> kNames{
        "e00", "e01", "e02", "e03", "e04", "e05", "e06", "e07", "e08", "e09", "e10", "e11", "e12", "e13", "e14",
        "e15", "e16", "e17", "e18", "e19", "e20", "e21", "e22", "e23", "e24", "e25", "e26", "e27", "e28", "e29",
        "e30", "e31", "e32", "e33", "e34", "e35", "e36", "e37", "e38", "e39", "e40", "e41", "e42", "e43", "e44",
        "e45", "e46", "e47", "e48", "e49", "e50", "e51", "e52", "e53", "e54", "e55", "e56", "e57", "e58", "e59",
    };
    for (const auto name : kNames) {
      rows.push_back({.parent = 0, .name = name, .bytes = 100});
    }
    auto tree = workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = {0};
    const auto scene = ladderScene(tree, {}, 544, 100);
    const auto& row = scene.rows[0];
    TEST_CHECK(row.rest.has_value() && row.rest->count == 60 - row.pieces.size());
    TEST_CHECK(row.rest->x + row.rest->w == row.x + row.w);
    TEST_CHECK(row.restLabeled == (row.rest->w >= 30));
    // A host too narrow for any bar still gives the bar 40 px.
    TEST_CHECK(ladderScene(tree, {}, 10, 100).rows[0].w == 40);
    TEST_CHECK(ladderScene(tree, {}, kNan, kNan).rows[0].w == 40);
    // The top of the tree has nothing above.
    tree.root = 0;
    tree.ancestors.clear();
    const auto top = ladderScene(tree, {}, 544, 100);
    TEST_CHECK(top.atTop && top.rows.empty());
    // An ancestor list naming nodes that do not exist gives bars without pieces.
    tree.root = 1;
    tree.ancestors = {500};
    const auto unknown = ladderScene(tree, {}, 544, 100);
    TEST_CHECK(unknown.rows.size() == 1 && unknown.rows[0].pieces.empty());
  }
} // namespace

int main() {
  testSegmentsTileTheBar();
  testRemainderCappedAndCounted();
  testShortListFollowsWeights();
  testAtMostFortySegments();
  testAtMostSixtyFourSegmentsWhateverMaxCount();
  testForcedTailEntryKeptLast();
  testForcedShownEntryLifted();
  testDominantEntryDrawnDominant();
  testHiddenOnPathEntryTakesItsOwnWidth();
  testForceMinTwelveTakesWidthInProportion();
  testForcedTailEntryGetsForceMinTwelve();
  testLiftTakesRoundingFromTheWidestOther();
  testForceMinThatIsNotPositiveReadsAsEight();
  testNoEntriesOrNoWidthGiveNothing();
  testSegmentsDegenerateInputs();
  testLadderFiveLevels();
  testLadderMergesFromTheNinthLevel();
  testPlanWithoutAncestorsIsEmpty();
  testPlanFitsAtEveryHeight();
  testPlanCoversEveryLevelOnce();
  testShortHostMergesSooner();
  testNaturalHeightMatchesLadder();
  testTallerHostThickensToTheLimit();
  testNearestFourNeverShrinkAtAnyHeight();
  testNanHeightReadsAsZero();
  testPlanExactHeights();
  testLimits();
  testSceneGeometry();
  testSceneLabelsOnlyWideSegmentsOfTheTwoNearestBars();
  testSceneHiddenOnPathEntryForcedAfterTheOthers();
  testSceneInfiniteHeightReadsAsZero();
  testSceneHeavyHiddenOnPathShareCappedAtHalf();
  testSceneMergedFarLevels();
  testSceneRemainderAndDegenerateHosts();
  std::println("workbench ladder plan checks passed");
  return 0;
}
