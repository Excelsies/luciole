#include "test_check.h"
#include "workbench/column_plan.h"

#include <array>
#include <cstddef>
#include <print>
#include <vector>

// The center's plan at three widths and three depths, with the default strip pane, the default column widths and
// the 320 px list minimum. The pane keeps its 153 px whatever the depth, so the list starts at the same place.
namespace {
  using namespace luciole::workbench;

  struct Expected {
    double width = 0;
    std::size_t ancestors = 0;
    double listWidth = 0;
    std::size_t columns = 0;
    double spare = 0;
  };

  // Strips farthest first: {kind is a stack, first level, last level, width, x}.
  struct StripAt {
    bool stack = false;
    std::size_t level = 0;
    std::size_t lastLevel = 0;
    double width = 0;
    double x = 0;
  };

  // Three named bands fit the default pane; deeper folders show four and fold the rest beyond them.
  const std::vector<StripAt> kThree{{false, 3, 3, 26, 46}, {false, 2, 2, 34, 73}, {false, 1, 1, 44, 108}};
  const std::vector<StripAt> kSeven{
      {true, 5, 7, 24, 0}, {false, 4, 4, 20, 25}, {false, 3, 3, 26, 46}, {false, 2, 2, 34, 73}, {false, 1, 1, 44, 108},
  };
  const std::vector<StripAt> kTwelve{
      {true, 5, 12, 24, 0}, {false, 4, 4, 20, 25}, {false, 3, 3, 26, 46}, {false, 2, 2, 34, 73}, {false, 1, 1, 44, 108},
  };

  const std::vector<StripAt>& stripsFor(std::size_t ancestors) {
    return ancestors == 3 ? kThree : ancestors == 7 ? kSeven : kTwelve;
  }

  constexpr std::array<Expected, 9> kPlans{{
      {700, 3, 547, 0, 227},
      {700, 7, 547, 0, 227},
      {700, 12, 547, 0, 227},
      {845, 3, 431, 1, 111},
      {845, 7, 431, 1, 111},
      {845, 12, 431, 1, 111},
      {1500, 3, 754, 3, 434},
      {1500, 7, 754, 3, 434},
      {1500, 12, 754, 3, 434},
  }};

  // Column x and width when one or three are drawn.
  constexpr std::array<std::array<double, 2>, 3> kWideColumns{{{907, 260}, {1168, 190}, {1359, 140}}};

  void plansMatch() {
    for (const auto& expected : kPlans) {
      const auto plan = planColumns(expected.width, expected.ancestors);
      std::println(
          "{} px, {} ancestors: strips {} px in {}, list {} at {}, {} columns", expected.width, expected.ancestors,
          plan.stripsWidth, plan.strips.size(), plan.list.width, plan.list.x, plan.columns.size()
      );
      TEST_CHECK(plan.margin == 0);
      TEST_CHECK(plan.stripsWidth == 153);
      TEST_CHECK(plan.list.x == 153);
      TEST_CHECK(plan.list.width == expected.listWidth);
      TEST_CHECK(plan.columns.size() == expected.columns);
      TEST_CHECK(plan.spare == expected.spare);
      const auto& strips = stripsFor(expected.ancestors);
      TEST_CHECK(plan.strips.size() == strips.size());
      for (std::size_t i = 0; i < strips.size() && i < plan.strips.size(); ++i) {
        const auto& strip = plan.strips[i];
        TEST_CHECK((strip.kind == StripKind::Stack) == strips[i].stack);
        TEST_CHECK(strip.level == strips[i].level && strip.lastLevel == strips[i].lastLevel);
        TEST_CHECK(strip.width == strips[i].width);
        TEST_CHECK(strip.x == strips[i].x);
      }
      if (expected.columns == 1) {
        TEST_CHECK(plan.columns[0].x == 153 + expected.listWidth && plan.columns[0].width == 260);
      }
      if (expected.columns == 3) {
        for (std::size_t i = 0; i < 3; ++i) {
          TEST_CHECK(plan.columns[i].x == kWideColumns[i][0] && plan.columns[i].width == kWideColumns[i][1]);
        }
      }
    }
  }
} // namespace

int main() {
  plansMatch();
  std::println("workbench center plan checks passed");
  return 0;
}
