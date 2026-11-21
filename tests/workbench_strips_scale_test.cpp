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

  constexpr std::array<double, 4> kScales{1.0, 44.0 / 42.0, 16.0 * 96.0 / 72.0 / 14.0, 3.0};
  constexpr double kView = 1600;

  ColumnPlan planAt(double scale, std::optional<double> saved, double view = kView) {
    PlanOptions options;
    options.textScale = scale;
    options.stripsWidth = saved;
    return planColumns(view, 5, options);
  }

  // What the page does with a drag or key result: it saves the width at the default size, and the pane draws that
  // times the scale; the two agree, in range, after any resize and after a reload.
  double saveAndDraw(double scale, double drawn, const ColumnPlan& before) {
    const double saved = savedStripsWidth(drawn, scale);
    TEST_CHECK(saved >= kStripsMinWidth && saved <= kStripsMaxWidth);
    const auto plan = planAt(scale, saved);
    TEST_CHECK(plan.stripsWidth == drawn);
    TEST_CHECK(plan.stripsMin == before.stripsMin);
    return saved;
  }

  void dragsReachBothEnds(double scale) {
    const auto plan = planAt(scale, std::nullopt);
    const auto drag = startStripsDrag(plan, std::nullopt);
    const auto wide = dragStrips(drag, drag.startX + 5000);
    const auto narrow = dragStrips(drag, drag.startX - 5000);
    // the ends, to the nearest width that can be saved
    TEST_CHECK(wide && *wide <= plan.stripsCeiling && *wide > plan.stripsCeiling - scale - 1);
    TEST_CHECK(narrow && *narrow >= plan.stripsMin && *narrow <= plan.stripsMin + scale + 1);
    for (const double travel : {-5000.0, -37.0, 0.0, 13.0, 155.0, 5000.0}) {
      const auto next = dragStrips(drag, drag.startX + travel);
      TEST_CHECK(next.has_value());
      (void)saveAndDraw(scale, *next, plan);
    }
    if (scale == 1.0) {
      TEST_CHECK(dragStrips(drag, drag.startX + 50) == drag.startWidth + 50);
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
        const double was = savedStripsWidth(drag.wanted, scale);
        saved = saveAndDraw(scale, *next, plan);
        TEST_CHECK(*saved == was + step); // every press moves the saved width, at any scale
        last = *next;
      }
      const auto plan = planAt(scale, saved);
      TEST_CHECK(presses > 50);
      if (step > 0) {
        TEST_CHECK(last <= plan.stripsCeiling && last > plan.stripsCeiling - scale - 1);
      } else {
        TEST_CHECK(last >= plan.stripsMin && last <= plan.stripsMin + scale + 1);
      }
      TEST_CHECK(plan.stripsWidth == last);
    }
  }

  // A saved width is in pixels at the default size: drawn times the scale, held to the range, and the first drag or
  // key starts from what is drawn.
  void aSavedWidthIsDrawnAtTheScale(double scale) {
    for (const double saved : {70.0, 80.0, 200.0, 321.0, 450.0}) {
      const auto plan = planAt(scale, saved);
      const auto want = std::clamp(std::round(saved * scale), plan.stripsMin, plan.stripsCeiling);
      TEST_CHECK(plan.stripsWidth == want);
      const auto drag = startStripsDrag(plan, saved);
      TEST_CHECK(drag.wanted == plan.stripsWidth);
      const auto up = stepStrips(drag, drag.startX + 1);
      TEST_CHECK(up && *up >= drag.wanted);
    }
  }

  // At 1.52 a drag to 450 saves a width within 70..321 that draws the same, before and after a reload.
  void aWideDragSurvivesTheRoundTrip(double scale) {
    const auto plan = planAt(scale, std::nullopt);
    const auto drag = startStripsDrag(plan, std::nullopt);
    const auto next = dragStrips(drag, drag.startX + (450 - drag.startWidth));
    TEST_CHECK(next.has_value());
    if (scale > 1.4) {
      TEST_CHECK(std::abs(*next - 450) <= scale);
    }
    const double saved = saveAndDraw(scale, *next, plan);
    TEST_CHECK(planAt(scale, saved).stripsWidth == planAt(scale, std::round(saved)).stripsWidth);
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
    aSavedWidthIsDrawnAtTheScale(scale);
    aWideDragSurvivesTheRoundTrip(scale);
    theCenterKeepsTheList(scale);
  }
  limitsAtTheDefaultSizeAreUntouched();
  return 0;
}
