#include "thumbnails/sheet_preview.h"

#include "formats/sheet_ods.h"
#include "formats/sheet_xlsx.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "thumbnails/provider_process.h"
#include "thumbnails/zip_members.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <format>
#include <optional>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  using luciole::formats::GridCellKind;
  using luciole::formats::TableGrid;

  constexpr std::string_view kResultMagic = "NSR1";
  constexpr std::string_view kErrorMagic = "NSE1";
  constexpr std::size_t kMaxMessageBytes = 256;
  constexpr std::string_view kUnreadable = "This spreadsheet could not be read.";
  constexpr std::string_view kTooLarge = "This spreadsheet is too large to preview.";
  constexpr std::string_view kEmpty = "This spreadsheet file is empty.";
  constexpr std::string_view kNoSheet = "This sheet does not exist.";
  constexpr std::string_view kCancelled = "Reading was cancelled.";
  // Bits of the answer's flag byte.
  constexpr std::uint8_t kTruncatedFlag = 1;
  constexpr std::uint8_t kRowsCutFlag = 2;
  constexpr std::uint8_t kColumnsCutFlag = 4;
  constexpr std::uint8_t kAllFlags = kTruncatedFlag | kRowsCutFlag | kColumnsCutFlag;

  std::unexpected<std::string> failure(std::string_view message) { return std::unexpected(std::string(message)); }

  // Up to 15 significant digits, so binary fractions such as 0.1 + 0.2 read as written.
  std::string numberText(double value) { return value == 0 ? "0" : std::format("{:.15g}", value); }

  struct CellValue {
    GridCellKind kind = GridCellKind::Text;
    std::string text;
  };

  CellValue cellValue(const luciole::formats::SheetCell& cell, luciole::formats::DateSystem system) {
    using luciole::formats::SheetCellKind;
    switch (cell.kind) {
    case SheetCellKind::Number:
      return {.kind = GridCellKind::Number, .text = numberText(cell.number)};
    case SheetCellKind::Boolean:
      return {.kind = GridCellKind::Boolean, .text = cell.number != 0 ? "TRUE" : "FALSE"};
    case SheetCellKind::Date:
      return {
          .kind = GridCellKind::Date,
          .text = luciole::formats::formatSheetDate(cell.number, system).value_or(numberText(cell.number))
      };
    case SheetCellKind::Time:
      return {
          .kind = GridCellKind::Time,
          .text = luciole::formats::formatSheetTime(cell.number).value_or(numberText(cell.number))
      };
    case SheetCellKind::Error:
      return {.kind = GridCellKind::Error, .text = cell.text};
    case SheetCellKind::Text:
      break;
    }
    return {.kind = GridCellKind::Text, .text = cell.text};
  }

  void putU32(std::string& out, std::size_t value) {
    const auto field = static_cast<std::uint32_t>(value);
    for (unsigned shift = 32; shift > 0; shift -= 8) {
      out += static_cast<char>((field >> (shift - 8)) & 0xFFU);
    }
  }

  void putText(std::string& out, std::string_view text) {
    putU32(out, text.size());
    out += text;
  }

  // Reads big-endian fields; every read checks the bytes left first.
  class AnswerReader {
  public:
    explicit AnswerReader(std::string_view bytes) : m_rest(bytes) {}

    bool empty() const noexcept { return m_rest.empty(); }

    std::optional<std::uint32_t> u32() {
      if (m_rest.size() < 4) {
        return std::nullopt;
      }
      std::uint32_t value = 0;
      for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8U) | static_cast<unsigned char>(m_rest[i]);
      }
      m_rest.remove_prefix(4);
      return value;
    }

    std::optional<std::uint8_t> u8() {
      if (m_rest.empty()) {
        return std::nullopt;
      }
      const auto value = static_cast<std::uint8_t>(m_rest.front());
      m_rest.remove_prefix(1);
      return value;
    }

    // A u32 length of at most `maxBytes`, then that many bytes.
    std::optional<std::string_view> text(std::size_t maxBytes) {
      const auto size = u32();
      if (!size || *size > maxBytes || *size > m_rest.size()) {
        return std::nullopt;
      }
      const std::string_view value = m_rest.substr(0, *size);
      m_rest.remove_prefix(*size);
      return value;
    }

  private:
    std::string_view m_rest;
  };

  // The cells after the counts of a result answer.
  std::optional<TableGrid> readCells(AnswerReader& reader, std::uint32_t rows, std::uint32_t columns) {
    const auto cells = reader.u32();
    if (!cells || *cells > std::size_t{rows} * columns) {
      return std::nullopt;
    }
    luciole::formats::TableGridBuilder builder(rows, columns);
    for (std::uint32_t i = 0; i < *cells; ++i) {
      const auto row = reader.u32();
      const auto column = reader.u32();
      const auto kind = reader.u8();
      const auto text = reader.text(luciole::formats::kMaxGridCellBytes);
      if (!row
          || !column
          || !kind
          || !text
          || *kind >= luciole::formats::kGridCellKindCount
          || !builder.set(*row, *column, static_cast<GridCellKind>(*kind), *text)) {
        return std::nullopt;
      }
    }
    return builder.finish();
  }

  std::optional<TableGrid> readResult(AnswerReader& reader) {
    const auto sheets = reader.u32();
    if (!sheets || *sheets == 0 || *sheets > luciole::formats::kMaxGridSheets) {
      return std::nullopt;
    }
    std::vector<std::string> names;
    for (std::uint32_t i = 0; i < *sheets; ++i) {
      const auto name = reader.text(luciole::formats::kMaxGridCellBytes);
      if (!name) {
        return std::nullopt;
      }
      names.push_back(luciole::formats::gridText(*name));
    }
    const auto index = reader.u32();
    const auto flags = reader.u8();
    const auto firstRow = reader.u32();
    const auto firstColumn = reader.u32();
    const auto rows = reader.u32();
    const auto columns = reader.u32();
    if (!index
        || *index >= *sheets
        || !flags
        || (*flags & ~kAllFlags) != 0
        || !firstRow
        || !firstColumn
        || !rows
        || !columns
        || *rows > luciole::formats::kMaxGridRows
        || *columns > luciole::formats::kMaxGridColumns
        || std::size_t{*rows} * *columns > luciole::formats::kMaxGridCells) {
      return std::nullopt;
    }
    auto grid = readCells(reader, *rows, *columns);
    if (!grid || !reader.empty()) {
      return std::nullopt;
    }
    grid->sheetNames = std::move(names);
    grid->sheetIndex = *index;
    grid->firstSourceRow = *firstRow;
    grid->firstSourceColumn = *firstColumn;
    grid->firstRowNumber = std::size_t{*firstRow} + 1;
    grid->rowsCut = grid->rowsCut || (*flags & kRowsCutFlag) != 0;
    grid->columnsCut = grid->columnsCut || (*flags & kColumnsCutFlag) != 0;
    grid->truncated = grid->truncated || grid->rowsCut || grid->columnsCut || (*flags & kTruncatedFlag) != 0;
    return grid;
  }

  bool writeAll(int fd, std::string_view bytes) {
    std::size_t offset = 0;
    // Each pass writes at least one byte or is an interruption; interruptions are bounded too.
    for (std::size_t pass = 0; offset < bytes.size() && pass <= bytes.size() + 64; ++pass) {
      const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return false;
      }
      offset += static_cast<std::size_t>(count);
    }
    return offset == bytes.size();
  }

  std::optional<std::string> readAll(int fd, std::size_t size) {
    std::string bytes(size, '\0');
    std::size_t offset = 0;
    for (std::size_t pass = 0; offset < size && pass <= size + 64; ++pass) {
      const auto count = ::pread(fd, bytes.data() + offset, size - offset, static_cast<off_t>(offset));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return std::nullopt;
      }
      offset += static_cast<std::size_t>(count);
    }
    return offset == size ? std::optional(std::move(bytes)) : std::nullopt;
  }

  // The answer for descriptor 3: the sheet's grid, or a failure message. Each member the reader asks for is read in
  // a pass of its own, so members it never asks for count toward no limit; the total limit spans the passes.
  std::string sheetAnswer(luciole::SheetFormat format, std::size_t sheetIndex) {
    using luciole::ZipMemberError;
    const luciole::ZipMemberLimits bounds;
    std::size_t kept = 0;
    std::optional<ZipMemberError> failed;
    const luciole::formats::MemberReader reader = [&](std::string_view name) -> std::optional<std::string> {
      if (failed) {
        return std::nullopt;
      }
      luciole::ZipMemberLimits limits = bounds;
      limits.maxTotalBytes = bounds.maxTotalBytes - kept;
      const auto members =
          luciole::readZipMembers(3, [name](std::string_view member) { return member == name; }, limits);
      if (!members) {
        failed = members.error();
        return std::nullopt;
      }
      const std::string* bytes = members->find(name);
      if (bytes == nullptr) {
        return std::nullopt;
      }
      kept += bytes->size();
      return *bytes;
    };
    const auto book = format == luciole::SheetFormat::Ods ? luciole::formats::readOds(reader, sheetIndex)
                                                          : luciole::formats::readXlsx(reader, sheetIndex);
    if (failed) {
      const bool damaged = failed == ZipMemberError::Unreadable || failed == ZipMemberError::DuplicateName;
      return luciole::encodeSheetError(damaged ? kUnreadable : kTooLarge);
    }
    if (!book) {
      return luciole::encodeSheetError(
          book.error().kind == luciole::formats::SheetErrorKind::SheetIndex ? kNoSheet : kUnreadable
      );
    }
    return luciole::encodeSheetGrid(luciole::sheetGrid(*book));
  }
} // namespace

