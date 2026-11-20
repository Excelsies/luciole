#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A table prepared for display: rows and columns of one-line cell text, each cell with a type, under fixed bounds.
namespace luciole::formats {
  enum class GridCellKind : std::uint8_t { Text, Number, Date, Time, Boolean, Error };
  inline constexpr std::size_t kGridCellKindCount = 6;

  inline constexpr std::size_t kMaxGridRows = 10'000;
  inline constexpr std::size_t kMaxGridColumns = 256;
  inline constexpr std::size_t kMaxGridCells = 1'000'000;
  // Cell text and column names are cut to this many bytes, on a code point boundary.
  inline constexpr std::size_t kMaxGridCellBytes = 256;
  inline constexpr std::size_t kMaxGridTextBytes = 16 * 1024 * 1024;
  inline constexpr std::size_t kMaxGridSheets = 256;

  struct TableGrid {
    std::vector<std::string> sheetNames;  // workbook order; empty for delimited text
    std::size_t sheetIndex = 0;           // the sheet shown
    std::vector<std::string> columnNames; // from a header row; empty when there is none
    std::size_t firstSourceRow = 0;       // the source row and column of grid cell (0, 0)
    std::size_t firstSourceColumn = 0;
    std::size_t firstRowNumber = 1; // the number shown beside grid row 0
    std::size_t rowCount = 0;
    std::size_t columnCount = 0;
    std::string text;                // every cell's text, row after row
    std::vector<std::uint32_t> ends; // per cell, row after row: where its text ends in `text`
    std::vector<GridCellKind> kinds; // per cell, row after row
    bool truncated = false;          // the source holds more than the grid
    bool rowsCut = false;            // the source holds rows past the grid's last; sets truncated too
    bool columnsCut = false;         // the source holds columns past the grid's last; sets truncated too
  };

  // Empty for a cell outside the grid.
  std::string_view gridCellText(const TableGrid& grid, std::size_t row, std::size_t column);
  // Text for a cell outside the grid.
  GridCellKind gridCellKind(const TableGrid& grid, std::size_t row, std::size_t column);

  // A column as spreadsheets name it: 0 is "A", 25 is "Z", 26 is "AA".
  std::string columnLetters(std::size_t index);
  // The header name of a column, or the letters of its source column where the header names none.
  std::string gridColumnName(const TableGrid& grid, std::size_t column);

  // Text for one grid line: line endings, tabs and other control characters become spaces, each byte of invalid
  // UTF-8 becomes U+FFFD, and the result is cut to kMaxGridCellBytes on a code point boundary.
  std::string gridText(std::string_view text);

  // Fills a grid cell by cell in ascending row-major order. Rows and columns are clamped to the grid limits, rows
  // first when their product passes kMaxGridCells; cells in the rows and columns cut away are passed over and the
  // grid is marked with rowsCut or columnsCut.
  class TableGridBuilder {
  public:
    TableGridBuilder(std::size_t rows, std::size_t columns);

    std::size_t rowCount() const noexcept { return m_rows; }
    std::size_t columnCount() const noexcept { return m_columns; }

    // Sets a cell after the last one set; the cells between stay empty Text. False, changing nothing, for a cell
    // outside the requested rows and columns or not after the last one. Text that passes kMaxGridTextBytes ends
    // the grid before its row and marks rowsCut.
    bool set(std::size_t row, std::size_t column, GridCellKind kind, std::string_view text);

    TableGrid finish();

  private:
    void fillTo(std::size_t cell);

    TableGrid m_grid;
    std::size_t m_requestedRows = 0;
    std::size_t m_requestedColumns = 0;
    std::size_t m_rows = 0;
    std::size_t m_columns = 0;
    std::size_t m_lastRow = 0; // the last cell set, in requested coordinates
    std::size_t m_lastColumn = 0;
    std::size_t m_cutRow = 0; // the row the text budget ran out in
    bool m_started = false;
    bool m_full = false;
  };
} // namespace luciole::formats
