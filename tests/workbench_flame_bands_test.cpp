#include "test_check.h"
#include "workbench/flame_bands.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <print>
#include <vector>

namespace {
  using namespace luciole::workbench;

  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInfinity = std::numeric_limits<double>::infinity();

  struct ExpectedRow {
    FlameRowKind kind;
    std::size_t level;
    double y;
    double h;
  };

  void checkBands(const FlameBands& bands, const std::vector<ExpectedRow>& expected) {
    TEST_CHECK(bands.rows.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
      TEST_CHECK(bands.rows[i].kind == expected[i].kind && bands.rows[i].level == expected[i].level);
      TEST_CHECK(bands.rows[i].y == expected[i].y && bands.rows[i].h == expected[i].h);
    }
  }

  double heightOf(const FlameBands& bands, FlameRowKind kind, std::size_t level) {
    const auto found =
        std::ranges::find_if(bands.rows, [&](const FlameRow& row) { return row.kind == kind && row.level == level; });
    return found == bands.rows.end() ? -1.0 : found->h;
  }

  std::vector<double> aheadHeights(const FlameBands& bands) {
    std::vector<double> heights;
    for (const auto& row : bands.rows) {
      if (row.kind == FlameRowKind::Ahead) {
        heights.push_back(row.h);
      }
    }
    return heights;
  }

  void testBandsFixedThicknesses() {
    using enum FlameRowKind;
    const auto bands = flameBands(290, 6);
    const std::vector<double> ancestors{24, 16, 12, 12, 6, 4};
    for (std::size_t k = 1; k <= 6; ++k) {
      TEST_CHECK(heightOf(bands, Ancestor, k) == ancestors[k - 1]);
    }
    TEST_CHECK(!bands.far);
    TEST_CHECK(heightOf(bands, Bar, 0) == 26);
    for (std::size_t i = 1; i < bands.rows.size(); ++i) {
      TEST_CHECK(bands.rows[i].y >= bands.rows[i - 1].y);
    }
    checkBands(
        bands,
        {{Ancestor, 6, 8, 4},
         {Ancestor, 5, 13, 6},
         {Ancestor, 4, 20, 12},
         {Ancestor, 3, 33, 12},
         {Ancestor, 2, 46, 16},
         {Ancestor, 1, 63, 24},
         {Bar, 0, 91, 26},
         {Ahead, 1, 118, 64},
         {Ahead, 2, 183, 49},
         {Ahead, 3, 233, 34}}
    );
    TEST_CHECK(bands.bottom == 267 && bands.strata == 6 && bands.ahead == 3);
    TEST_NEAR(bands.scale, 1.4545454555454547, 1e-15);
    const auto deep = flameBands(280, 11);
    TEST_CHECK(deep.far);
    TEST_CHECK(deep.rows[0].kind == Far);
    TEST_CHECK(std::ranges::count(deep.rows, Ancestor, &FlameRow::kind) == 6);
    checkBands(
        deep,
        {{Far, 7, 8, 4},
         {Ancestor, 6, 13, 4},
         {Ancestor, 5, 18, 6},
         {Ancestor, 4, 25, 12},
         {Ancestor, 3, 38, 12},
         {Ancestor, 2, 51, 16},
         {Ancestor, 1, 68, 24},
         {Bar, 0, 96, 26},
         {Ahead, 1, 123, 62},
         {Ahead, 2, 186, 48},
         {Ahead, 3, 235, 34}}
    );
    TEST_CHECK(deep.bottom == 269);
    TEST_NEAR(deep.scale, 1.4313725490196079, 1e-15);
    checkBands(
        flameBands(224, 11),
        {{Far, 5, 8, 4},
         {Ancestor, 4, 13, 12},
         {Ancestor, 3, 26, 12},
         {Ancestor, 2, 39, 16},
         {Ancestor, 1, 56, 24},
         {Bar, 0, 84, 26},
         {Ahead, 1, 111, 44},
         {Ahead, 2, 156, 34},
         {Ahead, 3, 191, 24}}
    );
    checkBands(
        flameBands(158, 11),
        {{Ancestor, 4, 8, 12},
         {Ancestor, 3, 21, 12},
         {Ancestor, 2, 34, 16},
         {Ancestor, 1, 51, 24},
         {Bar, 0, 79, 26},
         {Ahead, 1, 106, 24},
         {Ahead, 2, 131, 18}}
    );
    checkBands(flameBands(200, 0), {{Bar, 0, 8, 26}, {Ahead, 1, 35, 64}, {Ahead, 2, 100, 49}, {Ahead, 3, 150, 34}});
    TEST_CHECK(flameBands(281, 11).bottom == 271);
    TEST_CHECK((aheadHeights(flameBands(281, 11)) == std::vector<double>{63, 49, 34}));
  }

