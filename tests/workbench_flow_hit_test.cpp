#include "test_check.h"
#include "workbench/flow_hit.h"
#include "workbench_flow_fixture.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <print>
#include <vector>

using namespace luciole;
using namespace luciole::workbench;

namespace {
  using test::placeWide;
  using test::wideFrom;
  using test::wideTree;

  std::optional<FlowHit> hitOf(const FlowLayout& layout, double x, double y) {
    // Label widths as drawn: 35 px for the current folder, 42 px for the rest.
    std::vector<double> widths(layout.labels.size(), 42.0);
    widths[0] = 35.0;
    return flowHitAt(layout, widths, x, y);
  }

  void hitFindsPillsBarsAndLabels(const Tree& wide) {
    const auto layout = flowLayout(wideFrom(wide, 2, {1, 0}), {}, 900, 280);
    const auto stem = FlowHit{.kind = FlowHitKind::Stem, .index = 0};
    const auto current = FlowHit{.kind = FlowHitKind::Record, .index = 0};
    TEST_CHECK(hitOf(layout, 38, 140) == stem);
    TEST_CHECK(hitOf(layout, 55, 140) == stem);
    TEST_CHECK(!hitOf(layout, 0, 0).has_value());
    TEST_CHECK(!hitOf(layout, 400, 100).has_value());
    TEST_CHECK(hitOf(layout, 180, 36.59999999999998) == current);
    TEST_CHECK(hitOf(layout, 175, 40.59999999999998) == current);
    TEST_CHECK(hitOf(layout, 198, 140) == current);
    TEST_CHECK(!flowHitAt(layout, {}, 198, 140).has_value());

    const auto& first = layout.topology.columns[1];
    TEST_CHECK(first.size() == 5);
    const auto last = first[3];
    const auto& bar = layout.topology.records[last];
    TEST_CHECK(bar.kind == FlowRecordKind::Folder && bar.node == 6);
    TEST_NEAR(bar.y, 201.3027955415704, 1e-9);
    const double middle = bar.y + bar.height / 2;
    const auto lastHit = FlowHit{.kind = FlowHitKind::Record, .index = last};
    TEST_CHECK(hitOf(layout, 357, middle) == lastHit);
    TEST_CHECK(!hitOf(layout, 356, middle).has_value());
    TEST_CHECK(hitOf(layout, 391, middle) == lastHit);
    TEST_CHECK(hitOf(layout, 384, middle) == lastHit);
    TEST_CHECK(!hitOf(layout, std::nan(""), middle).has_value());
    TEST_CHECK(!flowHitAt(FlowLayout{}, {}, 10, 10).has_value());
  }

  void hitPrefersTheNearestCenter() {
    FlowLayout layout;
    layout.topology.records.resize(2);
    layout.topology.records[0].x = 10;
    layout.topology.records[0].y = 10;
    layout.topology.records[0].width = 16;
    layout.topology.records[0].height = 14;
    layout.topology.records[1] = layout.topology.records[0];
    layout.topology.records[1].y = 25;
    // The padded boxes (8 to 26 and 23 to 41) overlap; the nearer center wins, the first one on a tie.
    TEST_CHECK(flowHitAt(layout, {}, 12, 23.5) == (FlowHit{.kind = FlowHitKind::Record, .index = 0}));
    TEST_CHECK(flowHitAt(layout, {}, 12, 25.5) == (FlowHit{.kind = FlowHitKind::Record, .index = 1}));
    TEST_CHECK(flowHitAt(layout, {}, 12, 24.5) == (FlowHit{.kind = FlowHitKind::Record, .index = 0}));
    TEST_CHECK(!flowHitAt(layout, {}, 12, 41).has_value());
    TEST_CHECK(flowHitAt(layout, {}, 6, 17) == (FlowHit{.kind = FlowHitKind::Record, .index = 0}));
    TEST_CHECK(!flowHitAt(layout, {}, 30, 17).has_value());
  }

  void smallStemPiecesAreSixteenPixelsTallToHit(const Tree& wide) {
    const auto layout = flowLayout(wideFrom(wide, 2, {1, 0, 0, 0, 0, 0}), {}, 900, 280);
    const auto& dot = layout.stem[0];
    TEST_NEAR(dot.height, 8.0, 0.0);
    const double middle = dot.y + dot.height / 2;
    const auto hit = FlowHit{.kind = FlowHitKind::Stem, .index = 0};
    TEST_CHECK(flowHitAt(layout, {}, dot.x + 1, middle + 7.5) == hit);
    TEST_CHECK(flowHitAt(layout, {}, dot.x + 1, middle - 8) == hit);
    TEST_CHECK(!flowHitAt(layout, {}, dot.x + 1, middle + 8).has_value());
    TEST_CHECK(flowHitAt(layout, {}, dot.x - 1, middle) == hit);
    TEST_CHECK(!flowHitAt(layout, {}, dot.x - 1.5, middle).has_value());
  }
} // namespace

int main() {
  const auto wide = wideTree();
  hitFindsPillsBarsAndLabels(wide);
  hitPrefersTheNearestCenter();
  smallStemPiecesAreSixteenPixelsTallToHit(wide);
  std::println("workbench flow hit checks passed");
  return 0;
}
