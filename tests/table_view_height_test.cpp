#include "formats/table_grid.h"
#include "render/scene/node.h"
#include "table_view_rig.h"
#include "test_check.h"
#include "ui/controls/scrollbar.h"
#include "ui/controls/segmented.h"
#include "ui/controls/table_view.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <string>

// The height a table view takes at a width, which a card around it hugs: the rows and the header, the notice, the
// sheet list and a sideways bar when the table is wider than the view. The view is laid out at that height and at
// shorter ones, and the height is asked for again after each layout.
namespace {
  using namespace luciole;
  using namespace luciole::test::tableview;

  constexpr float kBar = Style::scrollbarWidth;

  std::shared_ptr<const TableGrid>
  sized(std::size_t rows, std::size_t columns, const std::function<void(TableGrid&)>& adjust = {}) {
    return grid(
        rows, columns, [](std::size_t row, std::size_t column) { return std::format("r{}c{}", row, column); }, adjust
    );
  }

  // The view laid out at `width` by `height` the way a page does it, until nothing waits.
  void lay(Rig& rig, float width, float height) {
    for (int pass = 0; pass < 8; ++pass) {
      rig.frames = 0;
      rig.view->update();
      rig.view->arrange(rig.renderer, {.x = 0, .y = 0, .width = width, .height = height});
      if (rig.frames == 0) {
        return;
      }
    }
    TEST_CHECK(rig.frames == 0);
  }

  float natural(Rig& rig, float width) {
    const auto height = rig.view->naturalHeight(rig.renderer, width);
    TEST_CHECK(height.has_value());
    return height.value_or(0);
  }

  struct Bars {
    int horizontal = 0;
    int vertical = 0;
  };

  // The scrollbars of the grid that show.
  Bars barsOf(const Rig& rig) {
    Bars bars;
    for (const auto& child : rig.view->canvas().children()) {
      const auto* bar = dynamic_cast<const Scrollbar*>(child.get());
      if (bar != nullptr && bar->visible()) {
        ++(bar->orientation() == ScrollOrientation::Horizontal ? bars.horizontal : bars.vertical);
      }
    }
    return bars;
  }

  // The heights of the view's parts that take part in the layout, the grid apart.
  float partsAbove(const Rig& rig) {
    float total = 0;
    for (const auto& child : rig.view->children()) {
      if (child->participatesInLayout() && child.get() != &rig.view->canvas()) {
        total += child->height();
      }
    }
    return total;
  }

  void rowsAndHeaderOnly() {
    Rig rig;
    rig.show(sized(4, 3));
    lay(rig, kWidth, kHeight);
    // A header row and four rows of kRow.
    TEST_NEAR(natural(rig, kWidth), 5 * kRow, 0.01F);
    lay(rig, kWidth, natural(rig, kWidth));
    TEST_NEAR(rig.view->canvas().height(), 5 * kRow, 0.01F);
    const auto bars = barsOf(rig);
    TEST_CHECK(bars.horizontal == 0 && bars.vertical == 0);
    rig.view->select({.row = 3, .column = 2});
    rig.settle();
    TEST_CHECK(rig.view->scrollY() == 0 && rig.view->scrollX() == 0);
  }

  void wideTableKeepsItsSidewaysBar() {
    Rig rig;
    rig.show(sized(4, 30));
    lay(rig, kWidth, kHeight);
    const float wanted = natural(rig, kWidth);
    TEST_NEAR(wanted, (5 * kRow) + kBar, 0.01F);
    lay(rig, kWidth, wanted);
    const auto bars = barsOf(rig);
    TEST_CHECK(bars.horizontal == 1 && bars.vertical == 0);
    rig.view->select({.row = 3, .column = 29});
    rig.settle();
    lay(rig, kWidth, wanted);
    TEST_CHECK(rig.view->scrollX() > 0);
    TEST_CHECK(rig.view->scrollY() == 0);
    TEST_CHECK(rig.view->selectedCell() && rig.view->canvas().cellInView(*rig.view->selectedCell()));
    // The last row clears the bar and nothing needs the vertical one.
    TEST_CHECK(barsOf(rig).vertical == 0);
  }

  void tallTableFillsTheRoomAndScrolls() {
    Rig rig;
    rig.show(sized(300, 3));
    lay(rig, kWidth, kHeight);
    TEST_CHECK(natural(rig, kWidth) >= 301 * kRow);
    TEST_NEAR(rig.view->canvas().height(), kHeight, 0.01F);
    const auto bars = barsOf(rig);
    TEST_CHECK(bars.vertical == 1 && bars.horizontal == 0);
    rig.view->select({.row = 299, .column = 0});
    rig.settle();
    lay(rig, kWidth, kHeight);
    // The grid's own height of kHeight less the header holds the rows; the last row ends at the bottom.
    TEST_NEAR(rig.view->scrollY(), (300 * kRow) - (kHeight - kRow), 0.5F);
  }