luciole::formats::TableGrid luciole::sheetGrid(const formats::SheetBook& book) {
  const auto& table = book.table;
  TableGrid grid;
  if (table.usedRange) {
    const auto& range = *table.usedRange;
    formats::TableGridBuilder builder(
        std::size_t{range.lastRow} - range.firstRow + 1, std::size_t{range.lastColumn} - range.firstColumn + 1
    );
    for (const auto& row : table.rows) {
      for (const auto& cell : row.cells) {
        auto value = cellValue(cell, table.dateSystem);
        // Rows and cells come in ascending order inside the range, so the builder takes every one.
        (void)builder.set(row.index - range.firstRow, cell.column - range.firstColumn, value.kind, value.text);
      }
    }
    grid = builder.finish();
    grid.firstSourceRow = range.firstRow;
    grid.firstSourceColumn = range.firstColumn;
    grid.firstRowNumber = std::size_t{range.firstRow} + 1;
  }
  grid.truncated = grid.truncated || table.truncated;
  const std::size_t names = std::min(book.sheetNames.size(), formats::kMaxGridSheets);
  for (std::size_t i = 0; i < names; ++i) {
    grid.sheetNames.push_back(formats::gridText(book.sheetNames[i]));
  }
  grid.sheetIndex = book.sheetIndex;
  return grid;
}

