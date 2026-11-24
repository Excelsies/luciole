#include "cpu_budget.h"
#include "formats/delimited_detect.h"
#include "formats/delimited_table.h"
#include "formats/delimited_write.h"
#include "formats_delimited_fixture.h"
#include "test_check.h"

#include <cstddef>
#include <ctime>
#include <expected>
#include <print>
#include <string>
#include <vector>

namespace {
  using namespace luciole::formats;
  using namespace luciole::test::delimited;

  // The output of a writer that must succeed.
  std::string written(const std::expected<std::string, EditError>& result) {
    TEST_CHECK(result.has_value());
    return *result;
  }

  std::string edited(std::string_view bytes, const Dialect& dialect, const std::vector<CellEdit>& edits) {
    return written(editCells(bytes, dialect, edits));
  }

  void testRoundTripWithoutEdits() {
    const std::vector<std::string> files{
        "a,b\n1,2\n",
        std::string(kBom) + "name;qty\r\nx;1\r\ny;2",
        "a,b\r\nc,d\ne,f\rg,h",
        "id,note\n1,\"line one\nline two\"\n2,\"say \"\"hi\"\"\"\n",
        "id,note\r\n1,\"a\r\nb\"\r\n",
        "a,b,c\n1\n\n2,3\n\n",
        "a\tb\n1\t\"2\t3\"\n",
        "a|b\n|\n",
        "'a,b',c\n'it''s',d\n",
        "\"a\\\"b\",c\n\"d\\\\\",e\n",
        "\"ab\"cd,e\"f\n",
        "",
        std::string(kBom),
        "\n\n\n",
    };
    for (const std::string& bytes : files) {
      const Dialect dialect = detectDialect(bytes);
      TEST_CHECK(edited(bytes, dialect, {}) == bytes);
    }
  }

  void testEditOneCell() {
    TEST_CHECK(edited("a,b,c\n1,2,3\n", Dialect{}, {{.row = 1, .column = 1, .text = "20"}}) == "a,b,c\n1,20,3\n");
    TEST_CHECK(edited("a,b,c\n1,2,3", Dialect{}, {{.row = 1, .column = 2, .text = "x"}}) == "a,b,c\n1,2,x");
    TEST_CHECK(edited("a,b,c\n1,2,3\n", Dialect{}, {{.row = 0, .column = 0, .text = ""}}) == ",b,c\n1,2,3\n");
    // An originally quoted cell stays quoted.
    TEST_CHECK(edited("a,\"b\"\n", Dialect{}, {{.row = 0, .column = 1, .text = "x"}}) == "a,\"x\"\n");
    // Everything else stays byte-identical: mark, CRLF, missing final newline, other quoting.
    const std::string bytes = std::string(kBom) + "a;\"b;c\"\r\nd;e\r\nf;g";
    const Dialect dialect = detectDialect(bytes);
    TEST_CHECK(
        edited(bytes, dialect, {{.row = 1, .column = 0, .text = "z"}}) == std::string(kBom) + "a;\"b;c\"\r\nz;e\r\nf;g"
    );
    // Several edits in any order.
    TEST_CHECK(
        edited(
            "a,b\nc,d\ne,f\n", Dialect{},
            {{.row = 2, .column = 1, .text = "F"},
             {.row = 0, .column = 0, .text = "A"},
             {.row = 0, .column = 1, .text = "B"}}
        )
        == "A,B\nc,d\ne,F\n"
    );
  }

