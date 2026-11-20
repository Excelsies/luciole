#include "test_check.h"
#include "workbench/flow_stem.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <print>
#include <vector>

namespace {
  using namespace luciole::workbench;

  void stemPillsShrinkAndFit() {
    TEST_CHECK(flowStemPlan(0, 200).pills.empty());
    TEST_NEAR(flowStemPlan(0, 200).width, 0.0, 0.0);
    const auto one = flowStemPlan(1, 200);
    TEST_CHECK(one.pills.size() == 1);
    TEST_NEAR(one.pills[0].height, 28.0, 0.0);
    TEST_NEAR(one.pills[0].width, 86.0, 0.0);
    TEST_NEAR(one.width, 86.0, 0.0);

    const auto deep = flowStemPlan(11, 200);
    TEST_CHECK(deep.pills.size() == 11);
    TEST_CHECK(deep.pills.back().level == 1);
    const std::array<double, 5> nearest{28, 22, 16, 12, 8};
    for (std::size_t i = 0; i < nearest.size(); ++i) {
      TEST_NEAR(deep.pills[deep.pills.size() - 1 - i].height, nearest[i], 0.0);
    }
    TEST_CHECK(deep.pills.front().kind == FlowPillKind::Dot);
    std::vector<std::size_t> labeled;
    for (const auto& pill : deep.pills) {
      if (pill.labeled) {
        labeled.push_back(pill.level);
      }
    }
    TEST_CHECK((labeled == std::vector<std::size_t>{2, 1}));
    TEST_CHECK(deep.width <= 200 + 1e-6);
    for (std::size_t i = 0; i < deep.pills.size(); ++i) {
      TEST_CHECK(deep.pills[i].width >= 2);
      if (i > 0) {
        TEST_CHECK(deep.pills[i].x >= deep.pills[i - 1].x + deep.pills[i - 1].width - 1e-6);
      }
    }
    TEST_NEAR(deep.width, 200.0, 1e-9);
    TEST_NEAR(deep.pills[0].width, 6.532374100719425, 1e-9);
    TEST_NEAR(deep.pills[9].x, 91.89928057553956, 1e-9);
    TEST_NEAR(deep.pills[9].width, 49.45323741007194, 1e-9);
    TEST_NEAR(deep.pills[10].x, 143.3525179856115, 1e-9);
    TEST_NEAR(deep.pills[10].width, 56.64748201438849, 1e-9);

    const auto huge = flowStemPlan(40, 200);
    TEST_CHECK(huge.width <= 200 + 1e-6);
    TEST_NEAR(huge.gap, 1.0, 0.0);
    TEST_NEAR(huge.pills[0].width, 1.5945945945945945, 1e-9);
    TEST_NEAR(huge.pills[37].width, 14.0, 1e-9);
    TEST_NEAR(huge.pills[39].width, 46.0, 1e-9);

    const auto three = flowStemPlan(3, 190);
    TEST_NEAR(three.width, 186.0, 0.0);
    TEST_NEAR(three.gap, 2.0, 0.0);
    TEST_NEAR(three.pills[1].x, 28.0, 0.0);
    TEST_CHECK(three.pills[0].kind == FlowPillKind::Pill);

    // Far pills stop at 1 px with 1 px gaps, so past 26 ancestors the stem outgrows 150 px.
    TEST_NEAR(flowStemPlan(26, 150).width, 150.0, 1e-9);
    TEST_NEAR(flowStemPlan(27, 150).width, 152.0, 1e-9);
    TEST_NEAR(flowStemPlan(100, 150).width, 298.0, 1e-9);
  }

  void nearestAncestorsStayTwelvePixels() {
    for (const std::size_t levels : {3U, 6U, 11U, 18U}) {
      for (const auto& pill : flowStemPlan(levels, 190).pills) {
        if (pill.level <= 3) {
          TEST_CHECK(std::min(pill.width, std::max(pill.height, 16.0)) >= 12);
        }
      }
    }
    for (const double width : {700.0, 2000.0}) {
      const double stem = std::max(150.0, std::min(250.0, width * 0.24));
      for (const std::size_t levels : {3U, 6U, 11U, 18U}) {
        for (const auto& pill : flowStemPlan(levels, stem).pills) {
          if (pill.level <= 3) {
            TEST_CHECK(pill.width >= 12 && pill.height >= 12);
          }
        }
      }
    }
  }

  // A path deeper than any filesystem allows keeps its nearest kMaxFlowStemLevels ancestors.
  void impossiblyDeepStemIsBounded() {
    const auto deep = flowStemPlan(std::numeric_limits<std::size_t>::max(), 150);
    TEST_CHECK(deep.pills.size() == kMaxFlowStemLevels);
    TEST_CHECK(deep.pills.front().level == kMaxFlowStemLevels && deep.pills.back().level == 1);
    TEST_CHECK(deep.pills.front().kind == FlowPillKind::Dot);
    const auto edge = flowStemPlan(kMaxFlowStemLevels, 150);
    TEST_CHECK(edge.pills.size() == kMaxFlowStemLevels);
    TEST_NEAR(edge.width, deep.width, 0.0);
  }
} // namespace

int main() {
  stemPillsShrinkAndFit();
  nearestAncestorsStayTwelvePixels();
  impossiblyDeepStemIsBounded();
  std::println("workbench flow stem checks passed");
  return 0;
}
