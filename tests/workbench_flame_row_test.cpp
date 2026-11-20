#include "test_check.h"
#include "workbench/flame_row.h"
#include "workbench_flame_fixture.h"

#include <algorithm>
#include <cstddef>
#include <print>
#include <span>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::flame;

  FlameRowOptions rowOptions(
      double min, double cap = 0.15, std::size_t pin = kNoFlameItem, double pinMin = 0,
      std::span<const double> want = {}
  ) {
    FlameRowOptions options;
    options.min = min;
    options.cap = cap;
    options.pin = pin;
    options.pinMin = pinMin;
    options.want = want;
    return options;
  }

  std::vector<double> normalized(const std::vector<double>& weights) {
    double total = 0;
    for (const double weight : weights) {
      total += weight;
    }
    std::vector<double> shares;
    shares.reserve(weights.size());
    for (const double weight : weights) {
      shares.push_back(weight / total);
    }
    return shares;
  }

  double widthSum(const FlameRowSplit& split) {
    double sum = split.more ? split.more->w : 0.0;
    for (const auto& seg : split.segs) {
      sum += seg.w;
    }
    return sum;
  }

  const FlameSegment* segmentOf(const FlameRowSplit& split, std::size_t index) {
    const auto found = std::ranges::find(split.segs, index, &FlameSegment::index);
    return found == split.segs.end() ? nullptr : &*found;
  }

  void testRowCoversSpanHeaviestFirst() {
    const auto shares = normalized({500, 300, 100, 50, 30, 10, 5, 5});
    const auto split = flameRowLayout(shares, 10, 1010, rowOptions(3));
    TEST_CHECK(!split.more.has_value());
    TEST_NEAR(widthSum(split), 1000.0, kEps);
    TEST_CHECK(std::ranges::all_of(split.segs, [](const FlameSegment& seg) { return seg.w >= 3 - kEps; }));
    TEST_CHECK(split.segs[0].w > split.segs[1].w);
    TEST_CHECK(std::abs(split.segs[0].w - 500) < 1 && split.segs[0].x == 10);
    const std::vector<double> expected{500, 300, 100, 50, 30, 10, 5, 5};
    for (std::size_t i = 0; i < expected.size(); ++i) {
      TEST_CHECK(split.segs[i].index == i);
      TEST_NEAR(split.segs[i].w, expected[i], kEps);
    }
    TEST_NEAR(split.segs[7].x, 1005.0, kEps);
  }

  void testTinyEntriesMergeUnderTheCap() {
    std::vector<double> weights{1000};
    weights.resize(501, 1);
    const auto split = flameRowLayout(normalized(weights), 0, 1000, rowOptions(3));
    TEST_CHECK(split.more.has_value());
    TEST_CHECK(split.more->w <= 150 + kEps);
    TEST_CHECK(split.more->indices.size() + split.segs.size() == 501);
    TEST_NEAR(split.more->x + split.more->w, 1000.0, kEps);
    TEST_CHECK(std::ranges::all_of(split.segs, [](const FlameSegment& seg) { return seg.w >= 3 - kEps; }));
  }

  void testMergedPieceBelowTheCapKeepsItsShare() {
    const auto split = flameRowLayout(normalized({1000, 500, 2}), 0, 1000, rowOptions(3));
    TEST_CHECK(split.more.has_value() && split.more->indices.size() == 1);
    TEST_CHECK(split.more->w >= 3 && split.more->w < 5);
    TEST_NEAR(split.segs[0].w, 664.6666666666666, kEps);
    TEST_NEAR(split.segs[1].w, 332.3333333333333, kEps);
    TEST_NEAR(split.more->x, 997.0, kEps);
    TEST_NEAR(split.more->w, 3.0, kEps);
  }

  void testPinnedEntryKeepsItsMinimum() {
    const auto shares = normalized({500, 300, 100, 50, 30, 10, 5, 5});
    const auto split = flameRowLayout(shares, 0, 1000, rowOptions(3, 0.15, 7, 120));
    const auto* pinned = segmentOf(split, 7);
    TEST_CHECK(pinned != nullptr && pinned->w >= 120 - kEps);
    TEST_NEAR(pinned->x, 880.0, kEps);
    TEST_NEAR(widthSum(split), 1000.0, kEps);
    TEST_CHECK(std::ranges::all_of(split.segs, [](const FlameSegment& seg) { return seg.w >= 3 - kEps; }));
    TEST_NEAR(split.segs[0].w, 442.2110552763819, kEps);
    std::vector<double> weights{1000};
    weights.resize(301, 1);
    const auto many = flameRowLayout(normalized(weights), 0, 1000, rowOptions(3, 0.15, 250, 120));
    const auto* buried = segmentOf(many, 250);
    TEST_CHECK(buried != nullptr && buried->w >= 120 - kEps);
    TEST_CHECK(many.more.has_value() && !std::ranges::contains(many.more->indices, std::size_t{250}));
    TEST_CHECK(many.more->w <= 150 + kEps);
  }

  void testPinnedEntryNaturalWidthAndNarrowSpan() {
    const auto split = flameRowLayout(normalized({800, 200}), 0, 1000, rowOptions(3, 0.15, 0, 120));
    TEST_CHECK(std::abs(split.segs[0].w - 800) < 1);
    const auto narrow = flameRowLayout(normalized({1, 1, 1}), 0, 100, rowOptions(3, 0.15, 1, 100));
    TEST_CHECK(narrow.segs.size() == 1);
    TEST_NEAR(narrow.segs[0].w, 100.0, kEps);
    const auto empty = flameRowLayout({}, 0, 100, rowOptions(3));
    TEST_CHECK(empty.segs.empty() && !empty.more.has_value());
  }

  void testRowDegenerateInputs() {
    const auto shares = normalized({3, 2, 1});
    for (const auto& [x0, x1] : {std::pair{0.0, 0.0}, {10.0, 5.0}, {0.0, kNan}, {kNan, 10.0}, {0.0, kInfinity}}) {
      const auto split = flameRowLayout(shares, x0, x1, rowOptions(3));
      TEST_CHECK(split.segs.empty() && !split.more.has_value());
    }
    // Shares that are not finite or negative read as 0; with nothing left the entries split evenly.
    const std::vector<double> bad{kNan, -1, kInfinity};
    const auto even = flameRowLayout(bad, 0, 300, rowOptions(3));
    TEST_CHECK(even.segs.size() == 3);
    for (const auto& seg : even.segs) {
      TEST_NEAR(seg.w, 100.0, kEps);
    }
    // A pin past the list is ignored; a bad minimum or cap reads as 0 and 0.15.
    const auto ignored = flameRowLayout(shares, 0, 600, rowOptions(3, 0.15, 9, 500));
    TEST_NEAR(ignored.segs[0].w, 300.0, kEps);
    const auto sanitized = flameRowLayout(shares, 0, 600, rowOptions(kNan, kNan));
    TEST_CHECK(sanitized.segs.size() == 3);
    TEST_NEAR(widthSum(sanitized), 600.0, kEps);
  }

  void testRowMergedPieceSqueezesOutEntries() {
    // The second entry earns 10.5 px by weight, but once the merged piece takes its room it would fall under 10.
    const std::vector<double> shares{0.85, 0.105, 0.009, 0.009, 0.009, 0.009, 0.009};
    const auto squeezed = flameRowLayout(shares, 0, 100, rowOptions(10));
    TEST_CHECK(squeezed.segs.size() == 1 && squeezed.segs[0].index == 0);
    TEST_NEAR(squeezed.segs[0].w, 85.0, kEps);
    TEST_CHECK(squeezed.more.has_value() && squeezed.more->indices.size() == 6 && squeezed.more->indices[0] == 1);
    TEST_NEAR(squeezed.more->x, 85.0, kEps);
    TEST_NEAR(squeezed.more->w, 15.0, kEps);
    // A span under two minimum widths is one merged piece holding everything.
    const auto whole = flameRowLayout(normalized({1, 1}), 0, 15, rowOptions(10));
    TEST_CHECK(whole.segs.empty() && whole.more.has_value() && whole.more->indices.size() == 2);
    TEST_NEAR(whole.more->x, 0.0, kEps);
    TEST_NEAR(whole.more->w, 15.0, kEps);
  }

  void testHeaviestEntriesGetTheirLabelWidth() {
    std::vector<double> weights;
    weights.reserve(40);
    for (int i = 0; i < 40; ++i) {
      weights.push_back(100.0 / (i + 1));
    }
    const auto shares = normalized(weights);
    const std::vector<double> want(40, 100);
    const auto plain = flameRowLayout(shares, 0, 1000, rowOptions(28));
    const auto wide = flameRowLayout(shares, 0, 1000, rowOptions(28, 0.15, kNoFlameItem, 0, want));
    TEST_NEAR(widthSum(wide), 1000.0, kEps);
    TEST_CHECK(std::ranges::all_of(wide.segs, [](const FlameSegment& seg) { return seg.w >= 28 - kEps; }));
    const auto namedIn = [](const FlameRowSplit& split) {
      return std::ranges::count_if(split.segs, [](const FlameSegment& seg) { return seg.w >= 100 - kEps; });
    };
    const auto named = static_cast<std::size_t>(namedIn(wide));
    TEST_CHECK(named > static_cast<std::size_t>(namedIn(plain)) && named >= 4);
    for (std::size_t i = 0; i < named; ++i) {
      TEST_CHECK(wide.segs[i].index == i);
    }
    for (std::size_t i = 1; i < wide.segs.size(); ++i) {
      TEST_CHECK(wide.segs[i].x >= wide.segs[i - 1].x + wide.segs[i - 1].w - kEps);
    }
    TEST_CHECK(wide.segs.size() == 8 && wide.more.has_value() && wide.more->indices.size() == 32);
    TEST_NEAR(wide.segs[0].w, 150.0, kEps);
    TEST_NEAR(wide.segs[7].x, 750.0, kEps);
    TEST_NEAR(wide.more->x, 850.0, kEps);
    const std::vector<double> huge(40, 900);
    const auto degraded = flameRowLayout(shares, 0, 1000, rowOptions(28, 0.15, kNoFlameItem, 0, huge));
    TEST_CHECK(std::ranges::all_of(degraded.segs, [](const FlameSegment& seg) { return seg.w >= 28 - kEps; }));
    TEST_CHECK(degraded.segs[0].w <= 1000);
    // A want of zero keeps every floor at the minimum: still a weight split, not a plain one.
    const std::vector<double> zero(5, 0);
    const auto floored =
        flameRowLayout(normalized({5, 4, 3, 2, 1}), 0, 300, rowOptions(18, 0.15, kNoFlameItem, 0, zero));
    const std::vector<double> expected{100, 80, 60, 40, 20};
    for (std::size_t i = 0; i < expected.size(); ++i) {
      TEST_NEAR(floored.segs[i].w, expected[i], kEps);
    }
  }

  void testLabelTextWidth() {
    LabelFont font;
    font.ascii['a'] = 6;
    font.ascii['W'] = 11;
    font.other = 9;
    TEST_CHECK(labelTextWidth("aWa", font) == 23);
    TEST_CHECK(labelTextWidth("a\xC3\xA9", font) == 15);
    TEST_CHECK(labelTextWidth("\xF0\x9F\x99\x82", font) == 9);
    TEST_CHECK(labelTextWidth("", font) == 0);
  }

} // namespace

int main() {
  testRowCoversSpanHeaviestFirst();
  testTinyEntriesMergeUnderTheCap();
  testMergedPieceBelowTheCapKeepsItsShare();
  testPinnedEntryKeepsItsMinimum();
  testPinnedEntryNaturalWidthAndNarrowSpan();
  testRowDegenerateInputs();
  testRowMergedPieceSqueezesOutEntries();
  testHeaviestEntriesGetTheirLabelWidth();
  testLabelTextWidth();
  std::println("workbench flame row checks passed");
  return 0;
}