  void testEditQuoting() {
    const std::string bytes = "a,b,c\n1,2,3\n";
    TEST_CHECK(edited(bytes, Dialect{}, {{.row = 1, .column = 1, .text = "2,5"}}) == "a,b,c\n1,\"2,5\",3\n");
    TEST_CHECK(
        edited(bytes, Dialect{}, {{.row = 1, .column = 1, .text = "say \"hi\""}}) == "a,b,c\n1,\"say \"\"hi\"\"\",3\n"
    );
    TEST_CHECK(edited(bytes, Dialect{}, {{.row = 1, .column = 1, .text = "x\ny"}}) == "a,b,c\n1,\"x\ny\",3\n");
    TEST_CHECK(edited(bytes, Dialect{}, {{.row = 1, .column = 1, .text = "x\ry"}}) == "a,b,c\n1,\"x\ry\",3\n");
    // A semicolon needs no quotes in a comma file; a backslash needs none outside quotes.
    TEST_CHECK(edited(bytes, Dialect{}, {{.row = 1, .column = 1, .text = "x;y\\"}}) == "a,b,c\n1,x;y\\,3\n");

    const Dialect backslash{.escape = QuoteEscape::Backslash};
    TEST_CHECK(
        edited(bytes, backslash, {{.row = 1, .column = 1, .text = "a\"b\\c"}}) == "a,b,c\n1,\"a\\\"b\\\\c\",3\n"
    );
    const Dialect single{.delimiter = Delimiter::Tab, .quote = Quote::Single};
    TEST_CHECK(edited("a\tb\n", single, {{.row = 0, .column = 1, .text = "it's"}}) == "a\t'it''s'\n");
    TEST_CHECK(edited("a\tb\n", single, {{.row = 0, .column = 1, .text = "x\ty"}}) == "a\t'x\ty'\n");
    TEST_CHECK(edited("a\tb\n", single, {{.row = 0, .column = 1, .text = "x,\"y"}}) == "a\tx,\"y\n");

    // Each edited file reads back the edited text.
    const std::vector<std::string> texts{"2,5", "say \"hi\"", "x\r\ny", "\\\"", "'", ""};
    for (const Dialect& dialect : {Dialect{}, backslash, Dialect{.quote = Quote::Single}}) {
      for (const std::string& text : texts) {
        const std::string out = edited(bytes, dialect, {{.row = 1, .column = 1, .text = text}});
        const Table table = readTable(out, dialect);
        TEST_CHECK(table.rows.size() == 2);
        TEST_CHECK(rowTexts(out, dialect, table, 1).at(1) == text);
      }
    }
  }

  void testEditShortRows() {
    // A missing cell in a short row gets empty cells before it; empty text there changes nothing.
    TEST_CHECK(edited("a,b,c\n1\n", Dialect{}, {{.row = 1, .column = 2, .text = "x"}}) == "a,b,c\n1,,x\n");
    TEST_CHECK(edited("a,b,c\n1\n", Dialect{}, {{.row = 1, .column = 2, .text = ""}}) == "a,b,c\n1\n");
    TEST_CHECK(
        edited("a,b,c\n1\n", Dialect{}, {{.row = 1, .column = 1, .text = "x"}, {.row = 1, .column = 2, .text = "y"}})
        == "a,b,c\n1,x,y\n"
    );
    TEST_CHECK(edited("a,b\n\nc,d\n", Dialect{}, {{.row = 1, .column = 1, .text = "x"}}) == "a,b\n,x\nc,d\n");
    TEST_CHECK(edited("a,b\n\nc,d\n", Dialect{}, {{.row = 1, .column = 0, .text = "x"}}) == "a,b\nx\nc,d\n");
    // A row whose only cell becomes empty keeps one cell instead of turning into an empty line.
    TEST_CHECK(edited("a\nb\n", Dialect{}, {{.row = 1, .column = 0, .text = ""}}) == "a\n\"\"\n");
    TEST_CHECK(edited("a,b\nc\n", Dialect{}, {{.row = 1, .column = 0, .text = ""}}) == "a,b\n\"\"\n");
  }

  void testEditErrors() {
    const std::string bytes = "a,b\nc\n";
    const auto row = editCells(bytes, Dialect{}, std::vector<CellEdit>{{.row = 2, .column = 0, .text = "x"}});
    TEST_CHECK(!row.has_value() && row.error() == EditError::RowOutOfRange);
    const auto column = editCells(bytes, Dialect{}, std::vector<CellEdit>{{.row = 1, .column = 2, .text = "x"}});
    TEST_CHECK(!column.has_value() && column.error() == EditError::ColumnOutOfRange);
    const auto twice = editCells(
        bytes, Dialect{},
        std::vector<CellEdit>{{.row = 0, .column = 1, .text = "x"}, {.row = 0, .column = 1, .text = "y"}}
    );
    TEST_CHECK(!twice.has_value() && twice.error() == EditError::DuplicateCell);
    const std::string open = "a,\"b\nc";
    const auto quote = editCells(open, Dialect{}, std::vector<CellEdit>{});
    TEST_CHECK(!quote.has_value() && quote.error() == EditError::UnterminatedQuote);
    TEST_CHECK(!insertRow(open, Dialect{}, 0, {}).has_value());
    TEST_CHECK(!deleteRow(open, Dialect{}, 0).has_value());
    TEST_CHECK(!insertColumn(open, Dialect{}, 0).has_value());
    TEST_CHECK(!deleteColumn(open, Dialect{}, 0).has_value());
    TEST_CHECK(insertRow(open, Dialect{}, 0, {}).error() == EditError::UnterminatedQuote);
  }

