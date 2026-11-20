#include "model/directory_options.h"
#include "test_check.h"
#include "workbench/column_plan.h"
#include "workbench/pane_layout.h"
#include "workbench/preview_home.h"

#include <cstddef>
#include <print>

// Where a preview goes when its column may not fit, at the sizes of real windows. The view is the window less the
// sidebar and the bars around it, measured in a real window: a 1100x700 window leaves 883x606, and a 1920x1080
// output at scale 1.5 or 2 is a 1280x720 or 960x540 window that leaves 1063x626 or 743x446.
namespace {
  using luciole::WorkbenchPreviewPlace;
  using namespace luciole::workbench;

  constexpr std::size_t kAncestors = 6;

  struct Fit {
    std::size_t columns = 0;
    bool jumpOpen = false;
  };

  Fit fitAt(const PaneState& state, ViewSize view) {
    const auto layout = layoutPanes(state, view, PartLimits{}, RibbonKind::Flame);
    return {
        .columns = layout.rects.center ? planColumns(layout.rects.center->width, kAncestors).columns.size() : 0,
        .jumpOpen = jumpAheadOpen(layout),
    };
  }

  void smallWindowsHaveNoColumnAndAnOpenJumpAhead() {
    for (const auto view : {ViewSize{883, 606}, ViewSize{1063, 626}, ViewSize{743, 446}}) {
      const auto fit = fitAt({}, view);
      TEST_CHECK(fit.columns == 0);
      TEST_CHECK(fit.jumpOpen);
      TEST_CHECK(
          previewHome(0, fit.columns, fit.jumpOpen, WorkbenchPreviewPlace::JumpAheadWhenNarrow)
          == PreviewHome::JumpAhead
      );
      TEST_CHECK(
          previewHome(0, fit.columns, fit.jumpOpen, WorkbenchPreviewPlace::HiddenWhenNarrow) == PreviewHome::None
      );
      TEST_CHECK(
          previewHome(0, fit.columns, fit.jumpOpen, WorkbenchPreviewPlace::JumpAheadAlways) == PreviewHome::JumpAhead
      );
    }
  }

  void aRoomyWindowKeepsTheColumn() {
    const auto fit = fitAt({}, {1184, 806});
    TEST_CHECK(fit.columns >= 1);
    for (const auto choice : {WorkbenchPreviewPlace::JumpAheadWhenNarrow, WorkbenchPreviewPlace::HiddenWhenNarrow}) {
      TEST_CHECK(previewHome(0, fit.columns, fit.jumpOpen, choice) == PreviewHome::Column);
    }
    // the third choice leaves the column whatever the room
    TEST_CHECK(
        previewHome(0, fit.columns, fit.jumpOpen, WorkbenchPreviewPlace::JumpAheadAlways) == PreviewHome::JumpAhead
    );
  }

  void aFileBeyondTheLastColumnCountsAsNotFitting() {
    const auto jump = WorkbenchPreviewPlace::JumpAheadWhenNarrow;
    TEST_CHECK(previewHome(1, 2, true, jump) == PreviewHome::Column);
    TEST_CHECK(previewHome(1, 1, true, jump) == PreviewHome::JumpAhead);
    TEST_CHECK(previewHome(2, 2, true, jump) == PreviewHome::JumpAhead);
    TEST_CHECK(previewHome(1, 1, true, WorkbenchPreviewPlace::HiddenWhenNarrow) == PreviewHome::None);
    TEST_CHECK(previewHome(1, 2, true, WorkbenchPreviewPlace::HiddenWhenNarrow) == PreviewHome::Column);
    TEST_CHECK(previewHome(1, 3, true, WorkbenchPreviewPlace::JumpAheadAlways) == PreviewHome::JumpAhead);
  }

  void withoutAJumpAheadPaneThereIsNoPreview() {
    PaneState collapsed;
    collapsed.collapsed.jump = true;
    const auto chosen = fitAt(collapsed, {883, 606});
    TEST_CHECK(!chosen.jumpOpen);
    TEST_CHECK(
        previewHome(0, chosen.columns, chosen.jumpOpen, WorkbenchPreviewPlace::JumpAheadWhenNarrow) == PreviewHome::None
    );
    TEST_CHECK(
        previewHome(0, chosen.columns, chosen.jumpOpen, WorkbenchPreviewPlace::JumpAheadAlways) == PreviewHome::None
    );

    // too narrow for the left column: the rail stands in for both panes
    const auto rail = fitAt({}, {420, 606});
    TEST_CHECK(!rail.jumpOpen);
    TEST_CHECK(
        previewHome(0, rail.columns, rail.jumpOpen, WorkbenchPreviewPlace::JumpAheadWhenNarrow) == PreviewHome::None
    );

    // too short for the instrument and Jump ahead together: Jump ahead closes
    const auto short_ = fitAt({}, {883, 300});
    TEST_CHECK(!short_.jumpOpen);
    TEST_CHECK(
        previewHome(0, short_.columns, short_.jumpOpen, WorkbenchPreviewPlace::JumpAheadWhenNarrow) == PreviewHome::None
    );

    // the instrument expanded over the view hides Jump ahead
    const auto expanded = fitAt(withExpanded({}, true), {883, 606});
    TEST_CHECK(!expanded.jumpOpen);
    TEST_CHECK(previewHome(0, 1, expanded.jumpOpen, WorkbenchPreviewPlace::JumpAheadAlways) == PreviewHome::None);

    // a wide view with Jump ahead collapsed: the column keeps the preview unless the choice is "always"
    const auto wide = fitAt(collapsed, {1184, 806});
    TEST_CHECK(wide.columns >= 1 && !wide.jumpOpen);
    TEST_CHECK(
        previewHome(0, wide.columns, wide.jumpOpen, WorkbenchPreviewPlace::JumpAheadWhenNarrow) == PreviewHome::Column
    );
    TEST_CHECK(
        previewHome(0, wide.columns, wide.jumpOpen, WorkbenchPreviewPlace::JumpAheadAlways) == PreviewHome::None
    );
  }
} // namespace

int main() {
  smallWindowsHaveNoColumnAndAnOpenJumpAhead();
  aRoomyWindowKeepsTheColumn();
  aFileBeyondTheLastColumnCountsAsNotFitting();
  withoutAJumpAheadPaneThereIsNoPreview();
  std::println("workbench preview home checks passed");
  return 0;
}