std::string luciole::encodeSheetGrid(const formats::TableGrid& grid) {
  std::string out(kResultMagic);
  putU32(out, grid.sheetNames.size());
  for (const auto& name : grid.sheetNames) {
    putText(out, name);
  }
  putU32(out, grid.sheetIndex);
  const auto flags = static_cast<std::uint8_t>(
      (grid.truncated ? kTruncatedFlag : 0)
      | (grid.rowsCut ? kRowsCutFlag : 0)
      | (grid.columnsCut ? kColumnsCutFlag : 0)
  );
  out += static_cast<char>(flags);
  putU32(out, grid.firstSourceRow);
  putU32(out, grid.firstSourceColumn);
  putU32(out, grid.rowCount);
  putU32(out, grid.columnCount);
  std::string cells;
  std::size_t count = 0;
  for (std::size_t row = 0; row < grid.rowCount; ++row) {
    for (std::size_t column = 0; column < grid.columnCount; ++column) {
      const auto text = formats::gridCellText(grid, row, column);
      const auto kind = formats::gridCellKind(grid, row, column);
      if (text.empty() && kind == GridCellKind::Text) {
        continue;
      }
      putU32(cells, row);
      putU32(cells, column);
      cells += static_cast<char>(kind);
      putText(cells, text);
      ++count;
    }
  }
  putU32(out, count);
  return out + cells;
}