  void testInsertRow() {
    const std::vector<std::string> cells{"x", "y"};
    TEST_CHECK(written(insertRow("a,b\n1,2\n", Dialect{}, 1, cells)) == "a,b\nx,y\n1,2\n");
    TEST_CHECK(written(insertRow("a,b\n1,2\n", Dialect{}, 0, cells)) == "x,y\na,b\n1,2\n");
    TEST_CHECK(written(insertRow("a,b\n1,2\n", Dialect{}, 2, cells)) == "a,b\n1,2\nx,y\n");
    TEST_CHECK(written(insertRow("a,b\n1,2", Dialect{}, 2, cells)) == "a,b\n1,2\nx,y");
    TEST_CHECK(written(insertRow("a,b\n1,2", Dialect{}, 1, cells)) == "a,b\nx,y\n1,2");
    const Dialect crlf{.lineEnding = RowEnding::CrLf};
    TEST_CHECK(written(insertRow("a,b\r\n1,2", crlf, 1, cells)) == "a,b\r\nx,y\r\n1,2");
    TEST_CHECK(written(insertRow("a,b\r\n1,2", crlf, 2, cells)) == "a,b\r\n1,2\r\nx,y");
    const Dialect cr{.lineEnding = RowEnding::Cr};
    TEST_CHECK(written(insertRow("a\r", cr, 1, cells)) == "a\rx,y\r");
    // Cells are quoted as edits are; no cells is an empty line; one empty cell is quoted.
    const std::vector<std::string> awkward{"p,q", "", "r\"s"};
    TEST_CHECK(written(insertRow("a\n", Dialect{}, 1, awkward)) == "a\n\"p,q\",,\"r\"\"s\"\n");
    TEST_CHECK(written(insertRow("a\n", Dialect{}, 1, {})) == "a\n\n");
    TEST_CHECK(written(insertRow("a\n", Dialect{}, 1, std::vector<std::string>{""})) == "a\n\"\"\n");
    // Empty files and a lone byte order mark.
    TEST_CHECK(written(insertRow("", Dialect{}, 0, cells)) == "x,y\n");
    const Dialect marked{.byteOrderMark = true};
    TEST_CHECK(written(insertRow(kBom, marked, 0, cells)) == std::string(kBom) + "x,y\n");
    TEST_CHECK(written(insertRow(std::string(kBom) + "a\n", marked, 0, cells)) == std::string(kBom) + "x,y\na\n");
    const auto far = insertRow("a\n", Dialect{}, 2, cells);
    TEST_CHECK(!far.has_value() && far.error() == EditError::RowOutOfRange);
  }

  void testDeleteRow() {
    TEST_CHECK(written(deleteRow("a\nb\nc\n", Dialect{}, 1)) == "a\nc\n");
    TEST_CHECK(written(deleteRow("a\nb\nc\n", Dialect{}, 0)) == "b\nc\n");
    TEST_CHECK(written(deleteRow("a\nb\nc\n", Dialect{}, 2)) == "a\nb\n");
    TEST_CHECK(written(deleteRow("a\nb\nc", Dialect{}, 2)) == "a\nb");
    TEST_CHECK(written(deleteRow("a\r\nb\rc", Dialect{}, 2)) == "a\r\nb");
    TEST_CHECK(written(deleteRow("a\r\nb\rc", Dialect{}, 1)) == "a\r\nc");
    TEST_CHECK(written(deleteRow("a", Dialect{}, 0)).empty());
    TEST_CHECK(written(deleteRow("a\n\nb\n", Dialect{}, 1)) == "a\nb\n");
    TEST_CHECK(written(deleteRow("a\n\"x\ny\"\nb\n", Dialect{}, 1)) == "a\nb\n");
    const Dialect marked{.byteOrderMark = true};
    TEST_CHECK(written(deleteRow(std::string(kBom) + "a\nb\n", marked, 0)) == std::string(kBom) + "b\n");
    const auto far = deleteRow("a\nb\n", Dialect{}, 2);
    TEST_CHECK(!far.has_value() && far.error() == EditError::RowOutOfRange);
    TEST_CHECK(!deleteRow("", Dialect{}, 0).has_value());
  }

