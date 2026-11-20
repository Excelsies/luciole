#include "formats/delimited_table.h"
#include "formats_delimited_fixture.h"
#include "test_check.h"

#include <cstddef>
#include <print>
#include <string>
#include <vector>

namespace {
  using namespace luciole::formats;
  using namespace luciole::test::delimited;

  void testReadPlainRows() {
    const std::string bytes = "a,b\n1,2\n";
    const Table table = readTable(bytes, Dialect{});
    TEST_CHECK(table.rows.size() == 2);
    TEST_CHECK(table.columnCount == 2);
    TEST_CHECK(!table.truncated);
    TEST_CHECK(!table.unterminatedQuote);
    TEST_CHECK((rowTexts(bytes, Dialect{}, table, 0) == Texts{"a", "b"}));
    TEST_CHECK((rowTexts(bytes, Dialect{}, table, 1) == Texts{"1", "2"}));
    TEST_CHECK((rowCells(table, 1)[1] == Cell{.begin = 6, .end = 7, .quoted = false}));
    TEST_CHECK(rowCells(table, 2).empty());
    // A row that points past the cells reads as empty.
    const Table broken{.rows = {{.firstCell = 1, .cellCount = 2}}, .cells = {Cell{}, Cell{}}};
    TEST_CHECK(rowCells(broken, 0).empty());
  }

  void testReadQuotedCells() {
    const std::string bytes = "x,\"a,b\",\"say \"\"hi\"\"\",\"line1\nline2\"\nnext\n";
    const Table table = readTable(bytes, Dialect{});
    TEST_CHECK(table.rows.size() == 2);
    TEST_CHECK((rowTexts(bytes, Dialect{}, table, 0) == Texts{"x", "a,b", "say \"hi\"", "line1\nline2"}));
    TEST_CHECK((rowTexts(bytes, Dialect{}, table, 1) == Texts{"next"}));
    TEST_CHECK((rowCells(table, 0)[1] == Cell{.begin = 2, .end = 7, .quoted = true}));
    TEST_CHECK(!rowCells(table, 0)[0].quoted);
    TEST_CHECK(!table.unterminatedQuote);
  }

  void testReadRaggedAndEmptyLines() {
    const std::string bytes = "a,b,c\n1\n\n2,3\n";
    const Table table = readTable(bytes, Dialect{});
    TEST_CHECK(table.rows.size() == 4);
    TEST_CHECK(table.rows[0].cellCount == 3);
    TEST_CHECK(table.rows[1].cellCount == 1);
    TEST_CHECK(table.rows[2].cellCount == 0);
    TEST_CHECK(table.rows[3].cellCount == 2);
    TEST_CHECK(table.columnCount == 3);
    TEST_CHECK((rowTexts(bytes, Dialect{}, table, 3) == Texts{"2", "3"}));
  }

  void testReadEdgeFiles() {
    TEST_CHECK(readTable("", Dialect{}).rows.empty());
    TEST_CHECK(readTable("", Dialect{}).columnCount == 0);

    const Table newline = readTable("\n", Dialect{});
    TEST_CHECK(newline.rows.size() == 1);
    TEST_CHECK(newline.rows[0].cellCount == 0);

    const std::string headerOnly = "name,size\n";
    const Table header = readTable(headerOnly, Dialect{});
    TEST_CHECK(header.rows.size() == 1);
    TEST_CHECK((rowTexts(headerOnly, Dialect{}, header, 0) == Texts{"name", "size"}));

    const std::string trailing = "a,\n,\n";
    const Table empties = readTable(trailing, Dialect{});
    TEST_CHECK((rowTexts(trailing, Dialect{}, empties, 0) == Texts{"a", ""}));
    TEST_CHECK((rowTexts(trailing, Dialect{}, empties, 1) == Texts{"", ""}));

    const std::string noFinal = "a,b\nc,d";
    TEST_CHECK((rowTexts(noFinal, Dialect{}, readTable(noFinal, Dialect{}), 1) == Texts{"c", "d"}));

    const std::string emptyQuoted = "\"\"\n";
    const Table quoted = readTable(emptyQuoted, Dialect{});
    TEST_CHECK(quoted.rows.size() == 1);
    TEST_CHECK((rowTexts(emptyQuoted, Dialect{}, quoted, 0) == Texts{""}));
  }

