#include "core/input/key_modifiers.h"
#include "cpu_budget.h"
#include "formats/table_grid.h"
#include "machine_slowdown.h"
#include "table_view_rig.h"
#include "test_check.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/table_view.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  using namespace luciole;
  using namespace luciole::test::tableview;

  // A small table read from delimited text: a header, text, numbers and booleans.
  std::shared_ptr<const TableGrid> people() {
    formats::TableGridBuilder builder(3, 3);
    const std::vector<std::vector<std::pair<GridCellKind, std::string>>> rows{
        {{GridCellKind::Text, "alpha"}, {GridCellKind::Number, "12"}, {GridCellKind::Boolean, "TRUE"}},
        {{GridCellKind::Text, "beta"}, {GridCellKind::Number, "7.5"}, {GridCellKind::Boolean, "FALSE"}},
        {{GridCellKind::Text, "gamma"}, {GridCellKind::Date, "2024-01-02"}, {GridCellKind::Error, "#N/A"}},
    };
    for (std::size_t row = 0; row < rows.size(); ++row) {
      for (std::size_t column = 0; column < rows[row].size(); ++column) {
        TEST_CHECK(builder.set(row, column, rows[row][column].first, rows[row][column].second));
      }
    }
    auto table = builder.finish();
    table.columnNames = {"Name", "Size", "Ok"};
    table.firstSourceRow = 1;
    return std::make_shared<const TableGrid>(std::move(table));
  }

  void showsCells() {
    Rig rig;
    std::vector<TableCell> reported;
    rig.view->onSelectionChanged = [&](TableCell source) { reported.push_back(source); };
    rig.show(people());
    const auto* name = labelWith(rig.root, "Name");
    const auto* alpha = labelWith(rig.root, "alpha");
    const auto* twelve = labelWith(rig.root, "12");
    const auto* yes = labelWith(rig.root, "TRUE");
    TEST_CHECK(name != nullptr && labelWith(rig.root, "Size") != nullptr && labelWith(rig.root, "Ok") != nullptr);
    TEST_CHECK(alpha != nullptr && twelve != nullptr && yes != nullptr);
    TEST_CHECK(alpha->textAlign() == TextAlign::Start);
    TEST_CHECK(twelve->textAlign() == TextAlign::End);
    TEST_CHECK(labelWith(rig.root, "2024-01-02")->textAlign() == TextAlign::End);
    TEST_CHECK(yes->textAlign() == TextAlign::Center);
    TEST_CHECK(labelWith(rig.root, "#N/A")->textAlign() == TextAlign::Center);
    // Row numbers count the rows below the header from one.
    TEST_CHECK(labelWith(rig.root, "1") != nullptr && labelWith(rig.root, "3") != nullptr);
    TEST_CHECK(labelWith(rig.root, "4") == nullptr);
    // Cells of one row share a line; the header is above them and the numbers to their left.
    TEST_NEAR(center(*alpha).y, center(*twelve).y, 0.5F);
    TEST_CHECK(center(*twelve).x > center(*alpha).x);
    TEST_CHECK(center(*name).y < center(*alpha).y);
    TEST_CHECK(center(*labelWith(rig.root, "1")).x < center(*alpha).x);
    TEST_NEAR(center(*labelWith(rig.root, "1")).y, center(*alpha).y, 0.5F);
    TEST_NEAR(center(*labelWith(rig.root, "beta")).y - center(*alpha).y, kRow, 0.5F);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 0, .column = 0}));
    TEST_CHECK(rig.view->selectedSource() == (TableCell{.row = 1, .column = 0}));
    TEST_CHECK(reported.size() == 1 && reported.front() == (TableCell{.row = 1, .column = 0}));
    TEST_CHECK(rig.view->notice().empty());
    // A header that is not there names columns by letters.
    rig.show(grid(1, 3, [](std::size_t, std::size_t) { return "x"; }));
    TEST_CHECK(labelWith(rig.root, "A") != nullptr && labelWith(rig.root, "C") != nullptr);
    TEST_CHECK(labelWith(rig.root, "Name") == nullptr);
  }

  void sizesColumns() {
    Rig rig;
    const std::string longText(formats::kMaxGridCellBytes, 'w');
    rig.show(grid(3, 3, [&](std::size_t row, std::size_t column) {
      return column == 0 ? std::string("a") : column == 1 ? (row == 2 ? longText : "b") : std::string(20, 'm');
    }));
    // A short column keeps the least width, a long one the most, and one between fits its widest text.
    TEST_NEAR(labelWith(rig.root, "a")->width(), 12 * 3.5F - (2 * kPad), 1);
    TEST_NEAR(labelWith(rig.root, longText)->width(), 12 * 22.0F - (2 * kPad), 1);
    TEST_NEAR(labelWith(rig.root, std::string(20, 'm'))->width(), (20 * 6.0F) + 3, 1);
    // The sample reaches the last row of a long table.
    rig.show(grid(5000, 1, [](std::size_t row, std::size_t) {
      return row == 4999 ? std::string(20, 'm') : std::string("x");
    }));
    TEST_NEAR(labelWith(rig.root, "x")->width(), (20 * 6.0F) + 3, 1);
  }

  void virtualizes() {
    const auto cell = [](std::size_t, std::size_t) { return std::string("cell"); };
    Rig small;
    small.show(grid(40, 20, cell));
    Rig big;
    big.show(grid(5000, 50, cell));
    const std::size_t smallLabels = small.view->canvas().labelCount();
    const std::size_t bigLabels = big.view->canvas().labelCount();
    std::println("labels: {} for 40 x 20, {} for 5,000 x 50", smallLabels, bigLabels);
    std::println("nodes: {} for 40 x 20, {} for 5,000 x 50", nodes(small.root), nodes(big.root));
    TEST_CHECK(bigLabels == smallLabels);
    TEST_CHECK(nodes(big.root) == nodes(small.root));
    // The viewport holds 19 rows of 14 columns: cells, column names and row numbers.
    TEST_CHECK(bigLabels <= (19 * 15) + 15 + 19);
    const std::size_t before = nodes(big.root);
    for (int step = 0; step < 40; ++step) {
      big.wheel(step % 4 == 3 ? 1 : 0, 2750.5);
      TEST_CHECK(big.view->canvas().labelCount() <= (20 * 16) + 16 + 20);
    }
    std::println("nodes after scrolling: {}", nodes(big.root));
    TEST_CHECK(nodes(big.root) <= before + (2 * (16 + 20 + 16)));
    TEST_CHECK(big.view->scrollY() > 0 && big.view->scrollX() > 0);
  }

  void scrolls() {
    Rig rig;
    rig.show(grid(5000, 50, [](std::size_t row, std::size_t column) { return std::format("r{}c{}", row, column); }));
    rig.wheel(0, 30);
    TEST_NEAR(rig.view->scrollY(), 30, 0.01F);
    rig.wheel(1, 50);
    TEST_NEAR(rig.view->scrollX(), 50, 0.01F);
    TEST_CHECK(labelWith(rig.root, "r0c0") == nullptr);
    TEST_CHECK(labelWith(rig.root, "r1c1") != nullptr);
    // Far past the end stops where the last row and column clear the scrollbars.
    rig.wheel(0, 1.0E9);
    const float bodyHeight = rig.view->canvas().bodyHeight();
    TEST_NEAR(rig.view->scrollY(), (5000 * kRow) + Style::scrollbarWidth - bodyHeight, 0.5F);
    TEST_CHECK(labelWith(rig.root, "5,000") != nullptr);
    TEST_CHECK(labelWith(rig.root, "1") == nullptr);
    rig.wheel(0, -1.0E9);
    TEST_NEAR(rig.view->scrollY(), 0, 0.01F);
    TEST_CHECK(labelWith(rig.root, "1") != nullptr);
    // A wheel turn over a table that fits is left to the views around it.
    Rig fits;
    fits.show(people());
    const Point at = center(fits.view->canvas());
    fits.dispatcher.pointerMotion(at.x, at.y, ++fits.serial);
    TEST_CHECK(!fits.dispatcher.pointerAxis(at.x, at.y, 0, 0, 30, 0, 0, 0));
    TEST_NEAR(fits.view->scrollY(), 0, 0.01F);
  }

  void movesSelection() {
    Rig rig;
    std::vector<TableCell> reported;
    rig.view->onSelectionChanged = [&](TableCell source) { reported.push_back(source); };
    rig.show(grid(
        300, 30, [](std::size_t row, std::size_t column) { return std::format("r{}c{}", row, column); },
        [](TableGrid& table) {
          table.firstSourceRow = 4;
          table.firstSourceColumn = 2;
        }
    ));
    rig.press(XKB_KEY_Up);
    rig.press(XKB_KEY_Left);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 0, .column = 0}));
    rig.press(XKB_KEY_Down);
    rig.press(XKB_KEY_Right);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 1, .column = 1}));
    TEST_CHECK(reported.back() == (TableCell{.row = 5, .column = 3}));
    rig.press(XKB_KEY_End, KeyMod::Ctrl);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 299, .column = 29}));
    TEST_CHECK(rig.view->selectedSource() == (TableCell{.row = 303, .column = 31}));
    // The last cell is in view, clear of the scrollbars.
    const auto& canvas = rig.view->canvas();
    TEST_NEAR(rig.view->scrollY() + canvas.bodyHeight() - Style::scrollbarWidth, 300 * kRow, 0.5F);
    TEST_CHECK(labelWith(rig.root, "r299c29") != nullptr);
    rig.press(XKB_KEY_Home);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 299, .column = 0}));
    TEST_NEAR(rig.view->scrollX(), 0, 0.01F);
    rig.press(XKB_KEY_Home, KeyMod::Ctrl);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 0, .column = 0}));
    TEST_NEAR(rig.view->scrollY(), 0, 0.01F);
    rig.press(XKB_KEY_Page_Down);
    const auto page = static_cast<std::size_t>(std::floor(canvas.bodyHeight() / kRow) - 1);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = page, .column = 0}));
    rig.press(XKB_KEY_Page_Up);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 0, .column = 0}));
    const std::size_t before = reported.size();
    rig.press(XKB_KEY_a);
    TEST_CHECK(reported.size() == before);
    // A click selects the cell under it.
    rig.click(center(*labelWith(rig.root, "r3c2")));
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 3, .column = 2}));
    TEST_CHECK(reported.back() == (TableCell{.row = 7, .column = 4}));
    // Clicks on the header and the row numbers select nothing new.
    rig.click(center(*labelWith(rig.root, "E")));
    rig.click(center(*labelWith(rig.root, "2")));
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 3, .column = 2}));
    rig.view->select({.row = 10'000, .column = 10'000});
    rig.settle();
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 299, .column = 29}));
  }

  // A press between a new table and its first frame lands on no cell, though the old table showed one there.
  void ignoresPressBeforeNewFrame() {
    const auto named = [](std::size_t rows, std::size_t columns) {
      return grid(rows, columns, [](std::size_t row, std::size_t column) {
        return std::format("r{}c{}", row, column);
      });
    };
    for (const bool message : {false, true}) {
      Rig rig;
      rig.show(named(100, 5));
      const Point at = center(*labelWith(rig.root, "r8c2"));
      if (message) {
        rig.view->setMessage("This spreadsheet could not be read.");
      } else {
        rig.view->setTable(named(2, 2));
      }
      rig.click(at);
      TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 0, .column = 0}));
    }
  }

  void switchesSheets() {
    Rig rig;
    std::vector<std::size_t> chosen;
    rig.view->onSheetChosen = [&](std::size_t sheet) { chosen.push_back(sheet); };
    const auto sheets = [](std::vector<std::string> names, std::size_t index) {
      return grid(
          2, 2, [](std::size_t, std::size_t) { return "v"; },
          [&](TableGrid& table) {
            table.sheetNames = names;
            table.sheetIndex = index;
          }
      );
    };
    rig.show(sheets({"One", "Two", "Three"}, 1));
    TEST_CHECK(shown(rig.view->sheetList()));
    TEST_CHECK(rig.view->sheetList().selectedIndex() == 1);
    TEST_CHECK(chosen.empty());
    const auto* three = labelWith(rig.view->sheetList(), "Three");
    TEST_CHECK(three != nullptr && labelWith(rig.view->sheetList(), "One") != nullptr);
    // The grid sits below the sheet list.
    TEST_CHECK(center(rig.view->canvas()).y > center(*three).y);
    rig.click(center(*three));
    TEST_CHECK((chosen == std::vector<std::size_t>{2}));
    // A sheet that fails to load keeps the list, with the chosen sheet marked, so another can be picked.
    rig.view->setMessage("This spreadsheet could not be read.");
    rig.settle();
    TEST_CHECK(shown(rig.view->sheetList()) && rig.view->sheetList().selectedIndex() == 2);
    TEST_CHECK(rig.view->notice() == "This spreadsheet could not be read." && !shown(rig.view->canvas()));
    rig.click(center(*labelWith(rig.view->sheetList(), "One")));
    TEST_CHECK((chosen == std::vector<std::size_t>{2, 0}));
    rig.view->setMessage({});
    rig.show(sheets({"One", "Two", "Three"}, 1));
    TEST_CHECK(rig.view->sheetList().selectedIndex() == 1 && shown(rig.view->canvas()));
    rig.show(sheets({"Only"}, 0));
    TEST_CHECK(!shown(rig.view->sheetList()));
    rig.show(sheets({"A", "B"}, 0));
    TEST_CHECK(shown(rig.view->sheetList()) && rig.view->sheetList().selectedIndex() == 0);
    TEST_CHECK((chosen == std::vector<std::size_t>{2, 0}));
  }

  void showsNotices() {
    Rig rig;
    const auto cut = [](std::size_t rows, std::size_t columns, bool rowsCut, bool columnsCut) {
      return grid(
          rows, columns, [](std::size_t, std::size_t) { return "v"; },
          [&](TableGrid& table) {
            table.truncated = true;
            table.rowsCut = rowsCut;
            table.columnsCut = columnsCut;
          }
      );
    };
    rig.show(cut(3, 2, true, true));
    TEST_CHECK(rig.view->notice() == "Showing the first 3 rows and 2 columns");
    TEST_CHECK(labelWith(rig.root, "Showing the first 3 rows and 2 columns") != nullptr);
    rig.show(cut(1, 1, true, true));
    TEST_CHECK(rig.view->notice() == "Showing the first 1 row and 1 column");
    // Only the dimension that was cut is named.
    rig.show(cut(5000, 2, true, false));
    TEST_CHECK(rig.view->notice() == "Showing the first 5,000 rows");
    rig.show(cut(4, 256, false, true));
    TEST_CHECK(rig.view->notice() == "Showing the first 256 columns");
    rig.show(cut(4, 3, false, false));
    TEST_CHECK(rig.view->notice() == "Some of this table is not shown.");
    rig.show(grid(0, 0, [](std::size_t, std::size_t) { return ""; }));
    TEST_CHECK(rig.view->notice() == "This table is empty.");
    TEST_CHECK(!rig.view->selectedCell());
    rig.view->setMessage("This spreadsheet could not be read.");
    rig.settle();
    TEST_CHECK(rig.view->notice() == "This spreadsheet could not be read.");
    TEST_CHECK(!shown(rig.view->canvas()));
    rig.view->setMessage({});
    rig.show(people());
    TEST_CHECK(rig.view->notice().empty() && shown(rig.view->canvas()));
  }

  void followsTextSize() {
    Rig rig;
    rig.show(people());
    rig.view->setTextSize(20);
    rig.settle();
    TEST_CHECK(rig.view->textSize() == 20);
    const auto* alpha = labelWith(rig.root, "alpha");
    const auto* beta = labelWith(rig.root, "beta");
    TEST_CHECK(alpha->fontSize() == 20 && labelWith(rig.root, "Name")->fontSize() == 20);
    TEST_NEAR(center(*beta).y - center(*alpha).y, 36, 0.5F);
    rig.view->setTextSize(std::numeric_limits<float>::quiet_NaN());
    rig.view->setTextSize(std::numeric_limits<float>::infinity());
    TEST_CHECK(rig.view->textSize() == 20);
    rig.view->setTextSize(1000);
    TEST_CHECK(rig.view->textSize() == 96);
    rig.view->setTextSize(1);
    TEST_CHECK(rig.view->textSize() == 6);
  }

  // A wider font at the same text size widens the columns.
  void followsFontChange() {
    Rig rig;
    rig.show(grid(2, 2, [](std::size_t, std::size_t column) { return std::string(column == 0 ? 20 : 2, 'm'); }));
    TEST_NEAR(labelWith(rig.root, std::string(20, 'm'))->width(), (20 * 6.0F) + 3, 1);
    rig.renderer.advance = 0.75F;
    ++rig.renderer.generation;
    rig.settle();
    TEST_NEAR(labelWith(rig.root, std::string(20, 'm'))->width(), (20 * 9.0F) + 3, 1);
  }

  // Column widths of the widest table come from a bounded sample, so planning stays short on the main context.
  void plansWideTablesQuickly() {
    Rig rig;
    rig.show(people());
    std::string text(43, 'm');
    const auto wide = grid(1500, formats::kMaxGridColumns, [&](std::size_t, std::size_t) { return text; });
    double least = 1.0E9;
    for (int round = 0; round < 5; ++round) {
      const double slowdown = luciole::test::machineSlowdown();
      rig.view->setTable(wide);
      const double start = threadMs();
      rig.view->update();
      least = std::min(least, (threadMs() - start) / slowdown);
      rig.settle();
    }
    std::println("update after a 1,500 x 256 table: {:.2f} ms of processor time", least);
    TEST_CHECK(::test::withinBudget(least, 2.5));
  }

  void showsNothingWithoutTable() {
    Rig rig;
    rig.show(people());
    rig.show(nullptr);
    TEST_CHECK(labels(rig.view->canvas()).empty());
    TEST_CHECK(!rig.view->selectedCell() && !rig.view->selectedSource());
    TEST_CHECK(rig.view->notice().empty());
    rig.press(XKB_KEY_Down);
    rig.wheel(0, 100);
    TEST_NEAR(rig.view->scrollY(), 0, 0.01F);
  }

  // A new text size keeps the cell at the top left there: the grid's offsets land on that cell's edges again.
  void textSizeKeepsTheTopCell() {
    const auto name = [](std::size_t row, std::size_t column) { return std::format("r{:03}c{:02}", row, column); };
    Rig rig;
    rig.show(grid(300, 30, name));
    rig.wheel(0, 2000.5);
    rig.wheel(1, 700.25);
    // Every cell has text of one width, so the distance between neighbors is the row height and the column width.
    const auto pitch = [&] -> Point {
      for (std::size_t row = 0; row + 1 < 300; ++row) {
        for (std::size_t column = 0; column + 1 < 30; ++column) {
          const auto* here = labelWith(rig.root, name(row, column));
          const auto* right = labelWith(rig.root, name(row, column + 1));
          const auto* below = labelWith(rig.root, name(row + 1, column));
          if (here != nullptr && right != nullptr && below != nullptr) {
            return {.x = center(*right).x - center(*here).x, .y = center(*below).y - center(*here).y};
          }
        }
      }
      TEST_CHECK(false);
      return {};
    };
    auto step = pitch();
    const auto row = static_cast<std::size_t>(rig.view->scrollY() / step.y);
    const auto column = static_cast<std::size_t>(rig.view->scrollX() / step.x);
    TEST_CHECK(row > 20 && column > 2);
    for (const float size : {8.4F, 12.0F, 18.0F, 36.0F}) {
      rig.view->setTextSize(size);
      rig.settle();
      TEST_NEAR(rig.view->textSize(), size, 0.0F);
      const auto* cell = labelWith(rig.root, name(row, column));
      TEST_CHECK(cell != nullptr && cell->fontSize() == size);
      step = pitch();
      std::println("table at {}: cells {:.1f} by {:.1f}, top left r{}c{}", size, step.x, step.y, row, column);
      TEST_NEAR(rig.view->scrollY(), static_cast<float>(row) * step.y, 0.5F);
      TEST_NEAR(rig.view->scrollX() / step.x, static_cast<float>(column), 0.02F);
    }
  }
} // namespace

int main() {
  showsCells();
  sizesColumns();
  virtualizes();
  scrolls();
  movesSelection();
  ignoresPressBeforeNewFrame();
  switchesSheets();
  showsNotices();
  followsTextSize();
  textSizeKeepsTheTopCell();
  followsFontChange();
  plansWideTablesQuickly();
  showsNothingWithoutTable();
  std::println("table-view: all checks passed");
  return 0;
}
