#include "thumbnails/table_loader.h"

#include "formats/delimited_detect.h"
#include "formats/delimited_table.h"
#include "formats/file_kind.h"
#include "fs/background_job.h"
#include "fs/user_file_save.h"
#include "model/private_key.h"
#include "thumbnails/sheet_preview.h"

#include <algorithm>
#include <array>
#include <utility>

namespace {
  using luciole::TableResult;
  using luciole::formats::TableGrid;

  constexpr std::string_view kNotLocal = "Only local files are shown as tables.";
  using Done = std::move_only_function<void(TableResult)>;

  struct TypeSource {
    std::string_view contentType;
    luciole::TableSource source;
  };

  constexpr std::array kContentTypes{
      TypeSource{"text/csv", luciole::TableSource::Csv},
      TypeSource{"text/x-csv", luciole::TableSource::Csv},
      TypeSource{"application/csv", luciole::TableSource::Csv},
      TypeSource{"text/x-comma-separated-values", luciole::TableSource::Csv},
      TypeSource{"text/tab-separated-values", luciole::TableSource::Tsv},
      TypeSource{"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", luciole::TableSource::Xlsx},
      TypeSource{"application/vnd.oasis.opendocument.spreadsheet", luciole::TableSource::Ods},
  };

  // True when the name is longer than `suffix` and ends with it, ignoring ASCII case.
  bool hasSuffix(std::string_view name, std::string_view suffix) {
    return name.size() > suffix.size()
        && std::ranges::equal(name.substr(name.size() - suffix.size()), suffix, {}, [](char c) {
             return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
           });
  }

  std::string readMessage(const luciole::FileError& error) {
    using luciole::FileErrorKind;
    switch (error.kind) {
    case FileErrorKind::Cancelled:
      return "Reading was cancelled.";
    case FileErrorKind::Missing:
      return "This file no longer exists.";
    case FileErrorKind::Denied:
      return "This file could not be read: permission denied.";
    case FileErrorKind::Unsupported:
      return std::string(kNotLocal);
    default:
      return "This file could not be read.";
    }
  }

  luciole::formats::GridCellKind gridKind(luciole::formats::CellType type) {
    switch (type) {
    case luciole::formats::CellType::Number:
      return luciole::formats::GridCellKind::Number;
    case luciole::formats::CellType::Date:
      return luciole::formats::GridCellKind::Date;
    case luciole::formats::CellType::Text:
      break;
    }
    return luciole::formats::GridCellKind::Text;
  }

  // Runs `work` on a worker and hands its answer to `done` on this thread's main context.
  void answer(const luciole::CancelToken& token, std::move_only_function<TableResult()> work, Done done) {
    luciole::runJob<TableResult>(
        token, std::move(work), [done = std::move(done)](luciole::JobResult<TableResult> result) mutable {
          done(result ? std::move(*result) : TableResult(std::unexpected(readMessage(result.error()))));
        }
    );
  }

  void loadDelimited(luciole::Location location, bool tabs, const luciole::CancelToken& token, Done done) {
    luciole::startUserFileRead(
        std::move(location), luciole::kMaxDelimitedPreviewBytes, token,
        [token, tabs,
         done = std::move(done)](std::expected<luciole::UserFileContent, luciole::FileError> read) mutable {
          if (!read) {
            done(std::unexpected(readMessage(read.error())));
            return;
          }
          answer(
              token,
              [content = std::move(*read), tabs] -> TableResult {
                if (luciole::containsPrivateKey(content.bytes)) {
                  return std::unexpected("This file holds a private key, so it is not shown.");
                }
                const auto dialect = luciole::delimitedDialect(content.bytes, tabs);
                return std::make_shared<const TableGrid>(
                    luciole::delimitedGrid(content.bytes, dialect, content.truncated)
                );
              },
              std::move(done)
          );
        }
    );
  }
} // namespace

