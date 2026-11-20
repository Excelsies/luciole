#pragma once

#include "formats/delimited_table.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

// Edits of delimited text: cells, rows and columns.
namespace luciole::formats {
  enum class EditError : std::uint8_t { RowOutOfRange, ColumnOutOfRange, DuplicateCell, UnterminatedQuote };

  struct CellEdit {
    std::size_t row = 0;
    std::size_t column = 0;
    std::string text;
  };

  // The writers take the bytes and the dialect they were read with and return new bytes in which everything
  // they do not change is byte-identical. Rows and columns count as readTable counts them, without limits.
  // A CR line ending that comes to stand before an LF gets an LF after it, so the two do not read as one CRLF.
  // Input with an unterminated quote is refused.

  // Replaces cell texts. An edited cell is quoted when its text holds the delimiter, the quote character or a
  // line ending, starts or ends with a space or tab, when the original cell was quoted, or when the row would
  // otherwise become an empty line.
  // A column past the end of a short row adds empty cells before it; empty text there changes nothing.
  // Rows must exist and columns must be below the table's column count; one cell may be edited once.
  std::expected<std::string, EditError>
  editCells(std::string_view bytes, const Dialect& dialect, std::span<const CellEdit> edits);

  // Inserts a row before `row` (0 to the row count). A row appended to a file without a final newline goes
  // after a new line ending, so the file still has none. No cells gives an empty line, which always ends with a
  // line ending. Cells are quoted as editCells quotes them; one empty cell is quoted.
  std::expected<std::string, EditError>
  insertRow(std::string_view bytes, const Dialect& dialect, std::size_t row, std::span<const std::string> cells);

  // Removes a row and its line ending; removing a last row without one removes the line ending before it,
  // unless the row before is an empty line.
  std::expected<std::string, EditError> deleteRow(std::string_view bytes, const Dialect& dialect, std::size_t row);

  // Inserts an empty cell at `column` (0 to the column count) in every row that reaches that column.
  // Shorter rows and empty lines are unchanged.
  std::expected<std::string, EditError>
  insertColumn(std::string_view bytes, const Dialect& dialect, std::size_t column);

  // Removes the cell at `column` and one delimiter beside it from every row that has it. A row whose only
  // cell goes becomes an empty line, with a line ending added when it had none. A row left with one empty cell
  // keeps it quoted.
  std::expected<std::string, EditError>
  deleteColumn(std::string_view bytes, const Dialect& dialect, std::size_t column);
} // namespace luciole::formats