std::string luciole::encodeSheetError(std::string_view message) {
  std::string out(kErrorMagic);
  putText(out, message.substr(0, kMaxMessageBytes));
  return out;
}

std::expected<luciole::formats::TableGrid, std::string> luciole::decodeSheetResult(std::string_view bytes) {
  AnswerReader reader(bytes.substr(std::min<std::size_t>(bytes.size(), 4)));
  if (bytes.starts_with(kErrorMagic)) {
    const auto message = reader.text(kMaxMessageBytes);
    const bool known = message && (*message == kTooLarge || *message == kNoSheet);
    return failure(known && reader.empty() ? *message : kUnreadable);
  }
  if (!bytes.starts_with(kResultMagic)) {
    return failure(kUnreadable);
  }
  auto grid = readResult(reader);
  if (!grid) {
    return failure(kUnreadable);
  }
  return std::move(*grid);
}

std::vector<std::string> luciole::sheetPreviewArguments(SheetFormat format, std::size_t sheetIndex) {
  return {
      "/proc/self/exe", "--sheet-preview-worker", format == SheetFormat::Ods ? "ods" : "xlsx",
      std::to_string(sheetIndex)
  };
}

int luciole::runSheetPreviewWorker(std::span<const std::string_view> arguments) {
  if (arguments.size() != 2 || (arguments[0] != "xlsx" && arguments[0] != "ods")) {
    return 2;
  }
  std::size_t sheetIndex = 0;
  const std::string_view index = arguments[1];
  const auto parsed = std::from_chars(index.data(), index.data() + index.size(), sheetIndex);
  struct stat info{};
  if (parsed.ec != std::errc{}
      || parsed.ptr != index.data() + index.size()
      || sheetIndex >= formats::kMaxGridSheets
      || ::fstat(3, &info) < 0
      || !S_ISREG(info.st_mode)
      || std::cmp_greater(info.st_size, kMaxSheetFileBytes)) {
    return 2;
  }
  try {
    const auto format = arguments[0] == "ods" ? SheetFormat::Ods : SheetFormat::Xlsx;
    return writeAll(STDOUT_FILENO, sheetAnswer(format, sheetIndex)) ? 0 : 1;
  } catch (const std::exception&) {
    return 1;
  }
}

std::expected<luciole::formats::TableGrid, std::string> luciole::readSheetPreview(
    const std::filesystem::path& path, SheetFormat format, std::size_t sheetIndex, const CancelToken& token
) {
  if (token.cancelled()) {
    return failure(kCancelled);
  }
  const Fd input(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY));
  struct stat info{};
  if (!input.valid() || ::fstat(input.get(), &info) < 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
    return failure(kUnreadable);
  }
  if (info.st_size == 0) {
    return failure(kEmpty);
  }
  if (std::cmp_greater(info.st_size, kMaxSheetFileBytes)) {
    return failure(kTooLarge);
  }
  const Fd output(::memfd_create("luciole-sheet", MFD_CLOEXEC));
  if (!output.valid()) {
    return failure(kUnreadable);
  }
  const auto ran = runProviderProcess(
      sheetPreviewArguments(format, sheetIndex), input.get(), gioCancellable(token), kMaxSheetResultBytes, 1,
      output.get()
  );
  if (token.cancelled()) {
    return failure(kCancelled);
  }
  struct stat answer{};
  if (!ran || ::fstat(output.get(), &answer) < 0) {
    return failure(kUnreadable);
  }
  if (answer.st_size <= 0 || std::cmp_greater(answer.st_size, kMaxSheetResultBytes)) {
    return failure(kUnreadable);
  }
  const auto bytes = readAll(output.get(), static_cast<std::size_t>(answer.st_size));
  if (!bytes) {
    return failure(kUnreadable);
  }
  return decodeSheetResult(*bytes);
}
