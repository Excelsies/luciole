#include "model/details_columns.h"
#include "test_check.h"
#include "workbench/column_plan.h"

#include <limits>
#include <print>

namespace {
  using luciole::DetailsColumn;
  using luciole::DetailsColumns;
  using luciole::DetailsColumnVisibility;
  using luciole::DetailsColumnWidths;
  using luciole::kDefaultDetailsColumnWidths;
  using luciole::ViewMode;
  using luciole::workbench::columnLeftOut;
  using luciole::workbench::fitListColumns;
  using luciole::workbench::listHeader;

  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInfinity = std::numeric_limits<double>::infinity();

  // Type, Size and Modified, as a new List view shows them.
  const DetailsColumnVisibility kChosen{};

  bool shows(const DetailsColumns& columns, bool type, bool size, bool modified) {
    return columns.visible(DetailsColumn::Type) == type
        && columns.visible(DetailsColumn::Size) == size
        && columns.visible(DetailsColumn::Modified) == modified;
  }

  // Name 160 + Type 152 + Size 118 + Modified 156 + 8 = 594 px.
  void everyColumnFitsAWideList() {
    const auto exact = fitListColumns(594, kChosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(exact, true, true, true));
    TEST_NEAR(exact.nameWidth, 160, 1e-3);
    TEST_NEAR(exact.contentWidth, 594, 1e-3);
    const auto wide = fitListColumns(1000, kChosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(wide, true, true, true));
    TEST_NEAR(wide.nameWidth, 566, 1e-3); // Name takes the room that is left
    TEST_NEAR(wide.contentWidth, 1000, 1e-3);
    TEST_NEAR(wide.typeWidth, 144, 1e-3);
    TEST_NEAR(wide.sizeWidth, 110, 1e-3);
    TEST_NEAR(wide.modifiedWidth, 148, 1e-3);
  }

  // Type goes first, then Modified, then Size; Name stays.
  void columnsLeaveInOrder() {
    const auto noType = fitListColumns(593, kChosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(noType, false, true, true));
    TEST_NEAR(noType.nameWidth, 311, 1e-3);
    TEST_NEAR(noType.contentWidth, 593, 1e-3);
    TEST_CHECK(shows(fitListColumns(442, kChosen, kDefaultDetailsColumnWidths), false, true, true));
    const auto sizeOnly = fitListColumns(441, kChosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(sizeOnly, false, true, false));
    TEST_NEAR(sizeOnly.nameWidth, 315, 1e-3);
    const auto list350 = fitListColumns(350, kChosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(list350, false, true, false));
    TEST_NEAR(list350.contentWidth, 350, 1e-3);
    TEST_CHECK(shows(fitListColumns(286, kChosen, kDefaultDetailsColumnWidths), false, true, false));
    const auto nameOnly = fitListColumns(285, kChosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(nameOnly, false, false, false));
    TEST_NEAR(nameOnly.nameWidth, 277, 1e-3);
    TEST_NEAR(nameOnly.contentWidth, 285, 1e-3);
  }

  // Columns turned on beyond the three leave before Type, the last one first.
  void moreColumnsLeaveFirst() {
    auto chosen = kChosen;
    chosen.accessed = true;
    chosen.owner = true;
    // 594 + Accessed 156 + Owner 128 = 878
    const auto all = fitListColumns(878, chosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(all, true, true, true));
    TEST_CHECK(all.visible(DetailsColumn::Accessed) && all.visible(DetailsColumn::Owner));
    const auto noOwner = fitListColumns(877, chosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(noOwner, true, true, true));
    TEST_CHECK(noOwner.visible(DetailsColumn::Accessed) && !noOwner.visible(DetailsColumn::Owner));
    const auto noExtra = fitListColumns(749, chosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(noExtra, true, true, true));
    TEST_CHECK(!noExtra.visible(DetailsColumn::Accessed) && !noExtra.visible(DetailsColumn::Owner));
  }

  // Hidden columns stay hidden, chosen widths are kept, and a chosen Name width gives way to the room.
  void choicesAreKept() {
    auto chosen = kChosen;
    chosen.size = false;
    const auto noSize = fitListColumns(1000, chosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(noSize, true, false, true));
    auto widths = kDefaultDetailsColumnWidths;
    widths[0] = 400; // a Name width set in List view
    widths[2] = 200; // Size
    const auto wider = fitListColumns(700, kChosen, widths);
    TEST_CHECK(shows(wider, true, true, true));
    TEST_NEAR(wider.sizeWidth, 192, 1e-3);
    TEST_NEAR(wider.nameWidth, 184, 1e-3);
    TEST_NEAR(wider.contentWidth, 700, 1e-3);
    const auto narrower = fitListColumns(675, kChosen, widths);
    TEST_CHECK(shows(narrower, false, true, true));
    TEST_NEAR(narrower.nameWidth, 311, 1e-3);
  }

