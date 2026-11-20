#include "formats/table_grid.h"
#include "test_check.h"

#include <cstddef>
#include <print>
#include <string>
#include <string_view>

namespace {
  using namespace luciole::formats;

  void letters() {
    TEST_CHECK(columnLetters(0) == "A");
    TEST_CHECK(columnLetters(25) == "Z");
    TEST_CHECK(columnLetters(26) == "AA");
    TEST_CHECK(columnLetters(51) == "AZ");
    TEST_CHECK(columnLetters(52) == "BA");
    TEST_CHECK(columnLetters(701) == "ZZ");
    TEST_CHECK(columnLetters(702) == "AAA");
    TEST_CHECK(columnLetters(16'383) == "XFD");
  }

  void oneLineText() {
    TEST_CHECK(gridText("plain") == "plain");
    TEST_CHECK(gridText("two\nlines\r\tand tab") == "two lines  and tab");
    TEST_CHECK(gridText(std::string_view("nul\0bell\a", 9)) == "nul bell ");
    TEST_CHECK(gridText("del\x7F") == "del ");
    TEST_CHECK(gridText("next\xC2\x85line") == "next line");
    // Invalid bytes, an overlong form, a surrogate and a truncated sequence each become U+FFFD.
    TEST_CHECK(
        gridText(
            "a\xFF"
            "b"
        )
        == "a\xEF\xBF\xBD"
           "b"
    );
    TEST_CHECK(gridText("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");
    TEST_CHECK(gridText("\xED\xA0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    TEST_CHECK(gridText("ok\xE2\x82") == "ok\xEF\xBF\xBD\xEF\xBF\xBD");
    TEST_CHECK(gridText("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80") == "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    // Long text is cut on a code point boundary.
    std::string euros;
    for (int i = 0; i < 200; ++i) {
      euros += "\xE2\x82\xAC";
    }
    const std::string cut = gridText(euros);
    TEST_CHECK(cut.size() == 255);
    TEST_CHECK(cut.size() <= kMaxGridCellBytes);
    TEST_CHECK(cut.ends_with("\xE2\x82\xAC"));
    TEST_CHECK(gridText(std::string(1000, 'x')).size() == kMaxGridCellBytes);
    TEST_CHECK(gridText("").empty());
  }

  void buildsRowMajor() {
    TableGridBuilder builder(3, 2);
    TEST_CHECK(builder.set(0, 1, GridCellKind::Number, "12"));
    TEST_CHECK(builder.set(1, 0, GridCellKind::Text, "b\nc"));
    TEST_CHECK(builder.set(2, 1, GridCellKind::Boolean, "TRUE"));
    const TableGrid grid = builder.finish();
    TEST_CHECK(grid.rowCount == 3);
    TEST_CHECK(grid.columnCount == 2);
    TEST_CHECK(grid.ends.size() == 6);
    TEST_CHECK(grid.kinds.size() == 6);
    TEST_CHECK(gridCellText(grid, 0, 0).empty());
    TEST_CHECK(gridCellKind(grid, 0, 0) == GridCellKind::Text);
    TEST_CHECK(gridCellText(grid, 0, 1) == "12");
    TEST_CHECK(gridCellKind(grid, 0, 1) == GridCellKind::Number);
    TEST_CHECK(gridCellText(grid, 1, 0) == "b c");
    TEST_CHECK(gridCellText(grid, 1, 1).empty());
    TEST_CHECK(gridCellText(grid, 2, 1) == "TRUE");
    TEST_CHECK(gridCellKind(grid, 2, 1) == GridCellKind::Boolean);
    TEST_CHECK(!grid.truncated && !grid.rowsCut && !grid.columnsCut);
    // Out of range reads are empty Text.
    TEST_CHECK(gridCellText(grid, 3, 0).empty());
    TEST_CHECK(gridCellText(grid, 0, 2).empty());
    TEST_CHECK(gridCellKind(grid, 9, 9) == GridCellKind::Text);
  }

  void refusesDisorder() {
    TableGridBuilder builder(2, 2);
    TEST_CHECK(builder.set(1, 0, GridCellKind::Text, "x"));
    TEST_CHECK(!builder.set(0, 1, GridCellKind::Text, "y"));
    TEST_CHECK(!builder.set(1, 0, GridCellKind::Text, "again"));
    TEST_CHECK(!builder.set(2, 0, GridCellKind::Text, "below"));
    TEST_CHECK(!builder.set(1, 2, GridCellKind::Text, "beside"));
    const TableGrid grid = builder.finish();
    TEST_CHECK(gridCellText(grid, 1, 0) == "x");
    TEST_CHECK(gridCellText(grid, 0, 1).empty());
  }

  void clampsToLimits() {
    // Columns past the limit are left out and the cells there are passed over.
    TableGridBuilder wide(2, kMaxGridColumns + 10);
    TEST_CHECK(wide.columnCount() == kMaxGridColumns);
    TEST_CHECK(wide.set(0, kMaxGridColumns - 1, GridCellKind::Text, "last"));
    TEST_CHECK(wide.set(0, kMaxGridColumns + 2, GridCellKind::Text, "gone"));
    TEST_CHECK(wide.set(1, 0, GridCellKind::Text, "next"));
    const TableGrid wideGrid = wide.finish();
    TEST_CHECK(wideGrid.truncated && wideGrid.columnsCut && !wideGrid.rowsCut);
    TEST_CHECK(gridCellText(wideGrid, 0, kMaxGridColumns - 1) == "last");
    TEST_CHECK(gridCellText(wideGrid, 1, 0) == "next");

    // Rows go first when rows times columns passes the cell limit.
    TableGridBuilder tall(kMaxGridRows, kMaxGridColumns);
    TEST_CHECK(tall.rowCount() == kMaxGridCells / kMaxGridColumns);
    TEST_CHECK(tall.set(tall.rowCount(), 0, GridCellKind::Text, "past"));
    const TableGrid tallGrid = tall.finish();
    TEST_CHECK(tallGrid.truncated && tallGrid.rowsCut && !tallGrid.columnsCut);

    TableGridBuilder many(kMaxGridRows + 1, 1);
    TEST_CHECK(many.rowCount() == kMaxGridRows);
    const TableGrid manyGrid = many.finish();
    TEST_CHECK(manyGrid.rowsCut && !manyGrid.columnsCut);

    TableGridBuilder none(0, 0);
    const TableGrid empty = none.finish();
    TEST_CHECK(empty.rowCount == 0 && empty.columnCount == 0 && empty.ends.empty());
    TEST_CHECK(!empty.truncated && !empty.rowsCut && !empty.columnsCut);
  }

  void textBudget() {
    // Full rows of full cells: 256 of them fill the budget, so the next row does not fit and ends the grid there.
    const std::size_t columns = kMaxGridColumns;
    const std::size_t fit = kMaxGridTextBytes / (columns * kMaxGridCellBytes);
    TEST_CHECK(fit == 256);
    TableGridBuilder builder(fit + 10, columns);
    const std::string cell(kMaxGridCellBytes, 'x');
    for (std::size_t row = 0; row < fit + 10; ++row) {
      for (std::size_t column = 0; column < columns; ++column) {
        TEST_CHECK(builder.set(row, column, GridCellKind::Text, cell));
      }
    }
    const TableGrid grid = builder.finish();
    TEST_CHECK(grid.truncated && grid.rowsCut && !grid.columnsCut);
    TEST_CHECK(grid.rowCount == fit);
    TEST_CHECK(grid.ends.size() == fit * columns);
    TEST_CHECK(grid.kinds.size() == fit * columns);
    TEST_CHECK(grid.text.size() == kMaxGridTextBytes);
    TEST_CHECK(gridCellText(grid, fit - 1, columns - 1) == cell);
    TEST_CHECK(gridCellText(grid, fit, 0).empty());
  }

  void columnNamesAndNumbers() {
    TableGridBuilder builder(1, 3);
    TableGrid grid = builder.finish();
    grid.firstSourceColumn = 26;
    TEST_CHECK(gridColumnName(grid, 0) == "AA");
    TEST_CHECK(gridColumnName(grid, 2) == "AC");
    grid.columnNames = {"Name", "Size"};
    TEST_CHECK(gridColumnName(grid, 0) == "Name");
    TEST_CHECK(gridColumnName(grid, 1) == "Size");
    // A header shorter than the grid names the rest by letters, and so does an empty name.
    TEST_CHECK(gridColumnName(grid, 2) == "AC");
    grid.columnNames = {"", "Size"};
    TEST_CHECK(gridColumnName(grid, 0) == "AA");
  }
} // namespace

int main() {
  letters();
  oneLineText();
  buildsRowMajor();
  refusesDisorder();
  clampsToLimits();
  textBudget();
  columnNamesAndNumbers();
  std::println("formats-table-grid: all checks passed");
  return 0;
}
