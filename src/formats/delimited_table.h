#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Reading delimited text (CSV, TSV and their kin) into rows of cells, and the text and type of a cell.
namespace luciole::formats {
  enum class Delimiter : std::uint8_t { Comma, Tab, Semicolon, Pipe };
  enum class Quote : std::uint8_t { Double, Single };
  // How a quote is written inside a quoted cell: twice ("") or after a backslash (\").
  enum class QuoteEscape : std::uint8_t { Doubled, Backslash };
  enum class RowEnding : std::uint8_t { Lf, CrLf, Cr };
  enum class CellType : std::uint8_t { Text, Number, Date };

  // Default bound on the text cellText returns for one cell.
  inline constexpr std::size_t kMaxCellTextBytes = 4096;
  // cellType reads at most this many bytes after trimming; longer text is Text.
  inline constexpr std::size_t kMaxTypedBytes = 64;

  struct Dialect {
    Delimiter delimiter = Delimiter::Comma;
    Quote quote = Quote::Double;
    QuoteEscape escape = QuoteEscape::Doubled;
    RowEnding lineEnding = RowEnding::Lf; // written after inserted rows
    bool byteOrderMark = false;           // a UTF-8 byte order mark comes first; it is skipped and kept
    bool header = false;                  // row 0 names the columns
    bool finalNewline = false;            // the last row ends with a line ending
    bool operator==(const Dialect&) const = default;
  };

  // Reading stops at maxRows rows or maxCells cells; cells past maxColumns in a row are skipped.
  struct TableLimits {
    std::size_t maxRows = 100'000;
    std::size_t maxColumns = 1'000;
    std::size_t maxCells = 1'000'000;
  };

  // A cell's raw bytes in the input, quotes included.
  struct Cell {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool quoted = false; // starts with the quote character, so its text needs unquoting through cellText
    bool operator==(const Cell&) const = default;
  };

  struct Row {
    std::size_t firstCell = 0; // index into Table::cells
    std::size_t cellCount = 0; // 0 for an empty line
  };

  // Rows in file order. Every line ending outside quotes (LF, CRLF or CR) ends a row; an empty line is a row
  // with no cells. Row 0 is the header row when the dialect has one.
  struct Table {
    std::vector<Row> rows;
    std::vector<Cell> cells;
    std::size_t columnCount = 0;    // most cells in a stored row
    bool truncated = false;         // a row, column or cell limit left something out
    bool unterminatedQuote = false; // a quoted cell runs to the end of the input
  };

  struct CellText {
    std::string text;
    bool clipped = false; // the text was cut at the byte bound, on a code point boundary
  };

  char delimiterChar(Delimiter delimiter);
  char quoteChar(Quote quote);

  // Reads rows of cells as byte ranges into `bytes`. Never fails: malformed input gives a defined table.
  // Time is linear in the input size, memory in the stored cells.
  Table readTable(std::string_view bytes, const Dialect& dialect, const TableLimits& limits = {});

  // The cells of one row; empty for an unknown row.
  std::span<const Cell> rowCells(const Table& table, std::size_t row);

  // The cell's text with quotes and escapes removed, at most `maxBytes` bytes.
  CellText
  cellText(std::string_view bytes, const Dialect& dialect, const Cell& cell, std::size_t maxBytes = kMaxCellTextBytes);

  // Number, Date (dates and times) or Text, from at most kMaxTypedBytes of trimmed text. Empty text is Text.
  CellType cellType(std::string_view text);

  // One type per column from the non-empty cells of up to `sampleRows` rows below the header: Number or Date
  // when every one of them is, otherwise Text.
  std::vector<CellType>
  columnTypes(std::string_view bytes, const Dialect& dialect, const Table& table, std::size_t sampleRows = 200);

  struct ColumnSample {
    CellType type = CellType::Text;
    std::size_t values = 0;                  // non-empty cells
    std::optional<std::size_t> commonLength; // set when every value has the same byte length
  };

  // The cells of `column` in rows first to last (exclusive): Number or Date when every non-empty one is, how many
  // are not empty, and their byte length when they all share it. Rows that do not reach the column are skipped.
  ColumnSample columnSample(
      std::string_view bytes, const Dialect& dialect, const Table& table, std::size_t column, std::size_t first,
      std::size_t last
  );
} // namespace luciole::formats
