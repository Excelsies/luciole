#pragma once

#include "formats/delimited_table.h"
#include "formats/table_grid.h"
#include "fs/cancel_token.h"
#include "fs/location.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace luciole {
  enum class TableSource : std::uint8_t { Csv, Tsv, Xlsx, Ods };

  // Delimited text is read up to this many bytes for a table; a longer file shows the rows that fit, marked cut.
  inline constexpr std::size_t kMaxDelimitedPreviewBytes = 8U * 1024U * 1024U;

  // The table a file holds, from its content type, then from its name; none for other files.
  std::optional<TableSource> tableSource(std::string_view name, std::string_view contentType);

  struct TableRequest {
    Location location;
    std::string name;
    std::string contentType;
    std::size_t sheetIndex = 0; // the workbook sheet to show
  };

  // The table, or a short sentence fit to show.
  using TableResult = std::expected<std::shared_ptr<const formats::TableGrid>, std::string>;

  // The dialect of delimited text, detected from its first bytes; `tabs` makes it split at tabs, as a TSV does.
  formats::Dialect delimitedDialect(std::string_view bytes, bool tabs);

  // The grid of delimited text read with `dialect`: a header row gives the column names, and each cell is typed as a
  // number, a date or text. `cut` says the bytes stop short of the file: their last row, which the cut may run
  // through, is left out and the grid is marked cut. Time is linear in the bytes.
  formats::TableGrid delimitedGrid(std::string_view bytes, const formats::Dialect& dialect, bool cut = false);

  // Reads a local file as a table on workers. CSV and TSV are read with startUserFileRead up to
  // kMaxDelimitedPreviewBytes, their dialect detected (TSV always splits at tabs), a detected header row taken as the
  // column names and each cell typed as a number, a date or text. XLSX and ODS are read in the resource-limited
  // helper (readSheetPreview). Text holding private key material is refused. `done` runs once on the calling
  // thread's main context, also after cancellation; callers check their own lifetime there.
  void startTableLoad(TableRequest request, const CancelToken& token, std::move_only_function<void(TableResult)> done);
} // namespace luciole
