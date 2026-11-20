#include "formats/sheet.h"
#include "formats/sheet_ods.h"
#include "test_check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <format>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
  // The largest single allocation since the last reset, recorded by the operator new below.
  std::size_t largestAllocation = 0;
} // namespace

// The replacements stay out of line so the compiler does not pair a free with a new it can see.
[[gnu::noinline]] void* operator new(std::size_t size) {
  largestAllocation = std::max(largestAllocation, size);
  if (void* memory = std::malloc(size)) {
    return memory;
  }
  throw std::bad_alloc();
}

[[gnu::noinline]] void operator delete(void* memory) noexcept { std::free(memory); }

[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
  // No single allocation of a read below may reach this; it is far below any unbounded growth.
  constexpr std::size_t kAllocationBound = 1024 * 1024;

  using namespace luciole::formats;

  constexpr std::string_view kContent = "content.xml";

  // A content.xml whose spreadsheet body holds `tables`.
  std::string content(std::string_view tables) {
    return std::format(
        R"(<?xml version="1.0" encoding="UTF-8"?>)"
        "\n"
        R"(<office:document-content xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" )"
        R"(xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" )"
        R"(xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" )"
        R"(xmlns:calcext="urn:org:documentfoundation:names:experimental:calc:xmlns:calcext:1.0" office:version="1.3">)"
        R"(<office:automatic-styles/><office:body><office:spreadsheet>{}</office:spreadsheet></office:body>)"
        R"(</office:document-content>)",
        tables
    );
  }

  std::string table(std::string_view name, std::string_view rows) {
    return std::format(R"(<table:table table:name="{}"><table:table-column/>{}</table:table>)", name, rows);
  }

  // The same content as the XLSX workbook test, as ODS.
  const std::string kBook = content(
      table(
          "Data &amp; more",
          R"(<table:table-header-rows><table:table-row>)"
          R"(<table:table-cell office:value-type="string" calcext:value-type="string"><text:p>Name</text:p>)"
          R"(</table:table-cell><table:table-cell office:value-type="string"><text:p>In<text:span>line</text:span>)"
          R"(<text:s text:c="2"/>x<text:s/></text:p><text:p>second<text:tab/>t<text:line-break/>u</text:p>)"
          R"(<office:annotation><text:p>note</text:p></office:annotation></table:table-cell>)"
          R"(<table:table-cell table:formula="of:=[.A1]&amp;[.B1]" office:value-type="string">)"
          R"(<text:p>NameInline</text:p></table:table-cell></table:table-row></table:table-header-rows>)"
          R"(<table:table-row table:number-rows-repeated="1"><table:table-cell table:number-columns-repeated="5"/>)"
          R"(</table:table-row><table:table-row>)"
          R"(<table:table-cell office:value-type="float" office:value="42.5"><text:p>42.5</text:p></table:table-cell>)"
          R"(<table:table-cell office:value-type="boolean" office:boolean-value="true"><text:p>TRUE</text:p>)"
          R"(</table:table-cell><table:table-cell office:value-type="date" office:date-value="2023-03-15"/>)"
          R"(<table:table-cell office:value-type="date" office:date-value="2023-03-15T12:00:00"/>)"
          R"(<table:table-cell table:formula="of:=1/0" office:value-type="string" office:string-value="" )"
          R"(calcext:value-type="error"><text:p>#DIV/0!</text:p></table:table-cell>)"
          R"(<table:table-cell table:formula="of:=3+4" office:value-type="float" office:value="7"/>)"
          R"(<table:table-cell office:value-type="percentage" office:value="0.25"/>)"
          R"(<table:table-cell office:value-type="currency" office:currency="EUR" office:value="-1.25"/>)"
          R"(<table:table-cell office:value-type="time" office:time-value="PT18H00M00S"/></table:table-row>)"
          R"(<table:table-row><table:table-cell/>)"
          R"(<table:table-cell office:value-type="float" office:value="1" table:number-columns-repeated="3"/>)"
          R"(<table:covered-table-cell/><table:table-cell table:number-columns-repeated="249"/>)"
          R"(<table:table-cell office:value-type="boolean" office:boolean-value="false"/>)"
          R"(<table:table-cell office:value-type="float" office:value="9" table:number-columns-repeated="3"/>)"
          R"(</table:table-row><table:table-row table:number-rows-repeated="2">)"
          R"(<table:table-cell table:number-columns-repeated="1024"/></table:table-row>)"
          R"(<table:table-row><table:table-cell><text:p>after</text:p></table:table-cell></table:table-row>)"
      )
      + table(
          "Second",
          R"(<table:table-row table:number-rows-repeated="1"/><table:table-row><table:table-cell/>)"
          R"(<table:table-cell office:value-type="float" office:value="1"/></table:table-row>)"
      )
  );

  MemberReader readerOf(const std::optional<std::string>& bytes) {
    return [bytes](std::string_view name) -> std::optional<std::string> {
      if (name != kContent) {
        return std::nullopt;
      }
      return bytes;
    };
  }

  std::expected<SheetBook, SheetError>
  readWith(const std::string& bytes, std::size_t sheet = 0, const SheetLimits& limits = {}) {
    return readOds(readerOf(bytes), sheet, limits);
  }

  const SheetCell* cellAt(const SheetTable& table, std::uint32_t row, std::uint32_t column) {
    for (const SheetRow& stored : table.rows) {
      if (stored.index != row) {
        continue;
      }
      for (const SheetCell& cell : stored.cells) {
        if (cell.column == column) {
          return &cell;
        }
      }
    }
    return nullptr;
  }

  bool hasText(const SheetTable& table, std::uint32_t row, std::uint32_t column, std::string_view text) {
    const SheetCell* cell = cellAt(table, row, column);
    return cell != nullptr && cell->kind == SheetCellKind::Text && cell->text == text;
  }

  bool hasNumber(const SheetTable& table, std::uint32_t row, std::uint32_t column, SheetCellKind kind, double value) {
    const SheetCell* cell = cellAt(table, row, column);
    return cell != nullptr && cell->kind == kind && std::abs(cell->number - value) < 1.0E-9;
  }

  std::vector<std::uint32_t> rowIndices(const SheetTable& table) {
    std::vector<std::uint32_t> indices;
    indices.reserve(table.rows.size());
    for (const SheetRow& row : table.rows) {
      indices.push_back(row.index);
    }
    return indices;
  }

  bool failsWith(const std::expected<SheetBook, SheetError>& result, SheetErrorKind kind) {
    return !result && result.error().kind == kind && result.error().member == kContent;
  }

  // Real rows, then a row of `count` cells that `repeat` names, then `after`.
  std::string repeated(std::string_view rowRepeat, std::string_view cells, std::string_view after = "") {
    return content(table(
        "Big",
        std::format(
            R"(<table:table-row><table:table-cell office:value-type="float" office:value="1"/></table:table-row>)"
            R"(<table:table-row><table:table-cell office:value-type="float" office:value="2"/></table:table-row>)"
            R"(<table:table-row table:number-rows-repeated="{}">{}</table:table-row>{})",
            rowRepeat, cells, after
        )
    ));
  }

  void testTablesAndCells() {
    const auto book = readWith(kBook);
    TEST_CHECK(book.has_value());
    TEST_CHECK((book->sheetNames == std::vector<std::string>{"Data & more", "Second"}));
    TEST_CHECK(book->sheetIndex == 0);
    const SheetTable& table = book->table;
    TEST_CHECK(table.dateSystem == DateSystem::Date1900);
    TEST_CHECK((rowIndices(table) == std::vector<std::uint32_t>{0, 2, 3, 6}));
    // Paragraphs join with newlines; spans, spaces, tabs and line breaks are kept, annotations are not.
    TEST_CHECK(hasText(table, 0, 0, "Name"));
    TEST_CHECK(hasText(table, 0, 1, "Inline  x \nsecond\tt\nu"));
    TEST_CHECK(hasText(table, 0, 2, "NameInline"));
    TEST_CHECK(cellAt(table, 0, 2)->formula);
    TEST_CHECK(!cellAt(table, 0, 1)->formula);
    TEST_CHECK(hasNumber(table, 2, 0, SheetCellKind::Number, 42.5));
    TEST_CHECK(hasNumber(table, 2, 1, SheetCellKind::Boolean, 1.0));
    TEST_CHECK(hasNumber(table, 2, 2, SheetCellKind::Date, 45000.0));
    TEST_CHECK(hasNumber(table, 2, 3, SheetCellKind::Date, 45000.5));
    const SheetCell* error = cellAt(table, 2, 4);
    TEST_CHECK(error != nullptr && error->kind == SheetCellKind::Error && error->text == "#DIV/0!" && error->formula);
    TEST_CHECK(hasNumber(table, 2, 5, SheetCellKind::Number, 7.0));
    TEST_CHECK(cellAt(table, 2, 5)->formula);
    TEST_CHECK(hasNumber(table, 2, 6, SheetCellKind::Number, 0.25));
    TEST_CHECK(hasNumber(table, 2, 7, SheetCellKind::Number, -1.25));
    TEST_CHECK(hasNumber(table, 2, 8, SheetCellKind::Time, 0.75));
    TEST_CHECK(formatSheetDate(cellAt(table, 2, 2)->number, table.dateSystem) == "2023-03-15");
    TEST_CHECK(formatSheetDate(cellAt(table, 2, 3)->number, table.dateSystem) == "2023-03-15T12:00:00");
    TEST_CHECK(formatSheetTime(cellAt(table, 2, 8)->number) == "18:00:00");
    TEST_CHECK(hasText(table, 6, 0, "after"));
  }

  void testRepeatedCellsAndColumnLimit() {
    const auto book = readWith(kBook);
    TEST_CHECK(book.has_value());
    const SheetTable& table = book->table;
    // Repeated cells are copies at consecutive columns; empty and covered cells only move the column.
    TEST_CHECK(cellAt(table, 3, 0) == nullptr);
    for (std::uint32_t column = 1; column <= 3; ++column) {
      TEST_CHECK(hasNumber(table, 3, column, SheetCellKind::Number, 1.0));
    }
    TEST_CHECK(cellAt(table, 3, 4) == nullptr);
    TEST_CHECK(hasNumber(table, 3, 254, SheetCellKind::Boolean, 0.0));
    TEST_CHECK(hasNumber(table, 3, 255, SheetCellKind::Number, 9.0));
    // Copies at columns 256 and 257 are past the column limit.
    TEST_CHECK(table.rows[2].cells.size() == 5);
    TEST_CHECK(table.truncated);
    TEST_CHECK((table.usedRange == SheetRange{.firstRow = 0, .firstColumn = 0, .lastRow = 6, .lastColumn = 255}));
  }

  void testSecondTable() {
    const auto book = readWith(kBook, 1);
    TEST_CHECK(book.has_value());
    TEST_CHECK(book->sheetIndex == 1);
    TEST_CHECK((rowIndices(book->table) == std::vector<std::uint32_t>{1}));
    TEST_CHECK(hasNumber(book->table, 1, 1, SheetCellKind::Number, 1.0));
    TEST_CHECK(!book->table.truncated);
    TEST_CHECK((book->table.usedRange == SheetRange{.firstRow = 1, .firstColumn = 1, .lastRow = 1, .lastColumn = 1}));
  }

  void testMillionEmptyRowsCostNothing() {
    const std::string emptyCells = R"(<table:table-cell table:number-columns-repeated="1000"/>)";
    const std::string bytes = repeated("1000000", emptyCells);
    const std::clock_t start = std::clock();
    largestAllocation = 0;
    const auto book = readWith(bytes);
    TEST_CHECK(largestAllocation < kAllocationBound);
    TEST_CHECK(book.has_value());
    TEST_CHECK((rowIndices(book->table) == std::vector<std::uint32_t>{0, 1}));
    TEST_CHECK(!book->table.truncated);
    // With limits that would admit them all, a real row after the empty ones lands at its own index.
    const std::string after =
        R"(<table:table-row><table:table-cell office:value-type="float" office:value="3"/></table:table-row>)";
    const SheetLimits roomy{.maxRows = 2'000'000, .maxCells = 2'000'000'000};
    const auto far = readWith(repeated("1000000", emptyCells, after), 0, roomy);
    TEST_CHECK(far.has_value());
    TEST_CHECK((rowIndices(far->table) == std::vector<std::uint32_t>{0, 1, 1'000'002}));
    TEST_CHECK(!far->table.truncated);
    // A count past any row index saturates; the row after it is past the limit.
    const auto huge = readWith(repeated("18446744073709551615", emptyCells, after));
    TEST_CHECK(huge.has_value());
    TEST_CHECK((rowIndices(huge->table) == std::vector<std::uint32_t>{0, 1}));
    TEST_CHECK(huge->table.truncated);
    TEST_CHECK(static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC < 1.0);
  }

  void testRepeatsOfNonEmptyRowsAreCapped() {
    const std::string cell = R"(<table:table-cell office:value-type="float" office:value="5"/>)";
    const std::clock_t start = std::clock();
    const auto capped = readWith(repeated("10000", cell));
    TEST_CHECK(capped.has_value());
    TEST_CHECK(capped->table.rows.size() == 5'000);
    TEST_CHECK(capped->table.rows.back().index == 4'999);
    TEST_CHECK(hasNumber(capped->table, 4'999, 0, SheetCellKind::Number, 5.0));
    TEST_CHECK(capped->table.truncated);
    // Within the limits every copy is kept.
    const auto whole = readWith(repeated("10000", cell), 0, SheetLimits{.maxRows = 20'000});
    TEST_CHECK(whole.has_value() && whole->table.rows.size() == 10'002 && !whole->table.truncated);
    // The cell limit caps copies too.
    const auto cells = readWith(repeated("10000", cell), 0, SheetLimits{.maxCells = 100});
    TEST_CHECK(cells.has_value() && cells->table.rows.size() == 100 && cells->table.truncated);
    // A cell repeated far past the column limit is copied up to it.
    const auto wide = readWith(repeated(
        "1",
        R"(<table:table-cell office:value-type="string" table:number-columns-repeated="1000000000">)"
        R"(<text:p>w</text:p></table:table-cell>)"
    ));
    TEST_CHECK(wide.has_value() && wide->table.rows.size() == 3 && wide->table.rows[2].cells.size() == 256);
    TEST_CHECK(wide->table.truncated);
    TEST_CHECK(static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC < 2.0);
  }

  void testSpacesAreBoundedByTheCellLimit() {
    const std::string bytes = content(table(
        "S",
        R"(<table:table-row><table:table-cell><text:p>a<text:s text:c="18446744073709551615"/>)"
        R"(</text:p></table:table-cell></table:table-row>)"
    ));
    largestAllocation = 0;
    const auto book = readWith(bytes, 0, SheetLimits{.maxCellChars = 4});
    TEST_CHECK(largestAllocation < kAllocationBound);
    TEST_CHECK(book.has_value() && hasText(book->table, 0, 0, "a   ") && book->table.truncated);
  }

  void testMissingMemberAndSheetIndex() {
    TEST_CHECK(failsWith(readOds(readerOf(std::nullopt)), SheetErrorKind::MissingMember));
    TEST_CHECK(failsWith(readWith(kBook, 2), SheetErrorKind::SheetIndex));
    const auto none = readWith(content(""));
    TEST_CHECK(failsWith(none, SheetErrorKind::SheetIndex));
  }

  void testBadValues() {
    for (const std::string_view cell :
         {R"(<table:table-cell office:value-type="float"/>)",
          R"(<table:table-cell office:value-type="float" office:value="x"/>)",
          R"(<table:table-cell office:value-type="boolean" office:boolean-value="maybe"/>)",
          R"(<table:table-cell office:value-type="date" office:date-value="2023-02-30"/>)",
          R"(<table:table-cell office:value-type="time" office:time-value="18:00"/>)",
          R"(<table:table-cell office:value-type="time" office:time-value="P1Y"/>)",
          R"(<table:table-cell office:value-type="time" office:time-value="PT"/>)",
          R"(<table:table-cell office:value-type="time" office:time-value="PT1H-2M"/>)",
          R"(<table:table-cell office:value-type="void"/>)", R"(<table:table-cell table:number-columns-repeated="0"/>)",
          R"(<table:table-cell table:number-columns-repeated="x"/>)",
          R"(<table:table-cell><text:p><text:s text:c="x"/></text:p></table:table-cell>)"}) {
      const std::string bytes = content(table("Bad", std::format("<table:table-row>{}</table:table-row>", cell)));
      TEST_CHECK(failsWith(readWith(bytes), SheetErrorKind::BadValue));
    }
    TEST_CHECK(failsWith(readWith(repeated("0", "")), SheetErrorKind::BadValue));
    // A table without a name is listed with an empty one.
    const auto unnamed = readWith(content("<table:table/>"));
    TEST_CHECK(unnamed.has_value() && unnamed->sheetNames == std::vector<std::string>{""});
    TEST_CHECK(unnamed->table.rows.empty() && !unnamed->table.usedRange.has_value());
  }

  void testDurations() {
    const auto time = [](std::string_view value) -> std::optional<double> {
      const auto book = readWith(content(table(
          "T",
          std::format(
              R"(<table:table-row><table:table-cell office:value-type="time" office:time-value="{}"/>)"
              R"(</table:table-row>)",
              value
          )
      )));
      if (!book || book->table.rows.empty()) {
        return std::nullopt;
      }
      return book->table.rows[0].cells[0].number;
    };
    TEST_NEAR(time("PT36H00M00S"), 1.5, 1.0E-12);
    TEST_NEAR(time("P1DT2H"), 1.0 + (2.0 / 24.0), 1.0E-12);
    TEST_NEAR(time("PT0H0M0.5S"), 0.5 / 86'400.0, 1.0E-12);
    TEST_NEAR(time("-PT6H"), -0.25, 1.0E-12);
  }

  void testMalformedXml() {
    const auto book = readWith(kBook.substr(0, kBook.size() / 2));
    TEST_CHECK(failsWith(book, SheetErrorKind::Xml));
    TEST_CHECK(book.error().xml == XmlError::Unterminated);
  }

  void testCellTextLimit() {
    const auto book = readWith(kBook, 0, SheetLimits{.maxCellChars = 3});
    TEST_CHECK(book.has_value());
    TEST_CHECK(hasText(book->table, 0, 0, "Nam"));
    TEST_CHECK(hasText(book->table, 0, 1, "Inl"));
    TEST_CHECK(book->table.truncated);
  }

  // Cells with a typed value keep no display text, so it does not count against the text limit.
  void testTypedCellsKeepNoText() {
    const auto book = readWith(kBook);
    TEST_CHECK(book.has_value());
    TEST_CHECK(cellAt(book->table, 2, 0)->text.empty());
    TEST_CHECK(cellAt(book->table, 2, 1)->text.empty());
    const std::string bytes = content(table(
        "N",
        R"(<table:table-row><table:table-cell office:value-type="currency" office:value="1234.5">)"
        R"(<text:p>1,234.50 €</text:p></table:table-cell></table:table-row>)"
    ));
    const auto money = readWith(bytes, 0, SheetLimits{.maxTextBytes = 0});
    TEST_CHECK(money.has_value() && !money->table.truncated);
    TEST_CHECK(
        hasNumber(money->table, 0, 0, SheetCellKind::Number, 1234.5) && cellAt(money->table, 0, 0)->text.empty()
    );
  }

  // A date value may carry a time-zone designator, which is passed over.
  void testDateWithZone() {
    const std::string bytes = content(table(
        "Z",
        R"(<table:table-row><table:table-cell office:value-type="date" office:date-value="2023-03-15T12:00:00+01:00"/>)"
        R"(<table:table-cell office:value-type="date" office:date-value="2023-03-15Z"/></table:table-row>)"
    ));
    const auto book = readWith(bytes);
    TEST_CHECK(book.has_value());
    TEST_CHECK(hasNumber(book->table, 0, 0, SheetCellKind::Date, 45000.5));
    TEST_CHECK(hasNumber(book->table, 0, 1, SheetCellKind::Date, 45000.0));
  }
} // namespace

int main() {
  testTablesAndCells();
  testRepeatedCellsAndColumnLimit();
  testSecondTable();
  testMillionEmptyRowsCostNothing();
  testRepeatsOfNonEmptyRowsAreCapped();
  testSpacesAreBoundedByTheCellLimit();
  testMissingMemberAndSheetIndex();
  testBadValues();
  testDurations();
  testMalformedXml();
  testCellTextLimit();
  testTypedCellsKeepNoText();
  testDateWithZone();
  return 0;
}
