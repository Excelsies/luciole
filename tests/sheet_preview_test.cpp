#include "fs/cancel_token.h"
#include "table_fixture.h"
#include "test_check.h"
#include "thumbnails/sheet_preview.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <cstdint>
#include <format>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using namespace luciole;
  using namespace luciole::test::table;
  using formats::GridCellKind;
  using formats::SheetCellKind;

  formats::SheetCell cell(std::uint32_t column, SheetCellKind kind, double number, std::string text = {}) {
    formats::SheetCell out;
    out.column = column;
    out.kind = kind;
    out.number = number;
    out.text = std::move(text);
    return out;
  }

  formats::SheetRow row(std::uint32_t index, std::vector<formats::SheetCell> cells) {
    formats::SheetRow out;
    out.index = index;
    out.cells = std::move(cells);
    return out;
  }

  formats::SheetBook sampleBook() {
    formats::SheetBook book;
    book.sheetNames = {"Data", "Two\nlines"};
    book.sheetIndex = 1;
    auto& table = book.table;
    table.rows.push_back(row(1, {cell(1, SheetCellKind::Text, 0, "Name"), cell(3, SheetCellKind::Number, 42.5)}));
    table.rows.push_back(row(
        3, {cell(1, SheetCellKind::Boolean, 1), cell(2, SheetCellKind::Date, 45000), cell(3, SheetCellKind::Time, 0.75)}
    ));
    table.rows.push_back(
        row(4, {cell(2, SheetCellKind::Error, 0, "#DIV/0!"), cell(3, SheetCellKind::Number, 0.1 + 0.2)})
    );
    table.usedRange = formats::SheetRange{.firstRow = 1, .firstColumn = 1, .lastRow = 4, .lastColumn = 3};
    table.truncated = true;
    return book;
  }

  void convertsSheets() {
    const auto grid = sheetGrid(sampleBook());
    TEST_CHECK(grid.rowCount == 4);
    TEST_CHECK(grid.columnCount == 3);
    TEST_CHECK(grid.firstSourceRow == 1 && grid.firstSourceColumn == 1);
    TEST_CHECK(grid.firstRowNumber == 2);
    TEST_CHECK(grid.truncated);
    TEST_CHECK((grid.sheetNames == std::vector<std::string>{"Data", "Two lines"}));
    TEST_CHECK(grid.sheetIndex == 1);
    TEST_CHECK(grid.columnNames.empty());
    TEST_CHECK(formats::gridColumnName(grid, 0) == "B");
    TEST_CHECK(formats::gridCellText(grid, 0, 0) == "Name");
    TEST_CHECK(formats::gridCellText(grid, 0, 2) == "42.5");
    TEST_CHECK(formats::gridCellKind(grid, 0, 2) == GridCellKind::Number);
    TEST_CHECK(formats::gridCellText(grid, 1, 0).empty());
    TEST_CHECK(formats::gridCellText(grid, 2, 0) == "TRUE");
    TEST_CHECK(formats::gridCellKind(grid, 2, 0) == GridCellKind::Boolean);
    TEST_CHECK(formats::gridCellText(grid, 2, 1) == "2023-03-15");
    TEST_CHECK(formats::gridCellKind(grid, 2, 1) == GridCellKind::Date);
    TEST_CHECK(formats::gridCellText(grid, 2, 2) == "18:00:00");
    TEST_CHECK(formats::gridCellKind(grid, 2, 2) == GridCellKind::Time);
    TEST_CHECK(formats::gridCellText(grid, 3, 1) == "#DIV/0!");
    TEST_CHECK(formats::gridCellKind(grid, 3, 1) == GridCellKind::Error);
    TEST_CHECK(formats::gridCellText(grid, 3, 2) == "0.3");

    formats::SheetBook only;
    only.sheetNames = {"Only"};
    const auto empty = sheetGrid(only);
    TEST_CHECK(empty.rowCount == 0 && empty.columnCount == 0 && !empty.truncated);
    TEST_CHECK(empty.sheetNames.size() == 1);
  }

  void roundTrips() {
    const auto grid = sheetGrid(sampleBook());
    const auto decoded = decodeSheetResult(encodeSheetGrid(grid));
    TEST_CHECK(decoded.has_value());
    TEST_CHECK(decoded->sheetNames == grid.sheetNames);
    TEST_CHECK(decoded->sheetIndex == 1);
    TEST_CHECK(decoded->rowCount == 4 && decoded->columnCount == 3);
    TEST_CHECK(decoded->firstSourceRow == 1 && decoded->firstSourceColumn == 1 && decoded->firstRowNumber == 2);
    TEST_CHECK(decoded->truncated && !decoded->rowsCut && !decoded->columnsCut);
    // Which dimensions were cut travels with the grid.
    for (const auto& [rows, columns] : {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
      auto cut = grid;
      cut.rowsCut = rows;
      cut.columnsCut = columns;
      const auto back = decodeSheetResult(encodeSheetGrid(cut));
      TEST_CHECK(back.has_value() && back->truncated && back->rowsCut == rows && back->columnsCut == columns);
    }
    for (std::size_t row = 0; row < grid.rowCount; ++row) {
      for (std::size_t column = 0; column < grid.columnCount; ++column) {
        TEST_CHECK(formats::gridCellText(*decoded, row, column) == formats::gridCellText(grid, row, column));
        TEST_CHECK(formats::gridCellKind(*decoded, row, column) == formats::gridCellKind(grid, row, column));
      }
    }
    TEST_CHECK(
        decodeSheetResult(encodeSheetError("This sheet does not exist.")).error() == "This sheet does not exist."
    );
    TEST_CHECK(
        decodeSheetResult(encodeSheetError("This spreadsheet is too large to preview.")).error()
        == "This spreadsheet is too large to preview."
    );
    // Only the helper's own sentences are shown; any other words read as a spreadsheet that could not be read.
    TEST_CHECK(
        decodeSheetResult(encodeSheetError("the zip member is short")).error() == "This spreadsheet could not be read."
    );
  }

  // Big-endian fields of a hand-made answer.
  struct Answer {
    std::string bytes = "NSR1";
    Answer& u32(std::uint32_t value) {
      for (int shift = 24; shift >= 0; shift -= 8) {
        bytes += static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU);
      }
      return *this;
    }
    Answer& u8(std::uint8_t value) {
      bytes += static_cast<char>(value);
      return *this;
    }
    Answer& text(std::string_view value) {
      u32(static_cast<std::uint32_t>(value.size()));
      bytes += value;
      return *this;
    }
    // One sheet named "S", sheet 0, no flags, the range at (0, 0) of rows by columns, and the cell count.
    static Answer head(std::uint32_t rows, std::uint32_t columns, std::uint32_t cells) {
      Answer answer;
      answer.u32(1).text("S").u32(0).u8(0).u32(0).u32(0).u32(rows).u32(columns).u32(cells);
      return answer;
    }
  };

  void refusesDamagedAnswers() {
    // A damaged answer reads as any other spreadsheet that could not be read.
    constexpr std::string_view kDamaged = "This spreadsheet could not be read.";
    const auto damaged = [&](const std::string& bytes) {
      const auto result = decodeSheetResult(bytes);
      return !result.has_value() && result.error() == kDamaged;
    };
    TEST_CHECK(damaged(""));
    TEST_CHECK(damaged("NSR"));
    TEST_CHECK(damaged("XXXX"));
    TEST_CHECK(damaged("NSR1"));
    TEST_CHECK(damaged("NFR1" + Answer::head(1, 1, 0).bytes.substr(4)));
    // The smallest valid answer decodes; each damage below is one change to it.
    TEST_CHECK(decodeSheetResult(Answer::head(1, 1, 0).bytes).has_value());
    TEST_CHECK(damaged(Answer().u32(0).u32(0).u8(0).u32(0).u32(0).u32(0).u32(0).u32(0).bytes));
    TEST_CHECK(damaged(Answer().u32(0xFFFFFFFFU).bytes));
    TEST_CHECK(damaged(Answer().u32(1).u32(0xFFFFFFF0U).bytes + "S"));
    TEST_CHECK(
        damaged(Answer().u32(1).text(std::string(300, 's')).u32(0).u8(0).u32(0).u32(0).u32(1).u32(1).u32(0).bytes)
    );
    TEST_CHECK(damaged(Answer().u32(1).text("S").u32(1).u8(0).u32(0).u32(0).u32(1).u32(1).u32(0).bytes));
    TEST_CHECK(damaged(Answer().u32(1).text("S").u32(0).u8(9).u32(0).u32(0).u32(1).u32(1).u32(0).bytes));
    TEST_CHECK(damaged(Answer().u32(1).text("S").u32(0).u8(8).u32(0).u32(0).u32(1).u32(1).u32(0).bytes));
    TEST_CHECK(
        decodeSheetResult(Answer().u32(1).text("S").u32(0).u8(7).u32(0).u32(0).u32(1).u32(1).u32(0).bytes).has_value()
    );
    TEST_CHECK(damaged(Answer::head(0xFFFFFFFFU, 1, 0).bytes));
    TEST_CHECK(damaged(Answer::head(1, 0xFFFFFFFFU, 0).bytes));
    TEST_CHECK(damaged(Answer::head(10'000, 256, 0).bytes));
    TEST_CHECK(damaged(Answer::head(2, 2, 5).bytes));
    TEST_CHECK(damaged(Answer::head(2, 2, 0xFFFFFFFFU).bytes));
    TEST_CHECK(damaged(Answer::head(2, 2, 1).u32(2).u32(0).u8(0).text("x").bytes));
    TEST_CHECK(damaged(Answer::head(2, 2, 1).u32(0).u32(2).u8(0).text("x").bytes));
    TEST_CHECK(damaged(Answer::head(2, 2, 1).u32(0).u32(0).u8(6).text("x").bytes));
    TEST_CHECK(damaged(Answer::head(2, 2, 1).u32(0).u32(0).u8(0).text(std::string(257, 'x')).bytes));
    TEST_CHECK(damaged(Answer::head(2, 2, 1).u32(0).u32(0).u8(0).u32(0xFFFFFFFFU).bytes + "x"));
    TEST_CHECK(damaged(Answer::head(2, 2, 2).u32(1).u32(0).u8(0).text("b").u32(0).u32(1).u8(0).text("a").bytes));
    TEST_CHECK(damaged(Answer::head(2, 2, 2).u32(0).u32(1).u8(0).text("a").u32(0).u32(1).u8(0).text("b").bytes));
    TEST_CHECK(damaged(Answer::head(1, 1, 0).bytes + "z"));
    TEST_CHECK(damaged("NSE1" + Answer().u32(0xFFFFFFFFU).bytes.substr(4)));
    TEST_CHECK(damaged("NSE1" + Answer().text("bell\a").bytes.substr(4)));
    TEST_CHECK(damaged("NSE1" + Answer().text(std::string(300, 'm')).bytes.substr(4)));
    TEST_CHECK(damaged("NSE1" + Answer().text("ok").bytes.substr(4) + "more"));

    // Every cut of a valid answer is damaged.
    const std::string whole = encodeSheetGrid(sheetGrid(sampleBook()));
    for (std::size_t size = 0; size < whole.size(); ++size) {
      TEST_CHECK(damaged(whole.substr(0, size)));
    }
    // Random bytes after a valid head give a defined answer, a grid within the limits or the damaged error.
    std::mt19937 random(7);
    std::uniform_int_distribution<int> byte(0, 255);
    for (int round = 0; round < 2000; ++round) {
      std::string noise = round % 2 == 0 ? Answer::head(3, 3, 4).bytes : std::string("NSR1");
      for (int i = 0; i < 64; ++i) {
        noise += static_cast<char>(byte(random));
      }
      const auto result = decodeSheetResult(noise);
      TEST_CHECK(result.has_value() || result.error() == kDamaged);
      if (result) {
        TEST_CHECK(result->rowCount <= formats::kMaxGridRows && result->columnCount <= formats::kMaxGridColumns);
        TEST_CHECK(result->ends.size() == result->rowCount * result->columnCount);
      }
    }
  }

  // Writes `members` as a ZIP archive, with each member named in `zeros` replaced by 65 MiB of deflated zeros whose
  // size the archive states only after the data.
  void writeWithZeros(
      const std::filesystem::path& path, std::vector<ZipMember> members, const std::vector<std::string>& zeros
  ) {
    std::erase_if(members, [&](const ZipMember& member) { return std::ranges::contains(zeros, member.first); });
    archive* out = archive_write_new();
    TEST_CHECK(archive_write_set_format_zip(out) == ARCHIVE_OK);
    TEST_CHECK(archive_write_zip_set_compression_deflate(out) == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(out, path.c_str()) == ARCHIVE_OK);
    for (const auto& [name, bytes] : members) {
      archive_entry* entry = archive_entry_new();
      archive_entry_set_pathname(entry, name.c_str());
      archive_entry_set_filetype(entry, name.ends_with('/') ? AE_IFDIR : AE_IFREG);
      archive_entry_set_size(entry, static_cast<la_int64_t>(bytes.size()));
      TEST_CHECK(archive_write_header(out, entry) == ARCHIVE_OK);
      TEST_CHECK(archive_write_data(out, bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()));
      archive_entry_free(entry);
    }
    const std::string block(1024 * 1024, '\0');
    for (const auto& name : zeros) {
      archive_entry* entry = archive_entry_new();
      archive_entry_set_pathname(entry, name.c_str());
      archive_entry_set_filetype(entry, AE_IFREG);
      TEST_CHECK(archive_write_header(out, entry) == ARCHIVE_OK);
      for (int mib = 0; mib < 65; ++mib) {
        TEST_CHECK(archive_write_data(out, block.data(), block.size()) == static_cast<la_ssize_t>(block.size()));
      }
      archive_entry_free(entry);
    }
    TEST_CHECK(archive_write_close(out) == ARCHIVE_OK);
    archive_write_free(out);
  }

  // Parts the sheet does not need, however large, leave it readable.
  void skipsUnusedMembers(const TemporaryFolder& folder) {
    const auto path = folder.path() / "pivot.xlsx";
    writeWithZeros(
        path, xlsxMembers({{"Small", "<row r=\"1\">" + textCell("A1", "Hello") + "</row>"}, {"Big", ""}}),
        {"xl/worksheets/sheet2.xml", "xl/pivotCache/pivotCacheRecords1.xml"}
    );
    const auto small = readSheetPreview(path, SheetFormat::Xlsx, 0, CancelToken{});
    if (!small) {
      std::println("{}", small.error());
    }
    TEST_CHECK(small.has_value() && formats::gridCellText(*small, 0, 0) == "Hello");
    TEST_CHECK(small->sheetNames.size() == 2);
    TEST_CHECK(
        readSheetPreview(path, SheetFormat::Xlsx, 1, CancelToken{}).error()
        == "This spreadsheet is too large to preview."
    );
  }

  void readsWorkbooks(const TemporaryFolder& folder) {
    const auto path = folder.path() / "book.xlsx";
    writeZip(
        path,
        xlsxMembers(
            {{"First", "<row r=\"1\">" + textCell("A1", "Name") + numberCell("B1", 3.5) + "</row>"},
             {"Second", "<row r=\"2\">" + textCell("C2", "Later") + "</row>"}}
        )
    );
    const auto first = readSheetPreview(path, SheetFormat::Xlsx, 0, CancelToken{});
    if (!first) {
      std::println("{}", first.error());
    }
    TEST_CHECK(first.has_value());
    TEST_CHECK((first->sheetNames == std::vector<std::string>{"First", "Second"}));
    TEST_CHECK(first->sheetIndex == 0);
    TEST_CHECK(first->rowCount == 1 && first->columnCount == 2);
    TEST_CHECK(formats::gridCellText(*first, 0, 0) == "Name");
    TEST_CHECK(formats::gridCellText(*first, 0, 1) == "3.5");
    TEST_CHECK(formats::gridCellKind(*first, 0, 1) == GridCellKind::Number);

    const auto second = readSheetPreview(path, SheetFormat::Xlsx, 1, CancelToken{});
    TEST_CHECK(second.has_value() && second->sheetIndex == 1);
    TEST_CHECK(second->firstSourceRow == 1 && second->firstSourceColumn == 2 && second->firstRowNumber == 2);
    TEST_CHECK(formats::gridColumnName(*second, 0) == "C");
    TEST_CHECK(formats::gridCellText(*second, 0, 0) == "Later");

    TEST_CHECK(readSheetPreview(path, SheetFormat::Xlsx, 2, CancelToken{}).error() == "This sheet does not exist.");
    TEST_CHECK(
        readSheetPreview(path, SheetFormat::Ods, 0, CancelToken{}).error() == "This spreadsheet could not be read."
    );

    const auto ods = folder.path() / "book.ods";
    writeZip(
        ods,
        odsMembers(
            {{"Sheet A",
              R"(<table:table-row><table:table-cell office:value-type="float" )"
              R"(office:value="12"/><table:table-cell><text:p>x</text:p></table:table-cell>)"
              "</table:table-row>"}}
        )
    );
    const auto odsGrid = readSheetPreview(ods, SheetFormat::Ods, 0, CancelToken{});
    TEST_CHECK(odsGrid.has_value());
    TEST_CHECK((odsGrid->sheetNames == std::vector<std::string>{"Sheet A"}));
    TEST_CHECK(formats::gridCellText(*odsGrid, 0, 0) == "12");
    TEST_CHECK(formats::gridCellKind(*odsGrid, 0, 0) == GridCellKind::Number);
    TEST_CHECK(formats::gridCellText(*odsGrid, 0, 1) == "x");

    CancelToken cancelled;
    cancelled.cancel();
    TEST_CHECK(!readSheetPreview(path, SheetFormat::Xlsx, 0, cancelled).has_value());
  }

  void refusesHostileFiles(const TemporaryFolder& folder) {
    const auto text = folder.path() / "fake.xlsx";
    writeFile(text, "name,size\nnot,a workbook\n");
    TEST_CHECK(
        readSheetPreview(text, SheetFormat::Xlsx, 0, CancelToken{}).error() == "This spreadsheet could not be read."
    );
    TEST_CHECK(
        readSheetPreview(folder.path() / "missing.xlsx", SheetFormat::Xlsx, 0, CancelToken{}).error()
        == "This spreadsheet could not be read."
    );
    TEST_CHECK(
        readSheetPreview(folder.path(), SheetFormat::Xlsx, 0, CancelToken{}).error()
        == "This spreadsheet could not be read."
    );
    // An empty file says so, as an empty model does.
    for (const auto* name : {"empty.xlsx", "empty.ods"}) {
      const auto empty = folder.path() / name;
      writeFile(empty, "");
      const auto format = std::string_view(name).ends_with(".ods") ? SheetFormat::Ods : SheetFormat::Xlsx;
      TEST_CHECK(readSheetPreview(empty, format, 0, CancelToken{}).error() == "This spreadsheet file is empty.");
    }

    // A sparse file past the size limit is refused before any helper starts.
    const auto huge = folder.path() / "huge.xlsx";
    writeFile(huge, "");
    std::filesystem::resize_file(huge, kMaxSheetFileBytes + 1);
    TEST_CHECK(
        readSheetPreview(huge, SheetFormat::Xlsx, 0, CancelToken{}).error()
        == "This spreadsheet is too large to preview."
    );

    // A worksheet of deflated zeros that unpacks past the member limit.
    const auto bomb = folder.path() / "bomb.xlsx";
    writeWithZeros(bomb, xlsxMembers({{"Big", ""}}), {"xl/worksheets/sheet1.xml"});
    TEST_CHECK(std::filesystem::file_size(bomb) < 1024 * 1024);
    TEST_CHECK(
        readSheetPreview(bomb, SheetFormat::Xlsx, 0, CancelToken{}).error()
        == "This spreadsheet is too large to preview."
    );
  }
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (!args.empty() && args.front() == "--sheet-preview-worker") {
    return luciole::runSheetPreviewWorker(std::span(args).subspan(1));
  }
  convertsSheets();
  roundTrips();
  refusesDamagedAnswers();
  const TemporaryFolder folder;
  readsWorkbooks(folder);
  skipsUnusedMembers(folder);
  refusesHostileFiles(folder);
  std::println("sheet-preview: all checks passed");
  return 0;
}
