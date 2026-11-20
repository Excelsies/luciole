#pragma once

#include "formats/sheet.h"
#include "formats/table_grid.h"
#include "fs/cancel_token.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luciole {
  enum class SheetFormat : std::uint8_t { Xlsx, Ods };

  // Workbooks larger than this are not previewed.
  inline constexpr std::uint64_t kMaxSheetFileBytes = 64U * 1024U * 1024U;
  // The most the helper may answer with.
  inline constexpr std::size_t kMaxSheetResultBytes = 32U * 1024U * 1024U;

  // The used range of a workbook's sheet as a grid: numbers with up to 15 significant digits, booleans as TRUE and
  // FALSE, dates and times in ISO form, error values as written. Grid cell (0, 0) is the range's first cell.
  formats::TableGrid sheetGrid(const formats::SheetBook& book);

  // The helper's answer, big-endian: "NSR1", the sheet count and each name as a u32 length and bytes, the sheet
  // index, a flag byte (1: truncated, 2: rows cut, 4: columns cut), the first source row and column, the row and
  // column counts, the count of cells that follow, and each cell as row, column, kind byte and text with a u32
  // length. Empty Text cells are left out. A failure is "NSE1" and a u32 length and a short printable message.
  std::string encodeSheetGrid(const formats::TableGrid& grid);
  std::string encodeSheetError(std::string_view message);
  // Reads an answer, checking every count, length, index and order against the grid limits before it is used. A
  // failure that says the workbook is too large or the sheet does not exist gives that sentence; any other failure,
  // and anything malformed, oversized or with bytes left over, says the spreadsheet could not be read.
  std::expected<formats::TableGrid, std::string> decodeSheetResult(std::string_view bytes);

  // Helper command line: the workbook is descriptor 3 and the answer goes to standard output.
  std::vector<std::string> sheetPreviewArguments(SheetFormat format, std::size_t sheetIndex);
  // Private helper protocol: "xlsx" or "ods", then the sheet index. Reads the archive members with ZipMemberLimits
  // and the sheet with SheetLimits, and answers in the format above.
  int runSheetPreviewWorker(std::span<const std::string_view> arguments);

  // Blocking; run on a worker. Reads one sheet of the workbook at `path` in the resource-limited helper. Errors are
  // short sentences fit to show: an empty file, one too large, a sheet that does not exist, or a spreadsheet that
  // could not be read.
  std::expected<formats::TableGrid, std::string> readSheetPreview(
      const std::filesystem::path& path, SheetFormat format, std::size_t sheetIndex, const CancelToken& token
  );
} // namespace luciole