  void testInsertColumn() {
    TEST_CHECK(written(insertColumn("a,b\n1,2\n", Dialect{}, 1)) == "a,,b\n1,,2\n");
    TEST_CHECK(written(insertColumn("a,b\n1,2\n", Dialect{}, 0)) == ",a,b\n,1,2\n");
    TEST_CHECK(written(insertColumn("a,b\n1,2", Dialect{}, 2)) == "a,b,\n1,2,");
    // Rows that do not reach the column and empty lines stay as they are.
    TEST_CHECK(written(insertColumn("a,b,c\n1\n\n", Dialect{}, 2)) == "a,b,,c\n1\n\n");
    TEST_CHECK(written(insertColumn("a,b,c\n1\n\n", Dialect{}, 1)) == "a,,b,c\n1,\n\n");
    TEST_CHECK(
        written(insertColumn("\"a,b\";c\r\n", Dialect{.delimiter = Delimiter::Semicolon}, 1)) == "\"a,b\";;c\r\n"
    );
    TEST_CHECK(written(insertColumn("", Dialect{}, 0)).empty());
    const auto far = insertColumn("a,b\n", Dialect{}, 3);
    TEST_CHECK(!far.has_value() && far.error() == EditError::ColumnOutOfRange);
  }

  void testDeleteColumn() {
    TEST_CHECK(written(deleteColumn("a,b,c\n1,2,3\n", Dialect{}, 1)) == "a,c\n1,3\n");
    TEST_CHECK(written(deleteColumn("a,b,c\n1,2,3\n", Dialect{}, 0)) == "b,c\n2,3\n");
    TEST_CHECK(written(deleteColumn("a,b,c\n1,2,3", Dialect{}, 2)) == "a,b\n1,2");
    TEST_CHECK(written(deleteColumn("a,\"x,y\",c\r\n", Dialect{}, 1)) == "a,c\r\n");
    TEST_CHECK(written(deleteColumn("a,b,c\n1\n\n", Dialect{}, 1)) == "a,c\n1\n\n");
    TEST_CHECK(written(deleteColumn("a\nb\n", Dialect{}, 0)) == "\n\n");
    TEST_CHECK(written(deleteColumn("a,b\r\nc,d\r\n", Dialect{}, 0)) == "b\r\nd\r\n");
    const auto far = deleteColumn("a,b\n", Dialect{}, 2);
    TEST_CHECK(!far.has_value() && far.error() == EditError::ColumnOutOfRange);
    TEST_CHECK(!deleteColumn("", Dialect{}, 0).has_value());
  }

  using Rows = std::vector<Texts>;

  // Every row's cell texts, read without limits.
  Rows tableRows(std::string_view bytes, const Dialect& dialect) {
    const std::size_t all = bytes.size() + 1;
    const Table table = readTable(bytes, dialect, TableLimits{.maxRows = all, .maxColumns = all, .maxCells = all});
    Rows rows;
    for (std::size_t row = 0; row < table.rows.size(); ++row) {
      rows.push_back(rowTexts(bytes, dialect, table, row));
    }
    return rows;
  }