  void noticeAndSheetsAreCounted() {
    {
      Rig rig;
      rig.show(sized(4, 3, [](TableGrid& table) {
        table.rowsCut = true;
        table.truncated = true;
      }));
      lay(rig, kWidth, kHeight);
      const float wanted = natural(rig, kWidth);
      lay(rig, kWidth, wanted);
      TEST_CHECK(!rig.view->notice().empty());
      TEST_CHECK(partsAbove(rig) > 0);
      TEST_NEAR(rig.view->canvas().height(), 5 * kRow, 0.01F);
      TEST_NEAR(wanted, partsAbove(rig) + (5 * kRow), 0.01F);
    }
    {
      Rig rig;
      rig.show(sized(4, 3, [](TableGrid& table) { table.sheetNames = {"First", "Second"}; }));
      lay(rig, kWidth, kHeight);
      const float wanted = natural(rig, kWidth);
      lay(rig, kWidth, wanted);
      TEST_CHECK(rig.view->sheetList().height() > 0);
      TEST_CHECK(partsAbove(rig) >= rig.view->sheetList().height());
      TEST_NEAR(rig.view->canvas().height(), 5 * kRow, 0.01F);
      TEST_NEAR(wanted, partsAbove(rig) + (5 * kRow), 0.01F);
    }
  }

  // A sentence in place of the grid: the card holds the sentence and the sheet list only.
  void messageHidesTheGrid() {
    Rig rig;
    rig.show(sized(4, 3));
    rig.view->setMessage("This sheet could not be read.");
    lay(rig, kWidth, kHeight);
    const float wanted = natural(rig, kWidth);
    lay(rig, kWidth, wanted);
    TEST_CHECK(rig.view->notice() == "This sheet could not be read.");
    TEST_CHECK(wanted > 0 && wanted < 2 * kRow);
    TEST_NEAR(wanted, partsAbove(rig), 0.01F);
  }

  void editingAndNothingLoadedFillTheRoom() {
    Rig rig;
    TEST_CHECK(!rig.view->naturalHeight(rig.renderer, kWidth).has_value());
    rig.show(sized(4, 3));
    lay(rig, kWidth, kHeight);
    TEST_CHECK(rig.view->naturalHeight(rig.renderer, kWidth).has_value());
    rig.view->setEditable(true);
    lay(rig, kWidth, kHeight);
    TEST_CHECK(!rig.view->naturalHeight(rig.renderer, kWidth).has_value());
    rig.view->setEditable(false);
    lay(rig, kWidth, kHeight);
    TEST_CHECK(rig.view->naturalHeight(rig.renderer, kWidth).has_value());
  }

  // The height may follow the width, never the height it is laid out at: laid out at the height it asked for, or at
  // a shorter one, it asks for the same height again, and the grid is the same size after a second layout. Widths
  // are swept across the one where the table stops needing the sideways bar, with the room above and below the
  // table's own height.
  void heightIsStable() {
    for (const std::size_t rows : {3UL, 40UL}) {
      Rig rig;
      rig.show(sized(rows, 14));
      lay(rig, kWidth, kHeight);
      int flips = 0;
      std::optional<bool> sideways;
      for (float width = 520; width <= 700; width += 1) {
        for (const float room : {60.0F, 150.0F, 400.0F}) {
          const float first = natural(rig, width);
          const float at = std::min(first, room);
          lay(rig, width, at);
          const float second = natural(rig, width);
          const float canvas = rig.view->canvas().height();
          lay(rig, width, at);
          TEST_NEAR(first, second, 0.01F);
          TEST_NEAR(natural(rig, width), first, 0.01F);
          TEST_NEAR(rig.view->canvas().height(), canvas, 0.01F);
          // Hugging leaves no vertical bar, and a table that fits the room keeps its rows in sight.
          if (first <= room) {
            TEST_CHECK(barsOf(rig).vertical == 0);
          }
        }
        lay(rig, width, kHeight);
        const bool now = barsOf(rig).horizontal == 1;
        flips += sideways && *sideways != now ? 1 : 0;
        sideways = now;
      }
      std::println("{} rows: the sideways bar changed {} time(s) over widths 520 to 700", rows, flips);
      TEST_CHECK(flips == 1);
    }
  }
} // namespace

int main() {
  rowsAndHeaderOnly();
  wideTableKeepsItsSidewaysBar();
  tallTableFillsTheRoomAndScrolls();
  noticeAndSheetsAreCounted();
  messageHidesTheGrid();
  editingAndNothingLoadedFillTheRoom();
  heightIsStable();
  std::println("table-view-height: all checks passed");
  return 0;
}