  // List view keeps every chosen column at its width and scrolls sideways; Workbench fits them and takes no drags.
  void headerFollowsTheMode() {
    const auto list = listHeader(ViewMode::List, 350, kChosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(list.columns, true, true, true));
    TEST_NEAR(list.columns.nameWidth, 160, 1e-3);
    TEST_NEAR(list.columns.contentWidth, 594, 1e-3);
    TEST_CHECK(list.resizable);
    const auto workbench = listHeader(ViewMode::Workbench, 350, kChosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(workbench.columns, false, true, false));
    TEST_NEAR(workbench.columns.nameWidth, 224, 1e-3); // 350 - Size 118 - 8
    TEST_NEAR(workbench.columns.contentWidth, 350, 1e-3);
    TEST_CHECK(!workbench.resizable);
    const auto grid = listHeader(ViewMode::Grid, 350, kChosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(grid.columns, true, true, true));
    TEST_CHECK(!grid.resizable);
    TEST_CHECK(!listHeader(ViewMode::Miller, 350, kChosen, kDefaultDetailsColumnWidths).resizable);
    auto widths = kDefaultDetailsColumnWidths;
    widths[0] = 400; // a Name width set in List view
    TEST_NEAR(listHeader(ViewMode::List, 1000, kChosen, widths).columns.nameWidth, 400, 1e-3);
    TEST_NEAR(listHeader(ViewMode::Workbench, 1000, kChosen, widths).columns.nameWidth, 566, 1e-3);
  }

  // A column that is chosen but not fitted is left out; one that is not chosen is not, nor is any in another mode.
  void leftOutColumns() {
    const auto fitted = listHeader(ViewMode::Workbench, 350, kChosen, kDefaultDetailsColumnWidths).columns;
    TEST_CHECK(columnLeftOut(ViewMode::Workbench, fitted, kChosen, DetailsColumn::Type));
    TEST_CHECK(columnLeftOut(ViewMode::Workbench, fitted, kChosen, DetailsColumn::Modified));
    TEST_CHECK(!columnLeftOut(ViewMode::Workbench, fitted, kChosen, DetailsColumn::Size));
    auto chosen = kChosen;
    luciole::detailsColumnFlag(chosen, DetailsColumn::Type) = false;
    TEST_CHECK(!columnLeftOut(ViewMode::Workbench, fitted, chosen, DetailsColumn::Type));
    TEST_CHECK(columnLeftOut(ViewMode::Workbench, fitted, chosen, DetailsColumn::Modified));
    TEST_CHECK(!columnLeftOut(ViewMode::List, fitted, kChosen, DetailsColumn::Type));
    const auto wide = listHeader(ViewMode::Workbench, 1000, kChosen, kDefaultDetailsColumnWidths).columns;
    TEST_CHECK(!columnLeftOut(ViewMode::Workbench, wide, kChosen, DetailsColumn::Type));
    TEST_CHECK(!columnLeftOut(ViewMode::Workbench, wide, kChosen, DetailsColumn::Modified));
  }

  void degenerateInputs() {
    for (const double viewport : {0.0, -10.0, kNan, -kInfinity, 100.0}) {
      const auto columns = fitListColumns(viewport, kChosen, kDefaultDetailsColumnWidths);
      TEST_CHECK(shows(columns, false, false, false));
      TEST_NEAR(columns.nameWidth, 160, 1e-3);
    }
    for (const double viewport : {kNan, kInfinity, -kInfinity}) {
      const auto list = listHeader(ViewMode::List, viewport, kChosen, kDefaultDetailsColumnWidths);
      TEST_CHECK(shows(list.columns, true, true, true));
      TEST_NEAR(list.columns.nameWidth, 160, 1e-3);
      TEST_NEAR(list.columns.contentWidth, 594, 1e-3);
    }
    const auto unbounded = fitListColumns(kInfinity, kChosen, kDefaultDetailsColumnWidths);
    TEST_CHECK(shows(unbounded, false, false, false));
    auto hostile = kDefaultDetailsColumnWidths;
    hostile[1] = -500;    // Type: read as its minimum, 64
    hostile[3] = 1 << 30; // Modified: read as its maximum, 2048
    const auto clamped = fitListColumns(1000, kChosen, hostile);
    TEST_CHECK(shows(clamped, false, true, false));
    TEST_NEAR(clamped.typeWidth, 56, 1e-3);
    TEST_NEAR(clamped.modifiedWidth, 2040, 1e-3);
    TEST_NEAR(clamped.contentWidth, 1000, 1e-3);
  }
} // namespace

int main() {
  everyColumnFitsAWideList();
  columnsLeaveInOrder();
  moreColumnsLeaveFirst();
  choicesAreKept();
  headerFollowsTheMode();
  leftOutColumns();
  degenerateInputs();
  std::println("workbench list columns checks passed");
  return 0;
}