  void testReadLineEndings() {
    const std::string bytes = "a\rb\r\nc\nd";
    const Table table = readTable(bytes, Dialect{});
    TEST_CHECK(table.rows.size() == 4);
    TEST_CHECK((rowTexts(bytes, Dialect{}, table, 1) == Texts{"b"}));
    TEST_CHECK((rowTexts(bytes, Dialect{}, table, 2) == Texts{"c"}));

    const std::string quotedCrLf = "\"a\r\nb\"\r\nc\r\n";
    const Table crlf = readTable(quotedCrLf, Dialect{.lineEnding = RowEnding::CrLf});
    TEST_CHECK(crlf.rows.size() == 2);
    TEST_CHECK((rowTexts(quotedCrLf, Dialect{}, crlf, 0) == Texts{"a\r\nb"}));
  }

  void testReadByteOrderMark() {
    const std::string bytes = std::string(kBom) + "a,b\n";
    const Dialect withMark{.byteOrderMark = true};
    const Table table = readTable(bytes, withMark);
    TEST_CHECK(rowCells(table, 0)[0].begin == 3);
    TEST_CHECK((rowTexts(bytes, withMark, table, 0) == Texts{"a", "b"}));

    const Table plain = readTable(bytes, Dialect{});
    TEST_CHECK(
        rowTexts(bytes, Dialect{}, plain, 0)[0]
        == "\xEF\xBB\xBF"
           "a"
    );
  }

  void testReadOtherSyntax() {
    const Dialect backslash{.escape = QuoteEscape::Backslash};
    const std::string escaped = "\"a\\\"b\",\"c\\\\d\",e\\f\n";
    TEST_CHECK((rowTexts(escaped, backslash, readTable(escaped, backslash), 0) == Texts{"a\"b", "c\\d", "e\\f"}));

    const Dialect single{.quote = Quote::Single};
    const std::string singles = "'a,b',c\"d,'it''s'\n";
    TEST_CHECK((rowTexts(singles, single, readTable(singles, single), 0) == Texts{"a,b", "c\"d", "it's"}));

    const Dialect tab{.delimiter = Delimiter::Tab};
    const std::string tabs = "a,b\tc\n";
    TEST_CHECK((rowTexts(tabs, tab, readTable(tabs, tab), 0) == Texts{"a,b", "c"}));

    const Dialect semicolon{.delimiter = Delimiter::Semicolon};
    const std::string semis = "1,5;2\n";
    TEST_CHECK((rowTexts(semis, semicolon, readTable(semis, semicolon), 0) == Texts{"1,5", "2"}));

    const Dialect pipe{.delimiter = Delimiter::Pipe};
    const std::string pipes = "a|b|\n";
    TEST_CHECK((rowTexts(pipes, pipe, readTable(pipes, pipe), 0) == Texts{"a", "b", ""}));

    // Bytes after a closing quote stay part of the cell; a quote inside an unquoted cell is literal.
    const std::string loose = "\"ab\"cd,e\"f\n";
    TEST_CHECK((rowTexts(loose, Dialect{}, readTable(loose, Dialect{}), 0) == Texts{"abcd", "e\"f"}));

    TEST_CHECK(delimiterChar(Delimiter::Comma) == ',');
    TEST_CHECK(delimiterChar(Delimiter::Tab) == '\t');
    TEST_CHECK(delimiterChar(Delimiter::Semicolon) == ';');
    TEST_CHECK(delimiterChar(Delimiter::Pipe) == '|');
    TEST_CHECK(quoteChar(Quote::Double) == '"');
    TEST_CHECK(quoteChar(Quote::Single) == '\'');
  }

  void testReadUnterminatedQuote() {
    const std::string bytes = "a,\"bc\nd,e";
    const Table table = readTable(bytes, Dialect{});
    TEST_CHECK(table.unterminatedQuote);
    TEST_CHECK(table.rows.size() == 1);
    TEST_CHECK((rowCells(table, 0)[1] == Cell{.begin = 2, .end = 9, .quoted = true}));
    TEST_CHECK((rowTexts(bytes, Dialect{}, table, 0) == Texts{"a", "bc\nd,e"}));

    const Dialect backslash{.escape = QuoteEscape::Backslash};
    const std::string dangling = "\"ab\\";
    const Table cut = readTable(dangling, backslash);
    TEST_CHECK(cut.unterminatedQuote);
    TEST_CHECK((rowTexts(dangling, backslash, cut, 0) == Texts{"ab"}));
  }