  void testWritersAtMixedLineEndings() {
    const Dialect lf{};
    const Dialect cr{.lineEnding = RowEnding::Cr};
    // A CR line ending that comes to stand before an LF gets an LF of its own, so the two are not one CRLF.
    const std::string emptyAfterCr = written(insertRow("a\rb\n", lf, 1, {}));
    TEST_CHECK(emptyAfterCr == "a\r\n\nb\n");
    TEST_CHECK((tableRows(emptyAfterCr, lf) == Rows{{"a"}, {}, {"b"}}));
    const std::string crBeforeEmpty = written(insertRow("\nb\n", cr, 0, std::vector<std::string>{"x"}));
    TEST_CHECK(crBeforeEmpty == "x\r\n\nb\n");
    TEST_CHECK((tableRows(crBeforeEmpty, cr) == Rows{{"x"}, {}, {"b"}}));
    const std::string rowGone = written(deleteRow("a\rb\n\nc\n", lf, 1));
    TEST_CHECK(rowGone == "a\r\n\nc\n");
    TEST_CHECK((tableRows(rowGone, lf) == Rows{{"a"}, {}, {"c"}}));
    const std::string columnGone = written(deleteColumn("a\rb\n", lf, 0));
    TEST_CHECK(columnGone == "\r\n\n");
    TEST_CHECK((tableRows(columnGone, lf) == Rows{{}, {}}));
    // A CRLF that was in the file stays one line ending.
    TEST_CHECK(written(deleteRow("a\r\nb\nc\n", lf, 1)) == "a\r\nc\n");
  }

  void testWritersAtUnterminatedLastRow() {
    const Dialect lf{};
    // An empty line needs its own line ending, so the file gains a final newline.
    TEST_CHECK(written(insertRow("a", lf, 1, {})) == "a\n\n");
    TEST_CHECK(written(insertRow("a", Dialect{.lineEnding = RowEnding::Cr}, 1, {})) == "a\r\r");
    TEST_CHECK((tableRows(written(insertRow("a", lf, 1, {})), lf) == Rows{{"a"}, {}}));
    // Removing the last row keeps an empty line before it.
    TEST_CHECK(written(deleteRow("a\n\nb", lf, 2)) == "a\n\n");
    TEST_CHECK((tableRows(written(deleteRow("a\n\nb", lf, 2)), lf) == Rows{{"a"}, {}}));
    // A last row whose only cell goes becomes an empty line with a line ending.
    TEST_CHECK(written(deleteColumn("a\nb", lf, 0)) == "\n\n");
    TEST_CHECK(written(deleteColumn("a\rb", lf, 0)) == "\r\n\n");
    TEST_CHECK((tableRows(written(deleteColumn("a\rb", lf, 0)), lf) == Rows{{}, {}}));
    // A row left with one empty cell keeps it, quoted.
    TEST_CHECK(written(deleteColumn(",", lf, 0)) == "\"\"");
    TEST_CHECK(written(deleteColumn(",b\nc,\n", lf, 1)) == "\"\"\nc\n");
    TEST_CHECK(written(deleteColumn("a,\n", lf, 0)) == "\"\"\n");
    TEST_CHECK((tableRows(written(deleteColumn(",", lf, 0)), lf) == Rows{{""}}));
  }

  void testEditQuotesPaddedText() {
    const std::string bytes = "a,b\n";
    TEST_CHECK(edited(bytes, Dialect{}, {{.row = 0, .column = 1, .text = " x "}}) == "a,\" x \"\n");
    TEST_CHECK(edited(bytes, Dialect{}, {{.row = 0, .column = 1, .text = "x "}}) == "a,\"x \"\n");
    TEST_CHECK(edited(bytes, Dialect{}, {{.row = 0, .column = 1, .text = "\tx"}}) == "a,\"\tx\"\n");
    TEST_CHECK(edited(bytes, Dialect{}, {{.row = 0, .column = 1, .text = "x y"}}) == "a,x y\n");
    const std::vector<std::string> cells{" p", "q"};
    TEST_CHECK(written(insertRow(bytes, Dialect{}, 1, cells)) == "a,b\n\" p\",q\n");
  }

  // On a mismatch, prints the input the writer was given before failing.
  void checkRows(const Rows& actual, const Rows& expected, std::string_view bytes, std::string_view writer) {
    if (actual != expected) {
      std::string shown;
      for (const char c : bytes) {
        shown += c == '\n' ? std::string("\\n") : (c == '\r' ? std::string("\\r") : std::string(1, c));
      }
      std::println(stderr, "{} on \"{}\"", writer, shown);
    }
    TEST_CHECK(actual == expected);
  }

