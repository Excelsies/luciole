#include "fs/cancel_token.h"
#include "fs/location.h"
#include "table_fixture.h"
#include "test_check.h"
#include "thumbnails/sheet_preview.h"
#include "thumbnails/table_loader.h"

#include <chrono>
#include <format>
#include <gio/gio.h>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
  using namespace luciole;
  using namespace luciole::test::table;
  using formats::GridCellKind;

  // Starts a load and runs the main context until it answers; fails when it answers twice.
  TableResult load(TableRequest request, const CancelToken& token = {}) {
    std::optional<TableResult> result;
    int answers = 0;
    startTableLoad(std::move(request), token, [&](TableResult value) {
      ++answers;
      result = std::move(value);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!result) {
      while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // A late second answer would arrive in these iterations.
    for (int i = 0; i < 20; ++i) {
      g_main_context_iteration(nullptr, FALSE);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(answers == 1);
    return std::move(*result);
  }

  TableRequest request(const std::filesystem::path& path, std::string_view contentType = {}) {
    return {
        .location = Location::fromPath(path.string()),
        .name = path.filename().string(),
        .contentType = std::string(contentType),
        .sheetIndex = 0,
    };
  }

  void choosesSources() {
    TEST_CHECK(tableSource("a.csv", "text/csv") == TableSource::Csv);
    TEST_CHECK(tableSource("a.bin", "text/csv") == TableSource::Csv);
    TEST_CHECK(tableSource("A.CSV", "text/plain") == TableSource::Csv);
    TEST_CHECK(tableSource("a.tsv", "text/plain") == TableSource::Tsv);
    TEST_CHECK(tableSource("a", "text/tab-separated-values") == TableSource::Tsv);
    TEST_CHECK(
        tableSource("a", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet") == TableSource::Xlsx
    );
    TEST_CHECK(tableSource("Book.XLSX", "application/zip") == TableSource::Xlsx);
    TEST_CHECK(tableSource("a", "application/vnd.oasis.opendocument.spreadsheet") == TableSource::Ods);
    TEST_CHECK(tableSource("b.ods", "application/octet-stream") == TableSource::Ods);
    TEST_CHECK(!tableSource("notes.txt", "text/plain"));
    TEST_CHECK(!tableSource("", ""));
    TEST_CHECK(!tableSource(".xlsx", ""));
  }

  void readsCsv(const TemporaryFolder& folder) {
    const auto path = folder.path() / "people.csv";
    writeFile(path, "Name,Size,When\nalpha,12,2024-01-02\n\"beta, b\",7.5,2023-12-31\n");
    const auto result = load(request(path, "text/csv"));
    TEST_CHECK(result.has_value());
    const auto& grid = **result;
    TEST_CHECK((grid.columnNames == std::vector<std::string>{"Name", "Size", "When"}));
    TEST_CHECK(grid.rowCount == 2 && grid.columnCount == 3);
    TEST_CHECK(grid.firstSourceRow == 1 && grid.firstSourceColumn == 0 && grid.firstRowNumber == 1);
    TEST_CHECK(grid.sheetNames.empty());
    TEST_CHECK(!grid.truncated && !grid.rowsCut && !grid.columnsCut);
    TEST_CHECK(formats::gridCellText(grid, 1, 0) == "beta, b");
    TEST_CHECK(formats::gridCellKind(grid, 1, 0) == GridCellKind::Text);
    TEST_CHECK(formats::gridCellText(grid, 0, 1) == "12");
    TEST_CHECK(formats::gridCellKind(grid, 0, 1) == GridCellKind::Number);
    TEST_CHECK(formats::gridCellKind(grid, 1, 2) == GridCellKind::Date);

    const auto numbers = folder.path() / "numbers.csv";
    writeFile(numbers, "1,2\n3,4\n");
    const auto plain = load(request(numbers, "text/csv"));
    TEST_CHECK(plain.has_value() && (*plain)->columnNames.empty());
    TEST_CHECK((*plain)->rowCount == 2 && (*plain)->firstSourceRow == 0);
    TEST_CHECK(formats::gridColumnName(**plain, 1) == "B");

    // The name asks for tabs even when the content type does not.
    const auto tabs = folder.path() / "pairs.tsv";
    writeFile(tabs, "key\tvalue, with comma\nk\t1\n");
    const auto tsv = load(request(tabs, "text/plain"));
    TEST_CHECK(tsv.has_value() && (*tsv)->columnCount == 2);
    TEST_CHECK(formats::gridColumnName(**tsv, 1) == "value, with comma");
  }

  void cutsLongText(const TemporaryFolder& folder) {
    // Rows past the grid limit.
    std::string rows = "n\n";
    for (std::size_t i = 0; i <= formats::kMaxGridRows; ++i) {
      rows += std::format("{}\n", i);
    }
    const auto tall = folder.path() / "tall.csv";
    writeFile(tall, rows);
    const auto many = load(request(tall, "text/csv"));
    TEST_CHECK(many.has_value() && (*many)->truncated && (*many)->rowsCut && !(*many)->columnsCut);
    TEST_CHECK((*many)->rowCount == formats::kMaxGridRows);

    // Columns past the grid limit.
    std::string names;
    std::string values;
    for (std::size_t i = 0; i < formats::kMaxGridColumns + 4; ++i) {
      names += std::format("{}c{}", i == 0 ? "" : ",", i);
      values += std::format("{}{}", i == 0 ? "" : ",", i);
    }
    const auto broad = folder.path() / "broad.csv";
    writeFile(broad, names + "\n" + values + "\n");
    const auto across = load(request(broad, "text/csv"));
    TEST_CHECK(across.has_value() && (*across)->columnsCut && !(*across)->rowsCut);
    TEST_CHECK((*across)->columnCount == formats::kMaxGridColumns && (*across)->rowCount == 1);

    // Wide rows reach the cell limit before the row limit: rows are cut too, and each row shown is whole.
    std::string block = names + "\n";
    for (std::size_t row = 0; row < 4'000; ++row) {
      block += std::format("{}{}\n", row, values.substr(1));
    }
    writeFile(broad, block);
    const auto square = load(request(broad, "text/csv"));
    constexpr std::size_t kRowsShown = formats::kMaxGridCells / formats::kMaxGridColumns;
    TEST_CHECK(square.has_value() && (*square)->columnsCut && (*square)->rowsCut);
    TEST_CHECK((*square)->rowCount == kRowsShown && (*square)->columnCount == formats::kMaxGridColumns);
    TEST_CHECK(formats::gridCellText(**square, kRowsShown - 1, 0) == std::to_string(kRowsShown - 1));
    TEST_CHECK(formats::gridCellText(**square, kRowsShown - 1, 255) == "255");

    // Bytes and rows past both limits: the row limit is reached first, so every row shown is whole.
    std::string longer = "n,text\n";
    for (std::size_t i = 0; longer.size() <= kMaxDelimitedPreviewBytes; ++i) {
      longer += std::format("{},{}\n", i, std::string(90, 'x'));
    }
    const auto both = folder.path() / "both.csv";
    writeFile(both, longer);
    const auto first = load(request(both, "text/csv"));
    TEST_CHECK(first.has_value() && (*first)->rowsCut && !(*first)->columnsCut);
    TEST_CHECK((*first)->rowCount == formats::kMaxGridRows);
    TEST_CHECK(formats::gridCellText(**first, formats::kMaxGridRows - 1, 0) == "9999");

    // Bytes past the read limit: the row the limit cuts through is left out.
    const std::string line = "name," + std::string(994, 'x') + "\n";
    TEST_CHECK(line.size() == 1000);
    std::string big;
    for (int i = 0; i < 8500; ++i) {
      big += line;
    }
    const auto wide = folder.path() / "big.csv";
    writeFile(wide, big);
    const auto cut = load(request(wide, "text/csv"));
    TEST_CHECK(cut.has_value() && (*cut)->truncated && (*cut)->rowsCut && !(*cut)->columnsCut);
    const std::size_t whole = kMaxDelimitedPreviewBytes / line.size();
    TEST_CHECK((*cut)->rowCount + ((*cut)->columnNames.empty() ? 0 : 1) == whole);
  }

  // The dialect and the grid from bytes already read, as an editor rebuilds them after a change.
  void gridsFromBytes() {
    TEST_CHECK(delimitedDialect("a;b;c\n1;2;3\n4;5;6\n", false).delimiter == formats::Delimiter::Semicolon);
    TEST_CHECK(delimitedDialect("a,b\n1,2\n", false).delimiter == formats::Delimiter::Comma);
    TEST_CHECK(delimitedDialect("a,b\n1,2\n", true).delimiter == formats::Delimiter::Tab);
    const auto crlf = delimitedDialect("\xEF\xBB\xBFName,Size\r\nalpha,12\r\n", false);
    TEST_CHECK(crlf.byteOrderMark && crlf.header && crlf.lineEnding == formats::RowEnding::CrLf);

    const std::string bytes = "Name;Size\nalpha;12\n\"be;ta\";7\n";
    const auto dialect = delimitedDialect(bytes, false);
    const auto grid = delimitedGrid(bytes, dialect);
    TEST_CHECK((grid.columnNames == std::vector<std::string>{"Name", "Size"}));
    TEST_CHECK(grid.rowCount == 2 && grid.columnCount == 2 && grid.firstSourceRow == 1);
    TEST_CHECK(!grid.truncated && !grid.rowsCut);
    TEST_CHECK(formats::gridCellText(grid, 1, 0) == "be;ta" && formats::gridCellText(grid, 0, 1) == "12");
    TEST_CHECK(formats::gridCellKind(grid, 0, 1) == GridCellKind::Number);

    // The same dialect reads a changed copy, whatever detection would make of it now.
    const auto changed = delimitedGrid("Name;Size\nalpha,x;12\n", dialect);
    TEST_CHECK(changed.rowCount == 1 && formats::gridCellText(changed, 0, 0) == "alpha,x");

    // Bytes that stop short of the file lose their last row and are marked cut.
    const auto cut = delimitedGrid(bytes, dialect, true);
    TEST_CHECK(cut.rowCount == 1 && cut.truncated && cut.rowsCut);

    TEST_CHECK(delimitedGrid("", formats::Dialect{}).rowCount == 0);
  }

  void readsWorkbooks(const TemporaryFolder& folder) {
    const auto path = folder.path() / "book.xlsx";
    writeZip(
        path,
        xlsxMembers(
            {{"One", "<row r=\"1\">" + numberCell("A1", 1) + "</row>"},
             {"Two", "<row r=\"4\">" + textCell("B4", "four") + "</row>"}}
        )
    );
    auto second = request(path, "application/zip");
    second.sheetIndex = 1;
    const auto grid = load(second);
    TEST_CHECK(grid.has_value());
    TEST_CHECK((*grid)->sheetIndex == 1 && (*grid)->sheetNames.size() == 2);
    TEST_CHECK((*grid)->firstRowNumber == 4);
    TEST_CHECK(formats::gridCellText(**grid, 0, 0) == "four");

    const auto ods = folder.path() / "book.ods";
    writeZip(
        ods,
        odsMembers(
            {{"S",
              "<table:table-row><table:table-cell><text:p>t</text:p></table:table-cell>"
              "</table:table-row>"}}
        )
    );
    const auto odsGrid = load(request(ods));
    TEST_CHECK(odsGrid.has_value() && formats::gridCellText(**odsGrid, 0, 0) == "t");
  }

  void reportsErrors(const TemporaryFolder& folder) {
    TEST_CHECK(load(request(folder.path() / "gone.csv", "text/csv")).error() == "This file no longer exists.");
    TEST_CHECK(load(request(folder.path() / "gone.xlsx")).error() == "This spreadsheet could not be read.");
    const auto notes = folder.path() / "notes.txt";
    writeFile(notes, "plain");
    TEST_CHECK(load(request(notes, "text/plain")).error() == "This file is not a table.");

    const auto key = folder.path() / "keys.csv";
    writeFile(key, "name,key\nhost,\"-----BEGIN OPENSSH PRIVATE KEY-----\"\n");
    TEST_CHECK(load(request(key, "text/csv")).error() == "This file holds a private key, so it is not shown.");

    // A spreadsheet is read from compressed XML, so the check runs on the cells that would be shown.
    const auto sheetKey = folder.path() / "keys.xlsx";
    writeZip(
        sheetKey,
        xlsxMembers({{"Keys", "<row r=\"1\">" + textCell("A1", "-----BEGIN OPENSSH PRIVATE KEY-----") + "</row>"}})
    );
    TEST_CHECK(load(request(sheetKey)).error() == "This file holds a private key, so it is not shown.");
    const auto odsKey = folder.path() / "keys.ods";
    writeZip(
        odsKey,
        odsMembers(
            {{"S",
              "<table:table-row><table:table-cell><text:p>PrivateKey = "
              "yAnzfakeWireGuardKeyForTestsOnlyAAAAAAAAAAA=</text:p>"
              "</table:table-cell></table:table-row>"}}
        )
    );
    TEST_CHECK(load(request(odsKey)).error() == "This file holds a private key, so it is not shown.");

    TableRequest remote{.location = Location::trash().child("a.csv"), .name = "a.csv", .contentType = "text/csv"};
    TEST_CHECK(load(remote).error() == "Only local files are shown as tables.");
    remote.name = "a.xlsx";
    TEST_CHECK(load(remote).error() == "Only local files are shown as tables.");

    CancelToken cancelled;
    cancelled.cancel();
    const auto table = folder.path() / "fine.csv";
    writeFile(table, "a,b\n1,2\n");
    TEST_CHECK(load(request(table, "text/csv"), cancelled).error() == "Reading was cancelled.");
    TEST_CHECK(load(request(folder.path() / "book.xlsx"), cancelled).error() == "Reading was cancelled.");
  }
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (!args.empty() && args.front() == "--sheet-preview-worker") {
    return luciole::runSheetPreviewWorker(std::span(args).subspan(1));
  }
  choosesSources();
  gridsFromBytes();
  const TemporaryFolder folder;
  readsCsv(folder);
  cutsLongText(folder);
  readsWorkbooks(folder);
  reportsErrors(folder);
  std::println("table-loader: all checks passed");
  return 0;
}