  void testReadLimits() {
    const std::string bytes = "a,b,c,d\n1,2\n3,4\n5,6\n7,8\n";
    const Table rows = readTable(bytes, Dialect{}, TableLimits{.maxRows = 3});
    TEST_CHECK(rows.rows.size() == 3);
    TEST_CHECK(rows.truncated);

    const Table columns = readTable(bytes, Dialect{}, TableLimits{.maxColumns = 2});
    TEST_CHECK(columns.rows.size() == 5);
    TEST_CHECK(columns.columnCount == 2);
    TEST_CHECK(columns.truncated);
    TEST_CHECK((rowTexts(bytes, Dialect{}, columns, 0) == Texts{"a", "b"}));
    TEST_CHECK((rowTexts(bytes, Dialect{}, columns, 4) == Texts{"7", "8"}));

    const Table cells = readTable(bytes, Dialect{}, TableLimits{.maxCells = 7});
    TEST_CHECK(cells.truncated);
    TEST_CHECK(cells.cells.size() == 7);
    TEST_CHECK(cells.rows.size() == 3);
    TEST_CHECK(cells.rows[2].cellCount == 1);

    const Table exact = readTable(bytes, Dialect{}, TableLimits{.maxRows = 5, .maxColumns = 4, .maxCells = 12});
    TEST_CHECK(!exact.truncated);
    TEST_CHECK(exact.rows.size() == 5);

    const Table none = readTable(bytes, Dialect{}, TableLimits{.maxRows = 0});
    TEST_CHECK(none.rows.empty());
    TEST_CHECK(none.truncated);
  }

  void testCellTextBound() {
    const std::string bytes = "abcdef,\"a\"\"bcd\",a\xC3\xA9";
    const Table table = readTable(bytes, Dialect{});
    const auto cells = rowCells(table, 0);
    const CellText plain = cellText(bytes, Dialect{}, cells[0], 4);
    TEST_CHECK(plain.text == "abcd");
    TEST_CHECK(plain.clipped);
    const CellText whole = cellText(bytes, Dialect{}, cells[0], 6);
    TEST_CHECK(whole.text == "abcdef");
    TEST_CHECK(!whole.clipped);
    const CellText quoted = cellText(bytes, Dialect{}, cells[1], 3);
    TEST_CHECK(quoted.text == "a\"b");
    TEST_CHECK(quoted.clipped);
    const CellText accent = cellText(bytes, Dialect{}, cells[2], 2);
    TEST_CHECK(accent.text == "a");
    TEST_CHECK(accent.clipped);
    TEST_CHECK(cellText(bytes, Dialect{}, cells[2], 3).text == "a\xC3\xA9");
    // Three- and four-byte sequences are kept whole or dropped whole.
    const std::string wide = "x\xE2\x82\xAC\xF0\x9F\x98\x80";
    const Cell wideCell = rowCells(readTable(wide, Dialect{}), 0)[0];
    TEST_CHECK(cellText(wide, Dialect{}, wideCell, 3).text == "x");
    TEST_CHECK(cellText(wide, Dialect{}, wideCell, 4).text == "x\xE2\x82\xAC");
    TEST_CHECK(cellText(wide, Dialect{}, wideCell, 7).text == "x\xE2\x82\xAC");
    TEST_CHECK(cellText(wide, Dialect{}, wideCell, 8).text == wide);
    TEST_CHECK(cellText(bytes, Dialect{}, cells[0], 0).text.empty());
    TEST_CHECK(cellText(bytes, Dialect{}, cells[0], 0).clipped);
  }