  void checkRowWriters(const std::string& bytes, const Dialect& dialect, const Rows& rows) {
    const Rows lines{{}, {""}, {"x", ""}};
    for (std::size_t row = 0; row <= rows.size(); ++row) {
      for (const Texts& cells : lines) {
        Rows expected = rows;
        expected.insert(expected.begin() + static_cast<std::ptrdiff_t>(row), cells);
        checkRows(tableRows(written(insertRow(bytes, dialect, row, cells)), dialect), expected, bytes, "insertRow");
      }
      if (row < rows.size()) {
        Rows expected = rows;
        expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(row));
        checkRows(tableRows(written(deleteRow(bytes, dialect, row)), dialect), expected, bytes, "deleteRow");
      }
    }
  }

  void checkColumnWriters(const std::string& bytes, const Dialect& dialect, const Rows& rows, std::size_t columns) {
    for (std::size_t column = 0; column <= columns; ++column) {
      Rows expected = rows;
      for (Texts& cells : expected) {
        if (!cells.empty() && cells.size() >= column) {
          cells.insert(cells.begin() + static_cast<std::ptrdiff_t>(column), "");
        }
      }
      checkRows(tableRows(written(insertColumn(bytes, dialect, column)), dialect), expected, bytes, "insertColumn");
      if (column == columns) {
        continue;
      }
      expected = rows;
      for (Texts& cells : expected) {
        if (cells.size() > column) {
          cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(column));
        }
      }
      checkRows(tableRows(written(deleteColumn(bytes, dialect, column)), dialect), expected, bytes, "deleteColumn");
    }
  }

  void checkCellEdits(const std::string& bytes, const Dialect& dialect, const Rows& rows, std::size_t columns) {
    for (std::size_t row = 0; row < rows.size(); ++row) {
      for (std::size_t column = 0; column < std::max<std::size_t>(columns, 1); ++column) {
        for (const std::string& text : {std::string(), std::string(" y")}) {
          Rows expected = rows;
          Texts& cells = expected[row];
          if (column < cells.size() || !text.empty()) {
            cells.resize(std::max(cells.size(), column + 1));
            cells[column] = text;
          }
          checkRows(
              tableRows(edited(bytes, dialect, {{.row = row, .column = column, .text = text}}), dialect), expected,
              bytes, "editCells"
          );
        }
      }
    }
  }

  // Every input up to five bytes over a small alphabet, with each line ending: the rows read back after a writer
  // are the rows read before with that one change.
  void testWritersAgreeWithReader() {
    constexpr std::string_view kAlphabet = "a,\"\n\r";
    constexpr std::size_t kMaxLength = 5;
    std::vector<std::string> inputs{""};
    std::size_t levelStart = 0;
    for (std::size_t length = 1; length <= kMaxLength; ++length) {
      const std::size_t levelEnd = inputs.size();
      for (std::size_t i = levelStart; i < levelEnd; ++i) {
        for (const char c : kAlphabet) {
          inputs.push_back(inputs[i] + c);
        }
      }
      levelStart = levelEnd;
    }
    std::size_t checked = 0;
    for (const RowEnding ending : {RowEnding::Lf, RowEnding::CrLf, RowEnding::Cr}) {
      const Dialect dialect{.lineEnding = ending};
      for (const std::string& bytes : inputs) {
        const Table table = readTable(bytes, dialect);
        if (table.unterminatedQuote) {
          continue;
        }
        const Rows rows = tableRows(bytes, dialect);
        checkRowWriters(bytes, dialect, rows);
        checkColumnWriters(bytes, dialect, rows, table.columnCount);
        checkCellEdits(bytes, dialect, rows, table.columnCount);
        ++checked;
      }
    }
    // 3906 inputs per line ending, minus those with an unterminated quote.
    TEST_CHECK(checked > 3 * 2000);
  }

  // Each hostile operation must finish within this processor time, sanitizers included; held by the benchmark.
  constexpr double kHostileCapMs = 2000.0;
  constexpr std::size_t kMebibyte = 1024 * 1024;

  template <typename Work> double elapsedMs(const Work& work) {
    const std::clock_t start = std::clock();
    work();
    return 1000.0 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
  }

  // The caps guard against work that grows faster than the input, which takes minutes on 1 MiB.
  template <typename Work> bool withinCap(const Work& work) {
    return ::test::withinGuard(elapsedMs(work), kHostileCapMs, "hostile 1 MiB operation, ms");
  }

  void testHostileQuotes() {
    const std::string even(kMebibyte, '"');
    Table table;
    Dialect dialect;
    TEST_CHECK(withinCap([&] { dialect = detectDialect(even); }));
    TEST_CHECK(withinCap([&] { table = readTable(even, dialect); }));
    TEST_CHECK(table.rows.size() == 1);
    TEST_CHECK(table.cells.size() == 1);
    TEST_CHECK(!table.unterminatedQuote);
    const CellText text = cellText(even, dialect, table.cells[0]);
    TEST_CHECK(text.clipped);
    TEST_CHECK(text.text == std::string(kMaxCellTextBytes, '"'));
    TEST_CHECK(withinCap([&] { TEST_CHECK(edited(even, dialect, {}) == even); }));

    const std::string odd(kMebibyte + 1, '"');
    TEST_CHECK(readTable(odd, dialect).unterminatedQuote);
    TEST_CHECK(!editCells(odd, dialect, std::vector<CellEdit>{}).has_value());
  }

  void testHostileSingleLine() {
    std::string line;
    line.reserve(kMebibyte);
    while (line.size() < kMebibyte) {
      line += "a,";
    }
    Table table;
    TEST_CHECK(withinCap([&] { TEST_CHECK(detectDialect(line).delimiter == Delimiter::Comma); }));
    TEST_CHECK(withinCap([&] { table = readTable(line, Dialect{}); }));
    TEST_CHECK(table.rows.size() == 1);
    TEST_CHECK(table.columnCount == TableLimits{}.maxColumns);
    TEST_CHECK(table.truncated);
    std::string out;
    TEST_CHECK(withinCap([&] { out = edited(line, Dialect{}, {{.row = 0, .column = kMebibyte / 2, .text = "z"}}); }));
    TEST_CHECK(out == line + "z");
    TEST_CHECK(withinCap([&] { out = written(deleteColumn(line, Dialect{}, 0)); }));
    TEST_CHECK(out == line.substr(2));

    const std::string letters(kMebibyte, 'a');
    const Table one = readTable(letters, Dialect{});
    TEST_CHECK(one.cells.size() == 1);
    TEST_CHECK(cellText(letters, Dialect{}, one.cells[0]).text.size() == kMaxCellTextBytes);
  }

  void testHostileMillionRows() {
    constexpr std::size_t kRows = 1'000'000;
    std::string column;
    column.reserve(kRows * 2);
    for (std::size_t i = 0; i < kRows; ++i) {
      column += "1\n";
    }
    Table table;
    Dialect dialect;
    TEST_CHECK(withinCap([&] { dialect = detectDialect(column); }));
    TEST_CHECK(dialect.delimiter == Delimiter::Comma);
    TEST_CHECK(!dialect.header);
    TEST_CHECK(withinCap([&] { table = readTable(column, dialect); }));
    TEST_CHECK(table.rows.size() == TableLimits{}.maxRows);
    TEST_CHECK(table.truncated);
    TEST_CHECK(withinCap([&] { TEST_CHECK(columnTypes(column, dialect, table).size() == 1); }));

    std::string out;
    TEST_CHECK(withinCap([&] { out = edited(column, dialect, {{.row = kRows - 1, .column = 0, .text = "x"}}); }));
    TEST_CHECK(out.size() == column.size());
    TEST_CHECK(out.ends_with("1\nx\n"));
    TEST_CHECK(withinCap([&] { out = written(deleteRow(column, dialect, kRows - 1)); }));
    TEST_CHECK(out.size() == column.size() - 2);
    const std::vector<std::string> cells{"2"};
    TEST_CHECK(withinCap([&] { out = written(insertRow(column, dialect, kRows, cells)); }));
    TEST_CHECK(out == column + "2\n");
    TEST_CHECK(withinCap([&] { out = written(insertColumn(column, dialect, 1)); }));
    TEST_CHECK(out.size() == column.size() + kRows);
  }
} // namespace

int main() {
  testRoundTripWithoutEdits();
  testEditOneCell();
  testEditQuoting();
  testEditShortRows();
  testEditErrors();
  testInsertRow();
  testDeleteRow();
  testInsertColumn();
  testDeleteColumn();
  testWritersAtMixedLineEndings();
  testWritersAtUnterminatedLastRow();
  testEditQuotesPaddedText();
  testWritersAgreeWithReader();
  testHostileQuotes();
  testHostileSingleLine();
  testHostileMillionRows();
  std::println("formats delimited write checks passed");
  return 0;
}
