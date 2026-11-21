#include "test_check.h"
#include "workbench/column_plan.h"
#include "workbench/pane_layout.h"

#include <array>
#include <cmath>
#include <optional>

// The strip pane's drag and key steps at the text scales of a 14 pt and a 16 pt system font: they reach the scaled
// ends of the pane's range, no press is dead before an end, a saved width outside the range starts from the drawn
// one, and the center's least width keeps the list at its floor.
namespace {
  using namespace luciole::workbench;

  constexpr std::array<double, 2> kScales{1.0, 16.0 * 96.0 / 72.0 / 14.0};
  constexpr double kView = 1600;

  ColumnPlan planAt(double scale, std::optional<double> saved, double view = kView) {
    PlanOptions options;
    options.textScale = scale;
    options.stripsWidth = saved;
    return planColumns(view, 5, options);
  }

  void dragsReachBothEnds(double scale) {
    const auto plan = planAt(scale, std::nullopt);
    const auto drag = startStripsDrag(plan, std::nullopt);
    const auto wide = dragStrips(drag, drag.startX + 5000);
    const auto narrow = dragStrips(drag, drag.startX - 5000);
    TEST_CHECK(wide && *wide == std::min(plan.stripsMax, plan.stripsCeiling));
    TEST_CHECK(narrow && *narrow == plan.stripsMin);
    TEST_CHECK(plan.stripsMin == stripsMinWidthAt(scale));
    // what a drag saves is what the plan then draws
    for (const double value : {*wide, *narrow, 200.0}) {
      TEST_CHECK(
          planAt(scale, resizeStrips(value, plan.stripsMax, plan.stripsMin, plan.stripsCeiling)).stripsWidth == value
      );
    }
  }

  void keysReachBothEndsWithoutDeadPresses(double scale) {
    for (const double step : {1.0, -1.0}) {
      std::optional<double> saved;
      double last = planAt(scale, saved).stripsWidth;
      int presses = 0;
      for (; presses < 2000; ++presses) {
        const auto plan = planAt(scale, saved);
        const auto drag = startStripsDrag(plan, saved);
        TEST_CHECK(drag.wanted == plan.stripsWidth); // a key starts from the drawn width
        const auto next = stepStrips(drag, drag.startX + step);
        TEST_CHECK(next.has_value());
        if (*next == drag.wanted) {
          break;
        }
        TEST_CHECK(*next == drag.wanted + step);
        saved = next;
        last = *next;
      }
      const auto plan = planAt(scale, saved);
      TEST_CHECK(presses > 50);
      TEST_CHECK(last == (step > 0 ? plan.stripsCeiling : plan.stripsMin));
      TEST_CHECK(plan.stripsWidth == last);
    }
  }

  void aSavedWidthOutsideTheRangeStartsFromTheDrawnOne(double scale) {
    const auto plan = planAt(scale, 80);
    const auto drag = startStripsDrag(plan, 80);
    TEST_CHECK(drag.wanted == plan.stripsWidth);
    TEST_CHECK(drag.wanted == std::max(80.0, plan.stripsMin));
    const auto up = stepStrips(drag, drag.startX + 1);
    TEST_CHECK(up && *up == drag.wanted + 1);
  }

  void theCenterKeepsTheList(double scale) {
    PartLimits limits = scaleLimits(PartLimits{}, scale);
    if (scale == 1.0) {
      TEST_CHECK(limits.centerMinWidth == kCenterMinWidth);
    }
    TEST_CHECK(limits.centerMinWidth == kListFloor + stripsMinWidthAt(scale));
    const auto plan = planAt(scale, std::nullopt, limits.centerMinWidth);
    TEST_CHECK(plan.list.width >= kListFloor);
    // a narrow window closes the ribbon or the left column before the center falls under it
    for (double width = 700; width < 1800; width += 25) {
      const auto layout = layoutPanes({}, {.width = width, .height = 900}, limits, RibbonKind::Flame);
      if (layout.rects.center) {
        TEST_CHECK(layout.rects.center->width >= limits.centerMinWidth || layout.squeezed);
      }
    }
  }
  // A width past the pane's range at the text size is kept as saved and drawn at the most: 450 at 1.52, 321 at 1.
  void aWideSavedWidth(double scale) {
    const auto plan = planAt(scale, 450);
    TEST_CHECK(plan.stripsWidth == std::min(450.0, plan.stripsCeiling));
    const auto drag = startStripsDrag(plan, 450);
    TEST_CHECK(drag.wanted == plan.stripsWidth);
    const auto next = dragStrips(drag, drag.startX - 10);
    TEST_CHECK(next && *next == plan.stripsWidth - 10);
    TEST_CHECK(normalizeStripsWidth(450) == 450.0);
    if (scale > 1.0) {
      // dragged to 450 and saved, the pane draws 450 and keeps it
      const auto wide = dragStrips(drag, drag.startX + 5000);
      const auto set = resizeStrips(450, plan.stripsMax, plan.stripsMin, plan.stripsCeiling);
      TEST_CHECK(wide && *wide >= 450 && set == 450.0 && planAt(scale, set).stripsWidth == 450);
    }
  }

  // At the default size the limits are the part's own, fractional or not.
  void limitsAtTheDefaultSizeAreUntouched() {
    PartLimits limits;
    limits.ribbon.minHeight = 76.5;
    limits.ribbon.preferredHeight = 160.25;
    limits.instrument.minWidth = 240.4;
    TEST_CHECK(scaleLimits(limits, 1.0) == limits);
  }
} // namespace

int main() {
  for (const double scale : kScales) {
    dragsReachBothEnds(scale);
    keysReachBothEndsWithoutDeadPresses(scale);
    aSavedWidthOutsideTheRangeStartsFromTheDrawnOne(scale);
    theCenterKeepsTheList(scale);
    aWideSavedWidth(scale);
  }
  limitsAtTheDefaultSizeAreUntouched();
  return 0;
}