std::optional<luciole::TableSource> luciole::tableSource(std::string_view name, std::string_view contentType) {
  const auto* const known = std::ranges::find(kContentTypes, contentType, &TypeSource::contentType);
  if (known != kContentTypes.end()) {
    return known->source;
  }
  const auto language = formats::fileKind(name).language;
  if (language == formats::Language::Csv || language == formats::Language::Tsv) {
    return language == formats::Language::Csv ? TableSource::Csv : TableSource::Tsv;
  }
  if (hasSuffix(name, ".xlsx")) {
    return TableSource::Xlsx;
  }
  if (hasSuffix(name, ".ods")) {
    return TableSource::Ods;
  }
  return std::nullopt;
}

luciole::formats::Dialect luciole::delimitedDialect(std::string_view bytes, bool tabs) {
  formats::DialectHint hint;
  if (tabs) {
    hint.delimiter = formats::Delimiter::Tab;
  }
  return formats::detectDialect(bytes, hint);
}

// The read limits pass a header, a row and a column more than the grid holds, and enough cells that a parse stopped
// by the cell limit holds more rows than the grid, so the grid builder sees which dimension was cut. When a read
// limit stopped the parse before a cut row, that row lies past the grid's last.
luciole::formats::TableGrid luciole::delimitedGrid(std::string_view bytes, const formats::Dialect& dialect, bool cut) {
  const formats::TableLimits limits{
      .maxRows = formats::kMaxGridRows + 2,
      .maxColumns = formats::kMaxGridColumns + 1,
      .maxCells = ((formats::kMaxGridCells / formats::kMaxGridColumns) + 2) * (formats::kMaxGridColumns + 1),
  };
  const formats::Table table = formats::readTable(bytes, dialect, limits);
  const std::size_t rows = table.rows.size() - (cut && !table.rows.empty() ? 1 : 0);
  const std::size_t first = dialect.header && rows > 0 ? 1 : 0;
  formats::TableGridBuilder builder(rows - first, table.columnCount);
  for (std::size_t row = first; row < rows && row - first < builder.rowCount(); ++row) {
    const auto cells = formats::rowCells(table, row);
    for (std::size_t column = 0; column < cells.size(); ++column) {
      const auto text = formats::cellText(bytes, dialect, cells[column], formats::kMaxGridCellBytes).text;
      (void)builder.set(row - first, column, gridKind(formats::cellType(text)), text);
    }
  }
  TableGrid grid = builder.finish();
  if (first == 1) {
    const auto names = formats::rowCells(table, 0);
    for (std::size_t column = 0; column < std::min(names.size(), grid.columnCount); ++column) {
      grid.columnNames.push_back(
          formats::gridText(formats::cellText(bytes, dialect, names[column], formats::kMaxGridCellBytes).text)
      );
    }
  }
  grid.firstSourceRow = first;
  grid.rowsCut = grid.rowsCut || cut;
  grid.truncated = grid.truncated || table.truncated || cut;
  return grid;
}

void luciole::startTableLoad(
    TableRequest request, const CancelToken& token, std::move_only_function<void(TableResult)> done
) {
  const auto source = tableSource(request.name, request.contentType);
  if (source == TableSource::Csv || source == TableSource::Tsv) {
    loadDelimited(std::move(request.location), source == TableSource::Tsv, token, std::move(done));
    return;
  }
  answer(
      token,
      [source, request = std::move(request), token] -> TableResult {
        if (!source) {
          return std::unexpected("This file is not a table.");
        }
        const auto path = request.location.isNative() ? request.location.nativePath() : std::nullopt;
        if (!path) {
          return std::unexpected(std::string(kNotLocal));
        }
        const auto format = source == TableSource::Ods ? SheetFormat::Ods : SheetFormat::Xlsx;
        auto grid = readSheetPreview(*path, format, request.sheetIndex, token);
        if (!grid) {
          return std::unexpected(std::move(grid.error()));
        }
        return std::make_shared<const formats::TableGrid>(std::move(*grid));
      },
      std::move(done)
  );
}
