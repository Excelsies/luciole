#pragma once

#include "formats/delimited_table.h"
#include "formats/delimited_write.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The bytes of a CSV or TSV file being edited as a table, the changes made to them and their undo history.
namespace luciole::editing {
  // Files larger than this are not edited as tables, and no change may make a file larger.
  inline constexpr std::size_t kTableByteLimit = 1024 * 1024;
  // The history keeps at most this many undo steps.
  inline constexpr std::size_t kUndoStepLimit = 100;
  // The history keeps at most this many bytes of file snapshots, the current one included.
  inline constexpr std::size_t kHistoryByteLimit = 16 * 1024 * 1024;

  enum class TableRefusal : std::uint8_t {
    TooLarge,
    Truncated,
    UnterminatedQuote,
    RowOutOfRange,
    ColumnOutOfRange,
    DuplicateCell,
    LastRow,
    LastColumn,
  };

  struct TableError {
    TableRefusal reason = TableRefusal::TooLarge;
    std::string message; // a plain sentence for the user
  };

  // Rows and columns are source coordinates: row 0 is the first row of the file, the header row when the dialect
  // has one. The dialect is the one given at open and never changes. A change rewrites the whole file through the
  // formats writers, which keep every byte the change does not touch.
  class TableSession {
  public:
    // Refuses bytes over kTableByteLimit, bytes the reader cuts short at its row, column or cell limits and bytes
    // with a quote that is never closed.
    [[nodiscard]] static std::expected<TableSession, TableError>
    open(std::string bytes, const formats::Dialect& dialect);

    // Valid until the next change, undo or redo.
    const std::string& bytes() const;
    // Read from bytes() with the default limits, which it fits.
    const formats::Table& table() const;
    const formats::Dialect& dialect() const;
    std::size_t rowCount() const;
    std::size_t columnCount() const;

    // Names the current bytes: each change gets a revision above every earlier one, and undo and redo return to
    // the revision of the position they reach.
    std::uint64_t revision() const;

    // True when the current history position is not the saved one.
    bool modified() const;
    // Marks the position holding `revision` as saved when the history still holds it; otherwise no position is.
    void markSaved(std::uint64_t revision);

    // Each change is applied whole or refused whole. A change that leaves the bytes as they are, or a cell that
    // keeps its text, is accepted without a new revision or history step. A cell past the end of a short row but
    // inside the column count gets empty cells before it.
    [[nodiscard]] std::expected<void, TableError> setCell(std::size_t row, std::size_t column, std::string_view text);
    // Inserts a row of columnCount() empty cells before `row` (0 to rowCount()).
    [[nodiscard]] std::expected<void, TableError> insertRow(std::size_t row);
    // Refuses to delete the only row. It may leave only empty lines, a table with no columns.
    [[nodiscard]] std::expected<void, TableError> deleteRow(std::size_t row);
    // Inserts an empty cell at `column` (0 to columnCount()) in every row that reaches it.
    [[nodiscard]] std::expected<void, TableError> insertColumn(std::size_t column);
    // Refuses to delete the only column.
    [[nodiscard]] std::expected<void, TableError> deleteColumn(std::size_t column);

    // Each reports whether undo() or redo() would move the history position.
    bool canUndo() const;
    bool canRedo() const;
    // Each reports whether it moved the history position.
    bool undo();
    bool redo();

  private:
    struct Snapshot {
      std::string bytes;
      std::uint64_t revision = 0;
    };

    TableSession(std::string bytes, const formats::Dialect& dialect, formats::Table table);

    // Applies the writer's output, or refuses with its error or with the reasons open would refuse the output.
    std::expected<void, TableError> commit(std::expected<std::string, formats::EditError> written);
    void moveTo(std::size_t position);

    formats::Dialect m_dialect;
    std::vector<Snapshot> m_history;
    std::size_t m_position = 0;
    std::size_t m_historyBytes = 0;
    std::uint64_t m_nextRevision = 1;
    std::optional<std::uint64_t> m_savedRevision;
    formats::Table m_table;
  };
} // namespace luciole::editing
