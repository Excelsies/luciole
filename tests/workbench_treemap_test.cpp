#include "test_check.h"
#include "workbench/treemap.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <numbers>
#include <print>
#include <vector>

using namespace luciole::workbench;

namespace {
  constexpr double kEps = 1e-6;
  constexpr TreemapRect kRect{.x = 8, .y = 8, .w = 800, .h = 420};
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();

  bool overlap(const TreemapRect& a, const TreemapRect& b) {
    return std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x) > kEps
        && std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y) > kEps;
  }

  // Deterministic weights, heaviest first.
  std::vector<double> weightsOf(std::size_t count, double seed) {
    double state = seed;
    std::vector<double> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      state = std::fmod(state * 1103515245.0 + 12345.0, 2147483648.0);
      out.push_back(1.0 + std::pow(std::fmod(state, 1000.0), 1.5));
    }
    std::ranges::sort(out, std::greater{});
    return out;
  }

  std::vector<BandItem> itemsOf(const std::vector<double>& weights) {
    double total = 0;
    for (const auto value : weights) {
      total += value;
    }
    std::vector<BandItem> items;
    items.reserve(weights.size());
    for (std::size_t i = 0; i < weights.size(); ++i) {
      items.push_back({.key = static_cast<NodeId>(i), .share = weights[i] / total});
    }
    return items;
  }

  PartitionOptions minimum(double minLength) {
    PartitionOptions options;
    options.minLength = minLength;
    return options;
  }

  bool covers(const std::vector<BandSegment>& segments, double length) {
    if (segments.empty() || std::abs(segments.front().s0) >= kEps || std::abs(segments.back().s1 - length) >= kEps) {
      return false;
    }
    for (std::size_t i = 1; i < segments.size(); ++i) {
      if (std::abs(segments[i].s0 - segments[i - 1].s1) >= kEps) {
        return false;
      }
    }
    return true;
  }

  void squarifyAreasFollowWeights() {
    const std::vector<std::pair<std::size_t, TreemapRect>> cases{
        {1, {.x = 5, .y = 7, .w = 300, .h = 200}},
        {7, {.x = 0, .y = 0, .w = 900, .h = 300}},
        {200, {.x = 10, .y = 10, .w = 1000, .h = 400}},
    };
    for (const auto& [count, rect] : cases) {
      const auto weights = weightsOf(count, static_cast<double>(count));
      double total = 0;
      for (const auto value : weights) {
        total += value;
      }
      const auto rects = squarify(weights, rect);
      TEST_CHECK(rects.size() == count);
      for (std::size_t i = 0; i < rects.size(); ++i) {
        TEST_NEAR((rects[i].w * rects[i].h) / (rect.w * rect.h), weights[i] / total, 1e-9);
      }
    }
  }

  void squarifyTilesStayInsideWithoutOverlap() {
    const TreemapRect rect{.x = 20, .y = 30, .w = 640, .h = 360};
    const auto rects = squarify(weightsOf(120, 3), rect);
    TEST_CHECK(rects.size() == 120);
    double area = 0;
    for (const auto& r : rects) {
      TEST_CHECK(r.w > 0 && r.h > 0);
      TEST_CHECK(r.x >= rect.x - kEps && r.y >= rect.y - kEps);
      TEST_CHECK(r.x + r.w <= rect.x + rect.w + kEps && r.y + r.h <= rect.y + rect.h + kEps);
      area += r.w * r.h;
    }
    for (std::size_t i = 0; i < rects.size(); ++i) {
      for (std::size_t j = i + 1; j < rects.size(); ++j) {
        TEST_CHECK(!overlap(rects[i], rects[j]));
      }
    }
    TEST_NEAR(area, rect.w * rect.h, 1e-4);
  }

  void squarifyKeepsTilesSquareAndRejectsBadInput() {
    const auto rects = squarify(std::vector<double>{1, 1, 1, 1}, {.x = 0, .y = 0, .w = 100, .h = 100});
    TEST_CHECK(rects.size() == 4);
    for (const auto& r : rects) {
      TEST_NEAR(r.w, 50.0, kEps);
      TEST_NEAR(r.h, 50.0, kEps);
    }
    TEST_CHECK(squarify(std::vector<double>{1, 0}, {.x = 0, .y = 0, .w = 10, .h = 10}).empty());
    TEST_CHECK(squarify(std::vector<double>{1}, {.x = 0, .y = 0, .w = 0, .h = 10}).empty());
    TEST_CHECK(squarify(std::vector<double>{}, {.x = 0, .y = 0, .w = 10, .h = 10}).empty());
  }

  // The worked example of Bruls, Huizing and van Wijk: weights 6 6 4 3 2 2 1 in 600 by 400.
  void squarifyMatchesTheWorkedExample() {
    const auto rects = squarify(std::vector<double>{6, 6, 4, 3, 2, 2, 1}, {.x = 0, .y = 0, .w = 600, .h = 400});
    const std::vector<TreemapRect> expected{
        {.x = 0, .y = 0, .w = 300, .h = 200},
        {.x = 0, .y = 200, .w = 300, .h = 200},
        {.x = 300, .y = 0, .w = 171.428571, .h = 233.333333},
        {.x = 471.428571, .y = 0, .w = 128.571429, .h = 233.333333},
        {.x = 300, .y = 233.333333, .w = 120, .h = 166.666667},
        {.x = 420, .y = 233.333333, .w = 120, .h = 166.666667},
        {.x = 540, .y = 233.333333, .w = 60, .h = 166.666667},
    };
    TEST_CHECK(rects.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
      TEST_NEAR(rects[i].x, expected[i].x, kEps);
      TEST_NEAR(rects[i].y, expected[i].y, kEps);
      TEST_NEAR(rects[i].w, expected[i].w, kEps);
      TEST_NEAR(rects[i].h, expected[i].h, kEps);
    }
  }

  void bandPerimeterAndRoundTrip() {
    const auto g = band(kRect, 40, 48);
    TEST_CHECK(g.has_value());
    const double expect = 2 * (kRect.w - 40 - 2 * g->cornerRadius)
        + 2 * (kRect.h - 40 - 2 * g->cornerRadius)
        + 2 * std::numbers::pi * g->cornerRadius;
    TEST_NEAR(g->perimeter, expect, 1e-9);
    TEST_NEAR(pointAt(*g, 0).y, kRect.y + 20, 1e-9);
    std::size_t steps = 0;
    for (double s = 0; s < g->perimeter; s += 13.7) {
      const auto p = pointAt(*g, s);
      const auto back = locate(*g, p.x, p.y);
      TEST_CHECK(back.has_value());
      TEST_CHECK(std::abs(*back - s) < kEps || std::abs(*back - s - g->perimeter) < kEps);
      ++steps;
    }
    TEST_CHECK(steps == 163);
  }

  void bandMatchesKnownPoints() {
    const auto g = band(kRect, 40, 48);
    TEST_CHECK(g.has_value());
    TEST_NEAR(g->cornerRadius, 28.0, kEps);
    TEST_NEAR(g->halfStraightX, 352.0, kEps);
    TEST_NEAR(g->halfStraightY, 162.0, kEps);
    TEST_NEAR(g->perimeter, 2231.929189, kEps);
    const std::vector<double> starts{0,           704,         747.982297,  1071.982297,
                                     1115.964594, 1819.964594, 1863.946891, 2187.946891};
    for (std::size_t i = 0; i < starts.size(); ++i) {
      TEST_NEAR(g->starts[i], starts[i], kEps);
    }
    struct Expect {
      double s;
      BandPoint point;
    };
    const std::vector<Expect> points{
        {0, {.x = 56, .y = 28, .normalX = 0, .normalY = -1}},
        {100, {.x = 156, .y = 28, .normalX = 0, .normalY = -1}},
        {709, {.x = 764.973469, .y = 28.445244, .normalX = 0.177624, .normalY = -0.984098}},
        {1081.982297, {.x = 786.233186, .y = 389.788767, .normalX = 0.9369, .normalY = 0.349599}},
        {1822.964594, {.x = 53.005737, .y = 407.839439, .normalX = -0.106938, .normalY = 0.994266}},
        {2207.946891, {.x = 34.844282, .y = 37.657819, .normalX = -0.755561, .normalY = -0.655078}},
        {-50, {.x = 28, .y = 62.017703, .normalX = -1, .normalY = 0}},
        {2241.929189, {.x = 66, .y = 28, .normalX = 0, .normalY = -1}},
    };
    for (const auto& [s, point] : points) {
      const auto p = pointAt(*g, s);
      TEST_NEAR(p.x, point.x, 1e-5);
      TEST_NEAR(p.y, point.y, 1e-5);
      TEST_NEAR(p.normalX, point.normalX, 1e-5);
      TEST_NEAR(p.normalY, point.normalY, 1e-5);
    }
    TEST_CHECK(!locate(*g, 20, 20).has_value());
    TEST_NEAR(locate(*g, 400, 15), 344.0, kEps);
    TEST_CHECK(bandPiece(*g, 703.9) == 0);
    TEST_CHECK(bandPiece(*g, 704) == 1);
    TEST_CHECK(bandPiece(*g, 2200) == 7);
    TEST_CHECK(bandPiece(*g, -5) == 0);
  }

  void locateRejectsPointsOffTheBand() {
    const auto g = band(kRect, 22, 30);
    TEST_CHECK(g.has_value());
    TEST_CHECK(!locate(*g, kRect.x + kRect.w / 2, kRect.y + kRect.h / 2).has_value());
    TEST_CHECK(!locate(*g, kRect.x - 5, kRect.y + 100).has_value());
    TEST_CHECK(locate(*g, kRect.x + kRect.w / 2, kRect.y + 3).has_value());
    TEST_CHECK(!locate(*g, kNan, kRect.y + 3).has_value());
  }

  void partitionCoversThePerimeterOnce() {
    const auto g = band(kRect, 40, 48);
    TEST_CHECK(g.has_value());
    for (const std::size_t count : {1U, 2U, 5U, 40U, 3000U}) {
      const auto items = itemsOf(weightsOf(count, static_cast<double>(count + 1)));
      const auto segments =
          partition(items, g->perimeter, {.minLength = 6, .pinKey = static_cast<NodeId>(count - 1), .pinMinimum = 10});
      TEST_CHECK(covers(segments, g->perimeter));
      std::vector<bool> seen(count, false);
      std::size_t present = 0;
      for (const auto& segment : segments) {
        const std::vector<NodeId> keys = segment.merged ? segment.keys : std::vector<NodeId>{segment.key};
        for (const auto key : keys) {
          TEST_CHECK(key < count && !seen[key]);
          seen[key] = true;
          ++present;
        }
      }
      TEST_CHECK(present == count);
    }
  }

  void partitionMinimumMergedTailAndPin() {
    const auto g = band(kRect, 22, 30);
    TEST_CHECK(g.has_value());
    const auto items = itemsOf(weightsOf(600, 9));
    const auto segments = partition(items, g->perimeter, {.minLength = 6, .pinKey = 599, .pinMinimum = 10});
    TEST_CHECK(!segments.empty() && segments.back().merged);
    TEST_CHECK(std::ranges::any_of(segments, [](const BandSegment& segment) {
      return !segment.merged && segment.key == 599 && segment.s1 - segment.s0 >= 10 - kEps;
    }));
    for (const auto& segment : segments) {
      TEST_CHECK(segment.s1 - segment.s0 >= 6 - kEps);
    }
    TEST_CHECK(partition({}, g->perimeter, minimum(6)).empty());
  }

  void partitionLengthsFollowShares() {
    const std::vector<BandItem> items{{.key = 0, .share = 0.5}, {.key = 1, .share = 0.3}, {.key = 2, .share = 0.2}};
    const auto segments = partition(items, 1000, minimum(6));
    TEST_CHECK(segments.size() == 3);
    TEST_NEAR(std::round(segments[0].s1 - segments[0].s0), 500.0, 0.0);
    TEST_NEAR(std::round(segments[1].s1 - segments[1].s0), 300.0, 0.0);
    TEST_NEAR(std::round(segments[2].s1 - segments[2].s0), 200.0, 0.0);
  }

  void partitionMatchesKnownLengths() {
    // A short pinned entry follows the long ones, before the merged tail.
    const std::vector<BandItem> pinned{
        {.key = 0, .share = 0.5}, {.key = 1, .share = 0.001}, {.key = 2, .share = 0.002}, {.key = 3, .share = 0.497}
    };
    auto segments = partition(pinned, 1000, {.minLength = 6, .pinKey = 2, .pinMinimum = 10});
    TEST_CHECK(segments.size() == 4);
    TEST_CHECK(segments[0].key == 0 && segments[1].key == 3 && segments[2].key == 2);
    TEST_CHECK(segments[3].merged && segments[3].keys == std::vector<NodeId>{1});
    TEST_NEAR(segments[0].s1, 493.480441, kEps);
    TEST_NEAR(segments[1].s1, 984.0, kEps);
    TEST_NEAR(segments[2].s1, 994.0, kEps);
    TEST_NEAR(segments[3].s1, 1000.0, kEps);
    // Two short entries merge into one tail of at least the minimum.
    const std::vector<BandItem> tail{
        {.key = 0, .share = 0.9}, {.key = 1, .share = 0.001}, {.key = 2, .share = 0.002}, {.key = 3, .share = 0.097}
    };
    segments = partition(tail, 1000, minimum(6));
    TEST_CHECK(segments.size() == 3);
    TEST_CHECK(segments[2].merged && (segments[2].keys == std::vector<NodeId>{1, 2}));
    TEST_NEAR(segments[0].s1, 897.291876, kEps);
    TEST_NEAR(segments[1].s1, 994.0, kEps);
    // The merged tail takes its raw length when that is above the minimum.
    const std::vector<BandItem> raw{{.key = 0, .share = 0.99}, {.key = 1, .share = 0.004}, {.key = 2, .share = 0.006}};
    segments = partition(raw, 300, minimum(6));
    TEST_CHECK(segments.size() == 2);
    TEST_NEAR(segments[0].s1, 294.0, kEps);
    TEST_CHECK(segments[1].merged && segments[1].keys.size() == 2);
    // A single short entry keeps its own segment at the minimum instead of merging.
    const std::vector<BandItem> single{{.key = 0, .share = 0.995}, {.key = 1, .share = 0.005}};
    segments = partition(single, 300, minimum(6));
    TEST_CHECK(segments.size() == 2 && !segments[1].merged && segments[1].key == 1);
    TEST_NEAR(segments[1].s0, 294.0, kEps);
  }

  void segmentPolygonStaysInsideTheBand() {
    const auto g = band(kRect, 40, 48);
    TEST_CHECK(g.has_value());
    const auto poly = segmentPolygon(*g, g->starts[1] - 20, g->starts[2] + 30);
    for (const auto& [x, y] : poly) {
      TEST_CHECK(x >= kRect.x - kEps && y >= kRect.y - kEps);
      TEST_CHECK(x <= kRect.x + kRect.w + kEps && y <= kRect.y + kRect.h + kEps);
    }
    TEST_CHECK(poly.size() > 8);
    TEST_CHECK(poly.size() == 22);
    TEST_NEAR(poly[0].x, 740.0, kEps);
    TEST_NEAR(poly[0].y, 8.0, kEps);
    TEST_NEAR(poly[1].x, 760.0, kEps);
    TEST_NEAR(poly[2].x, 769.536128, kEps);
    TEST_NEAR(poly[2].y, 8.956804, kEps);
  }

  BandSegment stretch(double s0, double s1) {
    BandSegment segment;
    segment.s0 = s0;
    segment.s1 = s1;
    return segment;
  }

  std::vector<BandSegment> twoSegments(double s0, double boundary, double end) {
    return {stretch(s0, boundary), stretch(boundary, end)};
  }

  void snapMovesArcBoundariesToTheNearerEnd() {
    const auto g = band(kRect, 40, 48);
    TEST_CHECK(g.has_value());
    const double arc = g->lengths[1];
    auto segments = snapToStraights(*g, twoSegments(0, g->starts[1] + arc * 0.3, g->perimeter), 6);
    TEST_NEAR(segments[0].s1, g->starts[1], kEps);
    TEST_NEAR(segments[1].s0, g->starts[1], kEps);
    segments = snapToStraights(*g, twoSegments(0, g->starts[1] + arc * 0.8, g->perimeter), 6);
    TEST_NEAR(segments[0].s1, g->starts[2], kEps);
    TEST_NEAR(segments[1].s0, g->starts[2], kEps);
    segments = snapToStraights(*g, twoSegments(0, g->starts[2] + 50, g->perimeter), 6);
    TEST_CHECK(segments[0].s1 == g->starts[2] + 50);
  }

  void snapFallsBackToTheOtherEndOrStays() {
    const auto g = band(kRect, 40, 48);
    TEST_CHECK(g.has_value());
    const double arc = g->lengths[1];
    const double s0 = g->starts[1] - 3;
    const double boundary = g->starts[1] + arc * 0.3;
    auto segments = snapToStraights(*g, twoSegments(s0, boundary, g->perimeter), 6);
    TEST_NEAR(segments[0].s1, g->starts[2], kEps);
    segments = snapToStraights(*g, twoSegments(s0, boundary, g->starts[1] + arc + 2), 6);
    TEST_CHECK(segments[0].s1 == boundary);
  }

  void snapKeepsCoverageOnARealPartition() {
    const auto g = band(kRect, 40, 48);
    TEST_CHECK(g.has_value());
    const auto segments = snapToStraights(*g, partition(itemsOf(weightsOf(30, 5)), g->perimeter, minimum(6)), 6);
    TEST_CHECK(covers(segments, g->perimeter));
  }

  void degenerateInputGivesADefinedResult() {
    const std::vector<BandItem> one{{.key = 0, .share = 1}};
    TEST_CHECK(partition(one, 0, minimum(6)).empty());
    TEST_CHECK(partition(one, kNan, minimum(6)).empty());
    TEST_CHECK(partition(one, -10, minimum(6)).empty());
    TEST_CHECK(partition(one, kInf, minimum(6)).empty());
    const std::vector<BandItem> nanShare{{.key = 0, .share = kNan}, {.key = 1, .share = 1}};
    const auto nan = partition(nanShare, 100, minimum(6));
    TEST_CHECK(!nan.empty());
    for (const auto& segment : nan) {
      TEST_CHECK(std::isfinite(segment.s0) && std::isfinite(segment.s1) && segment.s1 >= segment.s0);
    }
    TEST_NEAR(nan.front().s0, 0.0, 1e-9);
    TEST_NEAR(nan.back().s1, 100.0, 1e-9);
    const auto g = band({.x = 0, .y = 0, .w = 40, .h = 30}, 16, 20);
    TEST_CHECK(g.has_value());
    TEST_CHECK(segmentPolygon(*g, kNan, 10).empty());
    TEST_CHECK(segmentPolygon(*g, 10, 5).empty());
    const auto whole = segmentPolygon(*g, 0, g->perimeter);
    TEST_CHECK(!whole.empty() && whole.size() < 400);
    const std::vector<std::pair<double, double>> bad{{0, 10}, {10, 0}, {kNan, 10}, {-3, 10}, {kInf, 10}};
    for (const auto& [w, h] : bad) {
      TEST_CHECK(!band({.x = 0, .y = 0, .w = w, .h = h}, 4, 4).has_value());
    }
    TEST_CHECK(!band({.x = 0, .y = 0, .w = 10, .h = 10}, 0, 4).has_value());
    TEST_CHECK(!band({.x = 0, .y = 0, .w = 10, .h = 10}, 4, kNan).has_value());
    TEST_CHECK(squarify(std::vector<double>{1}, {.x = 0, .y = 0, .w = kNan, .h = 10}).empty());
    TEST_CHECK(squarify(std::vector<double>{1, kInf}, {.x = 0, .y = 0, .w = 10, .h = 10}).empty());
    TEST_CHECK(squarify(std::vector<double>{1e308, 1e308}, {.x = 0, .y = 0, .w = 10, .h = 10}).empty());
    // The area or the scale overflows.
    TEST_CHECK(squarify(std::vector<double>{1, 1}, {.x = 0, .y = 0, .w = 1e200, .h = 1e200}).empty());
    const double denorm = std::numeric_limits<double>::denorm_min();
    TEST_CHECK(squarify(std::vector<double>{denorm, denorm}, {.x = 0, .y = 0, .w = 100, .h = 100}).empty());
    const auto tiny = band({.x = 0, .y = 0, .w = 1, .h = 1}, 1, 1);
    TEST_CHECK(tiny.has_value() && tiny->perimeter == 0);
    const auto center = pointAt(*tiny, 3);
    TEST_CHECK(std::isfinite(center.x) && std::isfinite(center.y));
    // A band with no perimeter holds no point.
    TEST_CHECK(!locate(*tiny, 0.5, 0.5).has_value());
    TEST_CHECK(!locate(*tiny, 0.5, 0).has_value());
    const auto thick = band({.x = 0, .y = 0, .w = 10, .h = 10}, 20, 5);
    TEST_CHECK(thick.has_value() && thick->perimeter == 0);
    TEST_CHECK(!locate(*thick, 5, 0).has_value());
    TEST_CHECK(segmentPolygon(*tiny, 0, 1).empty());
    std::vector<double> many(5000);
    for (std::size_t i = 0; i < many.size(); ++i) {
      many[i] = 1.0 + static_cast<double>(i % 7);
    }
    std::ranges::sort(many, std::greater{});
    TEST_CHECK(squarify(many, {.x = 0, .y = 0, .w = 3, .h = 3}).size() == 5000);
    // A NaN minimum holds nothing back: the one entry covers the whole length.
    const auto nanMinimum = partition(one, 50, minimum(kNan));
    TEST_CHECK(nanMinimum.size() == 1 && !nanMinimum[0].merged && nanMinimum[0].key == 0);
    TEST_NEAR(nanMinimum[0].s0, 0.0, 0.0);
    TEST_NEAR(nanMinimum[0].s1, 50.0, 0.0);
    const auto snapped = snapToStraights(*g, {}, 6);
    TEST_CHECK(snapped.empty());
  }

  void insetMovesEverySideIn() {
    TEST_CHECK((inset({.x = 10, .y = 20, .w = 100, .h = 50}, 3) == TreemapRect{.x = 13, .y = 23, .w = 94, .h = 44}));
    TEST_CHECK((inset({.x = 0, .y = 0, .w = 4, .h = 4}, -1) == TreemapRect{.x = -1, .y = -1, .w = 6, .h = 6}));
  }
} // namespace

int main() {
  squarifyAreasFollowWeights();
  squarifyTilesStayInsideWithoutOverlap();
  squarifyKeepsTilesSquareAndRejectsBadInput();
  squarifyMatchesTheWorkedExample();
  bandPerimeterAndRoundTrip();
  bandMatchesKnownPoints();
  locateRejectsPointsOffTheBand();
  partitionCoversThePerimeterOnce();
  partitionMinimumMergedTailAndPin();
  partitionLengthsFollowShares();
  partitionMatchesKnownLengths();
  segmentPolygonStaysInsideTheBand();
  snapMovesArcBoundariesToTheNearerEnd();
  snapFallsBackToTheOtherEndOrStays();
  snapKeepsCoverageOnARealPartition();
  degenerateInputGivesADefinedResult();
  insetMovesEverySideIn();
  std::println("workbench treemap checks passed");
  return 0;
}