  void testCellType() {
    TEST_CHECK(cellType("42") == CellType::Number);
    TEST_CHECK(cellType("-3.5") == CellType::Number);
    TEST_CHECK(cellType("+5") == CellType::Number);
    TEST_CHECK(cellType(".5") == CellType::Number);
    TEST_CHECK(cellType("1,5") == CellType::Number);
    TEST_CHECK(cellType("1,234,567.89") == CellType::Number);
    TEST_CHECK(cellType("1.234.567") == CellType::Number);
    TEST_CHECK(cellType("1.234,5") == CellType::Number);
    TEST_CHECK(cellType("12%") == CellType::Number);
    TEST_CHECK(cellType("1e-5") == CellType::Number);
    TEST_CHECK(cellType("6.02E23") == CellType::Number);
    TEST_CHECK(cellType(" 7 ") == CellType::Number);
    TEST_CHECK(cellType("2024") == CellType::Number);

    TEST_CHECK(cellType("2024-01-31") == CellType::Date);
    TEST_CHECK(cellType("2024/1/5") == CellType::Date);
    TEST_CHECK(cellType("31/01/2024") == CellType::Date);
    TEST_CHECK(cellType("31.01.24") == CellType::Date);
    TEST_CHECK(cellType("2024-01-31T10:20:30Z") == CellType::Date);
    TEST_CHECK(cellType("2024-01-31 10:20:30.125+02:00") == CellType::Date);
    TEST_CHECK(cellType("10:20") == CellType::Date);
    TEST_CHECK(cellType("9:05:59") == CellType::Date);

    TEST_CHECK(cellType("") == CellType::Text);
    TEST_CHECK(cellType("   ") == CellType::Text);
    TEST_CHECK(cellType("abc") == CellType::Text);
    TEST_CHECK(cellType("1.2.3") == CellType::Text);
    TEST_CHECK(cellType("1,2,3") == CellType::Text);
    TEST_CHECK(cellType("1.2.345") == CellType::Text);
    TEST_CHECK(cellType("1,23,456") == CellType::Text);
    TEST_CHECK(cellType("1..2") == CellType::Text);
    TEST_CHECK(cellType("1.") == CellType::Text);
    TEST_CHECK(cellType("-") == CellType::Text);
    TEST_CHECK(cellType("1e") == CellType::Text);
    TEST_CHECK(cellType("12a") == CellType::Text);
    TEST_CHECK(cellType("2024-01-31-") == CellType::Text);
    TEST_CHECK(cellType("2024-01/31") == CellType::Text);
    TEST_CHECK(cellType("10:2") == CellType::Text);
    TEST_CHECK(cellType("10:20:5") == CellType::Text);
    TEST_CHECK(cellType("10:20:30.") == CellType::Text);
    TEST_CHECK(cellType("10:20+2") == CellType::Text);
    TEST_CHECK(cellType("10:20+0200") == CellType::Date);
    TEST_CHECK(cellType("NaN") == CellType::Text);
    TEST_CHECK(cellType(std::string(kMaxTypedBytes, '7')) == CellType::Number);
    TEST_CHECK(cellType(std::string(kMaxTypedBytes + 1, '7')) == CellType::Text);
  }

  void testColumnTypes() {
    const std::string bytes = "name,size,when,note\nx,10,2024-01-02,a\ny,\"2,5\",2024-02-03,1\nz,,2024-03-04\n";
    const Dialect dialect{.header = true};
    const Table table = readTable(bytes, dialect);
    TEST_CHECK(
        (columnTypes(bytes, dialect, table)
         == std::vector<CellType>{CellType::Text, CellType::Number, CellType::Date, CellType::Text})
    );
    // Without a header row the names make every column Text.
    TEST_CHECK((columnTypes(bytes, Dialect{}, table) == std::vector<CellType>(4, CellType::Text)));
    // Only the sampled rows count.
    const std::string mixed = "n\n1\n2\nlate\n";
    const Dialect header{.header = true};
    const Table numbers = readTable(mixed, header);
    TEST_CHECK((columnTypes(mixed, header, numbers, 2) == std::vector<CellType>{CellType::Number}));
    TEST_CHECK((columnTypes(mixed, header, numbers, 3) == std::vector<CellType>{CellType::Text}));
    TEST_CHECK(columnTypes("", Dialect{}, readTable("", Dialect{})).empty());
  }

} // namespace

int main() {
  testReadPlainRows();
  testReadQuotedCells();
  testReadRaggedAndEmptyLines();
  testReadEdgeFiles();
  testReadLineEndings();
  testReadByteOrderMark();
  testReadOtherSyntax();
  testReadUnterminatedQuote();
  testReadLimits();
  testCellTextBound();
  testCellType();
  testColumnTypes();
  std::println("formats delimited table checks passed");
  return 0;
}
