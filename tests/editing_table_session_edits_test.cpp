#include "editing/table_session.h"
#include "test_check.h"

#include <cstddef>
#include <expected>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::editing;
  using luciole::formats::cellText;
  using luciole::formats::Delimiter;
  using luciole::formats::Dialect;
  using luciole::formats::rowCells;
  using luciole::formats::RowEnding;

  constexpr std::string_view kMark = "\xEF\xBB\xBF";
  const Dialect kComma{};
  const Dialect kTabCrLf{.delimiter = Delimiter::Tab, .lineEnding = RowEnding::CrLf};
  const Dialect kSemicolonMark{
      .delimiter = Delimiter::Semicolon, .lineEnding = RowEnding::CrLf, .byteOrderMark = true, .header = true
  };

  TableSession opened(std::string_view bytes, const Dialect& dialect) {
    auto session = TableSession::open(std::string(bytes), dialect);
    TEST_CHECK(session.has_value());
    return std::move(*session);
  }

  void accepted(const std::expected<void, TableError>& result) {
    if (!result) {
      std::println(stderr, "refused: {}", result.error().message);
    }
    TEST_CHECK(result.has_value());
  }

  TableRefusal refused(const std::expected<void, TableError>& result) {
    TEST_CHECK(!result.has_value());
    TEST_CHECK(!result.error().message.empty() && result.error().message.ends_with('.'));
    return result.error().reason;
  }

  // The text of a cell as the session's table reads it.
  std::string cell(const TableSession& session, std::size_t row, std::size_t column) {
    const auto cells = rowCells(session.table(), row);
    TEST_CHECK(column < cells.size());
    return cellText(session.bytes(), session.dialect(), cells[column], session.bytes().size()).text;
  }

  void testSetCellComma() {
    const std::string_view bytes = "id,name,note\n1,\"Smith, J\",\"say \"\"hi\"\"\"\n2,Lee,plain\n";
    auto session = opened(bytes, kComma);
    accepted(session.setCell(2, 1, "Kim"));
    TEST_CHECK(session.bytes() == "id,name,note\n1,\"Smith, J\",\"say \"\"hi\"\"\"\n2,Kim,plain\n");
    TEST_CHECK(cell(session, 2, 1) == "Kim");
    TEST_CHECK(session.revision() > 0 && session.modified());
    // A quoted cell stays quoted; quotes in the text are doubled.
    accepted(session.setCell(1, 2, "new \"one\""));
    TEST_CHECK(session.bytes() == "id,name,note\n1,\"Smith, J\",\"new \"\"one\"\"\"\n2,Kim,plain\n");
    TEST_CHECK(cell(session, 1, 2) == "new \"one\"");
    // A delimiter in the text quotes the cell.
    accepted(session.setCell(0, 0, "a,b"));
    TEST_CHECK(session.bytes() == "\"a,b\",name,note\n1,\"Smith, J\",\"new \"\"one\"\"\"\n2,Kim,plain\n");
    TEST_CHECK(session.rowCount() == 3 && session.columnCount() == 3);
  }

  void testSetCellTabCrLfWithoutFinalNewline() {
    auto session = opened("a\tb\r\n1\t\"x\ty\"\r\n2\t3", kTabCrLf);
    accepted(session.setCell(2, 1, "4\t5"));
    TEST_CHECK(session.bytes() == "a\tb\r\n1\t\"x\ty\"\r\n2\t\"4\t5\"");
    TEST_CHECK(cell(session, 2, 1) == "4\t5");
    accepted(session.setCell(1, 1, "z"));
    TEST_CHECK(session.bytes() == "a\tb\r\n1\t\"z\"\r\n2\t\"4\t5\"");
  }

  void testSetCellSemicolonWithMark() {
    const std::string bytes = std::string(kMark) + "k;v\r\n\"a;b\";1\r\nc;2\r\n";
    auto session = opened(bytes, kSemicolonMark);
    accepted(session.setCell(2, 0, "d"));
    TEST_CHECK(session.bytes() == std::string(kMark) + "k;v\r\n\"a;b\";1\r\nd;2\r\n");
    // A comma needs no quotes in a semicolon file.
    accepted(session.setCell(0, 1, "x,y"));
    TEST_CHECK(session.bytes() == std::string(kMark) + "k;x,y\r\n\"a;b\";1\r\nd;2\r\n");
    TEST_CHECK(cell(session, 0, 1) == "x,y" && cell(session, 1, 0) == "a;b");
  }

  void testSetCellLineBreak() {
    auto session = opened("a,b\n1,2\n", kComma);
    accepted(session.setCell(1, 1, "two\nlines"));
    TEST_CHECK(session.bytes() == "a,b\n1,\"two\nlines\"\n");
    TEST_CHECK(session.rowCount() == 2);
    TEST_CHECK(cell(session, 1, 1) == "two\nlines");
    accepted(session.setCell(0, 0, "x\r\ny"));
    TEST_CHECK(session.bytes() == "\"x\r\ny\",b\n1,\"two\nlines\"\n");
    TEST_CHECK(session.rowCount() == 2);
  }

  void testSetCellShortRow() {
    // A cell past the end of a short row gets the empty cells before it.
    auto session = opened("a,b,c\n1\n\n", kComma);
    TEST_CHECK(session.rowCount() == 3 && session.columnCount() == 3);
    accepted(session.setCell(1, 2, "x"));
    TEST_CHECK(session.bytes() == "a,b,c\n1,,x\n\n");
    TEST_CHECK(cell(session, 1, 1).empty() && cell(session, 1, 2) == "x");
    // An empty line becomes a row too.
    accepted(session.setCell(2, 1, "y"));
    TEST_CHECK(session.bytes() == "a,b,c\n1,,x\n,y\n");
    TEST_CHECK(rowCells(session.table(), 2).size() == 2);
  }

  void testSetCellWithoutChange() {
    auto session = opened("a,\"b\", c\n1\n", kComma);
    // Same text in a plain, a quoted and a padded cell, and empty text past the end of a short row.
    accepted(session.setCell(0, 0, "a"));
    accepted(session.setCell(0, 1, "b"));
    accepted(session.setCell(0, 2, " c"));
    accepted(session.setCell(1, 2, ""));
    TEST_CHECK(session.bytes() == "a,\"b\", c\n1\n");
    TEST_CHECK(session.revision() == 0 && !session.modified());
    TEST_CHECK(!session.undo());
  }

  void testSetCellRefusals() {
    auto session = opened("a,b,c\n1\n", kComma);
    TEST_CHECK(refused(session.setCell(2, 0, "x")) == TableRefusal::RowOutOfRange);
    TEST_CHECK(refused(session.setCell(1, 3, "x")) == TableRefusal::ColumnOutOfRange);
    TEST_CHECK(refused(session.setCell(9, 9, "")) == TableRefusal::RowOutOfRange);
    TEST_CHECK(session.bytes() == "a,b,c\n1\n" && session.revision() == 0);
  }

  void testSetCellByteLimit() {
    // 16383 lines of 64 bytes and "ab\n" leave 61 bytes below 1 MiB.
    std::string bytes;
    for (std::size_t i = 0; i < 16383; ++i) {
      bytes += std::string(63, 'x') + "\n";
    }
    bytes += "ab\n";
    TEST_CHECK(kTableByteLimit - bytes.size() == 61);
    auto session = opened(bytes, kComma);
    TEST_CHECK(refused(session.setCell(16383, 0, std::string(64, 'y'))) == TableRefusal::TooLarge);
    TEST_CHECK(session.bytes() == bytes && session.revision() == 0 && !session.modified());
    accepted(session.setCell(16383, 0, std::string(63, 'y')));
    TEST_CHECK(session.bytes().size() == kTableByteLimit);
    // Inserting a row adds one empty quoted cell and a line ending: 3 bytes.
    TEST_CHECK(refused(session.insertRow(0)) == TableRefusal::TooLarge);
    TEST_CHECK(refused(session.insertColumn(0)) == TableRefusal::TooLarge);
    TEST_CHECK(session.bytes().size() == kTableByteLimit && session.rowCount() == 16384);
  }

  void testInsertRow() {
    auto comma = opened("a,b\n1,\"x,y\"\n", kComma);
    accepted(comma.insertRow(1));
    TEST_CHECK(comma.bytes() == "a,b\n,\n1,\"x,y\"\n");
    accepted(comma.insertRow(3));
    TEST_CHECK(comma.bytes() == "a,b\n,\n1,\"x,y\"\n,\n");
    TEST_CHECK(comma.rowCount() == 4 && cell(comma, 3, 1).empty());

    // A row appended to a file without a final newline leaves it without one.
    auto tab = opened("a\tb\r\n1\t2", kTabCrLf);
    accepted(tab.insertRow(2));
    TEST_CHECK(tab.bytes() == "a\tb\r\n1\t2\r\n\t");
    accepted(tab.insertRow(0));
    TEST_CHECK(tab.bytes() == "\t\r\na\tb\r\n1\t2\r\n\t");

    auto semicolon = opened(std::string(kMark) + "k;v\r\nc;2\r\n", kSemicolonMark);
    accepted(semicolon.insertRow(0));
    TEST_CHECK(semicolon.bytes() == std::string(kMark) + ";\r\nk;v\r\nc;2\r\n");

    // One column: the empty cell is quoted so the row is not an empty line.
    auto single = opened("a\nb\n", kComma);
    accepted(single.insertRow(1));
    TEST_CHECK(single.bytes() == "a\n\"\"\nb\n");
    TEST_CHECK(single.rowCount() == 3 && rowCells(single.table(), 1).size() == 1);

    TEST_CHECK(refused(single.insertRow(4)) == TableRefusal::RowOutOfRange);
    // 100,000 empty lines are the reader's row limit.
    auto full = opened(std::string(100'000, '\n'), kComma);
    TEST_CHECK(refused(full.insertRow(0)) == TableRefusal::Truncated);
    TEST_CHECK(full.rowCount() == 100'000 && full.revision() == 0);
  }

  void testDeleteRow() {
    auto comma = opened("a,b\n1,\"x\ny\"\n2,3\n", kComma);
    accepted(comma.deleteRow(1));
    TEST_CHECK(comma.bytes() == "a,b\n2,3\n");
    TEST_CHECK(comma.rowCount() == 2);

    // Deleting the last row of a file without a final newline removes the line ending before it.
    auto tab = opened("a\tb\r\n1\t2\r\n3\t4", kTabCrLf);
    accepted(tab.deleteRow(2));
    TEST_CHECK(tab.bytes() == "a\tb\r\n1\t2");

    auto semicolon = opened(std::string(kMark) + "k;v\r\nc;2\r\n", kSemicolonMark);
    accepted(semicolon.deleteRow(0));
    TEST_CHECK(semicolon.bytes() == std::string(kMark) + "c;2\r\n");
    TEST_CHECK(refused(semicolon.deleteRow(0)) == TableRefusal::LastRow);
    TEST_CHECK(refused(semicolon.deleteRow(1)) == TableRefusal::RowOutOfRange);
    TEST_CHECK(semicolon.bytes() == std::string(kMark) + "c;2\r\n" && semicolon.rowCount() == 1);

    // Deleting the only row with cells leaves empty lines and no columns.
    auto blank = opened("a,b\n\n", kComma);
    accepted(blank.deleteRow(0));
    TEST_CHECK(blank.bytes() == "\n" && blank.rowCount() == 1 && blank.columnCount() == 0);
  }

  void testInsertColumn() {
    // Rows that reach the column get an empty cell; shorter rows and empty lines stay as they are.
    auto ragged = opened("a,b,c\n1\n\n\"x,y\",2,3\n", kComma);
    accepted(ragged.insertColumn(1));
    TEST_CHECK(ragged.bytes() == "a,,b,c\n1,\n\n\"x,y\",,2,3\n");
    TEST_CHECK(ragged.columnCount() == 4);
    accepted(ragged.insertColumn(4));
    TEST_CHECK(ragged.bytes() == "a,,b,c,\n1,\n\n\"x,y\",,2,3,\n");
    TEST_CHECK(refused(ragged.insertColumn(6)) == TableRefusal::ColumnOutOfRange);

    auto tab = opened("a\tb\r\n1\t2", kTabCrLf);
    accepted(tab.insertColumn(0));
    TEST_CHECK(tab.bytes() == "\ta\tb\r\n\t1\t2");

    auto semicolon = opened(std::string(kMark) + "k;v\r\n", kSemicolonMark);
    accepted(semicolon.insertColumn(2));
    TEST_CHECK(semicolon.bytes() == std::string(kMark) + "k;v;\r\n");

    // 1000 cells are the reader's column limit.
    auto wide = opened(std::string(999, ',') + "\n", kComma);
    TEST_CHECK(refused(wide.insertColumn(0)) == TableRefusal::Truncated);
    TEST_CHECK(wide.columnCount() == 1000 && wide.revision() == 0);
  }

  void testDeleteColumn() {
    auto comma = opened("a,\"b,c\",d\n1,2\n\n", kComma);
    accepted(comma.deleteColumn(1));
    TEST_CHECK(comma.bytes() == "a,d\n1\n\n");
    TEST_CHECK(comma.columnCount() == 2);

    auto tab = opened("a\tb\r\n\"x\ty\"\t2", kTabCrLf);
    accepted(tab.deleteColumn(0));
    TEST_CHECK(tab.bytes() == "b\r\n2");

    // A row left with one empty cell keeps it quoted.
    auto semicolon = opened(std::string(kMark) + "k;v\r\n\"a;b\";\r\n", kSemicolonMark);
    accepted(semicolon.deleteColumn(0));
    TEST_CHECK(semicolon.bytes() == std::string(kMark) + "v\r\n\"\"\r\n");
    TEST_CHECK(semicolon.rowCount() == 2 && rowCells(semicolon.table(), 1).size() == 1);

    // A short row whose only cell goes becomes an empty line.
    auto ragged = opened("a,b\nc\n", kComma);
    accepted(ragged.deleteColumn(0));
    TEST_CHECK(ragged.bytes() == "b\n\n");
    TEST_CHECK(refused(ragged.deleteColumn(0)) == TableRefusal::LastColumn);
    TEST_CHECK(refused(ragged.deleteColumn(1)) == TableRefusal::ColumnOutOfRange);
    TEST_CHECK(ragged.bytes() == "b\n\n" && ragged.columnCount() == 1);
  }

  void testRowAndColumnChangesKeepDoubledQuotes() {
    // LF endings, no final newline, and doubled quotes inside quoted cells.
    const std::string_view base = "a,\"say \"\"hi\"\"\",c\n1,2,\"q\"\"\"\n3,4,5";
    struct Case {
      std::expected<void, TableError> (*change)(TableSession&);
      std::string_view after;
    };
    const std::vector<Case> cases{
        {[](TableSession& s) { return s.insertRow(1); }, "a,\"say \"\"hi\"\"\",c\n,,\n1,2,\"q\"\"\"\n3,4,5"},
        {[](TableSession& s) { return s.insertRow(3); }, "a,\"say \"\"hi\"\"\",c\n1,2,\"q\"\"\"\n3,4,5\n,,"},
        {[](TableSession& s) { return s.deleteRow(2); }, "a,\"say \"\"hi\"\"\",c\n1,2,\"q\"\"\""},
        {[](TableSession& s) { return s.insertColumn(2); }, "a,\"say \"\"hi\"\"\",,c\n1,2,,\"q\"\"\"\n3,4,,5"},
        {[](TableSession& s) { return s.deleteColumn(1); }, "a,c\n1,\"q\"\"\"\n3,5"},
        {[](TableSession& s) { return s.deleteColumn(2); }, "a,\"say \"\"hi\"\"\"\n1,2\n3,4"},
    };
    for (const Case& item : cases) {
      auto session = opened(base, kComma);
      accepted(item.change(session));
      TEST_CHECK(session.bytes() == item.after);
      TEST_CHECK(session.undo() && session.bytes() == base);
    }
    auto session = opened(base, kComma);
    accepted(session.deleteColumn(2));
    TEST_CHECK(cell(session, 0, 1) == "say \"hi\"");
  }

  void testChangesWithoutEffect() {
    // A file of empty lines has no row that reaches column 0.
    auto blank = opened("\n\n", kComma);
    accepted(blank.insertColumn(0));
    TEST_CHECK(blank.bytes() == "\n\n" && blank.revision() == 0 && !blank.modified());
  }
} // namespace

int main() {
  testSetCellComma();
  testSetCellTabCrLfWithoutFinalNewline();
  testSetCellSemicolonWithMark();
  testSetCellLineBreak();
  testSetCellShortRow();
  testSetCellWithoutChange();
  testSetCellRefusals();
  testSetCellByteLimit();
  testInsertRow();
  testDeleteRow();
  testInsertColumn();
  testDeleteColumn();
  testRowAndColumnChangesKeepDoubledQuotes();
  testChangesWithoutEffect();
  std::println("editing table session edit tests passed");
  return 0;
}