  void testBandsFitAtEveryDepth() {
    for (const double height : {160.0, 180.0, 200.0, 260.0, 320.0}) {
      for (const std::size_t depth : {0U, 1U, 2U, 3U, 5U, 6U, 11U, 30U}) {
        const auto bands = flameBands(height, depth);
        TEST_CHECK(bands.rows[0].y >= 8);
        TEST_CHECK(bands.bottom <= height - 8);
        for (std::size_t i = 1; i < bands.rows.size(); ++i) {
          TEST_CHECK(bands.rows[i].y >= bands.rows[i - 1].y + bands.rows[i - 1].h);
        }
        if (depth >= 2) {
          TEST_CHECK(bands.strata >= 2);
        }
        const auto ahead = aheadHeights(bands).size();
        TEST_CHECK(ahead == bands.ahead && ahead >= 2 && ahead <= 3 && (height < 200 || ahead == 3));
      }
    }
    for (const double height : {kFlameMinRibbon, 160.0, 180.0, 200.0, kFlameMinBand, 260.0, 320.0}) {
      for (const std::size_t depth : {2U, 3U, 4U, 5U, 11U, 30U}) {
        const auto bands = flameBands(height, depth);
        std::vector<double> outs;
        for (const auto& row : bands.rows) {
          if (row.kind == FlameRowKind::Ancestor && row.level >= 2 && row.level <= 4) {
            outs.push_back(row.h);
          }
        }
        TEST_CHECK(outs.size() == std::min<std::size_t>(depth, 4) - 1);
        TEST_CHECK(std::ranges::all_of(outs, [](double h) { return h >= 12; }));
        TEST_CHECK(heightOf(bands, FlameRowKind::Ancestor, 1) == 24);
      }
    }
    TEST_CHECK(kFlameMinBand >= 200 && kFlameMinBand <= 230);
    TEST_CHECK((aheadHeights(flameBands(kFlameMinBand, 11)) == std::vector<double>{44, 34, 24}));
    TEST_CHECK(aheadHeights(flameBands(kFlameMinBand, 0))[0] > 44);
    for (const double height : {260.0, 320.0}) {
      for (const std::size_t depth : {0U, 3U, 11U}) {
        TEST_CHECK(aheadHeights(flameBands(height, depth))[0] <= 64);
      }
    }
    TEST_CHECK(kFlameMaxBand >= 250 && kFlameMaxBand <= 320);
    TEST_CHECK(std::ranges::none_of(flameBands(200, 0).rows, [](const FlameRow& row) {
      return row.kind == FlameRowKind::Ancestor || row.kind == FlameRowKind::Far;
    }));
  }

  void testBandsOverTheRibbonRange() {
    TEST_CHECK(kFlameMinRibbon <= 160 && kFlameMinRibbon >= 150);
    TEST_CHECK((aheadHeights(flameBands(kFlameMinBand, 11)) == std::vector<double>{44, 34, 24}));
    TEST_CHECK((aheadHeights(flameBands(kFlameMinRibbon, 11)) == std::vector<double>{24, 18}));
    double previous = 0;
    // Every integer height from the ribbon's minimum to well past its maximum.
    for (int height = 160; height <= 320; ++height) {
      const auto bands = flameBands(height, 11);
      TEST_CHECK(bands.bottom <= height - 8 && bands.rows[0].y >= 8);
      std::vector<double> outs;
      bool beyondFourth = false;
      for (const auto& row : bands.rows) {
        if (row.kind == FlameRowKind::Ancestor && row.level <= 4) {
          outs.push_back(row.h);
        }
        beyondFourth = beyondFourth || (row.kind == FlameRowKind::Ancestor && row.level > 4);
      }
      std::ranges::sort(outs, std::greater{});
      TEST_CHECK((outs == std::vector<double>{24, 16, 12, 12}));
      double total = 0;
      for (const double h : aheadHeights(bands)) {
        total += h;
      }
      TEST_CHECK(total >= previous - 1);
      previous = total;
      if (height < kFlameMinBand) {
        TEST_CHECK(!beyondFourth);
      }
    }
    TEST_CHECK(aheadHeights(flameBands(kFlameMinRibbon, 0)).size() >= 2);
    TEST_CHECK(flameBands(kFlameMaxBand, 11).far);
  }

  void testBandsDegenerateHeights() {
    for (const double height : {kNan, -40.0}) {
      const auto bands = flameBands(height, 3);
      const auto zero = flameBands(0, 3);
      TEST_CHECK(bands.rows.size() == zero.rows.size() && bands.bottom == zero.bottom);
    }
    const auto huge = flameBands(kInfinity, 3);
    TEST_CHECK(std::ranges::all_of(huge.rows, [](const FlameRow& row) { return std::isfinite(row.h); }));
    TEST_CHECK(aheadHeights(huge)[0] == 64);
    const auto deepest = flameBands(400, std::numeric_limits<std::size_t>::max());
    TEST_CHECK(deepest.strata == 6 && deepest.far);
  }

  void testBandHeightAndLimits() {
    TEST_CHECK(flameBandHeight(100) == kFlameMinRibbon);
    TEST_CHECK(flameBandHeight(200) == 200);
    TEST_CHECK(flameBandHeight(900) == kFlameMaxBand);
    TEST_CHECK(flameBandHeight(kNan) == kFlameMinRibbon);
    TEST_CHECK(kFlameLimits.minW == 480 && kFlameLimits.minH == 160);
    TEST_CHECK(kFlameLimits.prefH == 224 && kFlameLimits.maxH == 281);
  }

} // namespace

int main() {
  testBandsFixedThicknesses();
  testBandsFitAtEveryDepth();
  testBandsOverTheRibbonRange();
  testBandsDegenerateHeights();
  testBandHeightAndLimits();
  std::println("workbench flame bands checks passed");
  return 0;
}
