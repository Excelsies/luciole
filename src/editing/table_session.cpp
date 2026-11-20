#include "editing/table_session.h"

#include <algorithm>
#include <array>
#include <utility>

namespace luciole::editing {
  namespace {
    TableError refusal(TableRefusal reason, std::string message) {
      return TableError{.reason = reason, .message = std::move(message)};
    }

    enum class Source : std::uint8_t { OpenedFile, Change };

    // The table read from `bytes`, or why bytes from this source cannot be edited.
    std::expected<formats::Table, TableError>
    readEditable(std::string_view bytes, const formats::Dialect& dialect, Source source) {
      const bool opened = source == Source::OpenedFile;
      if (bytes.size() > kTableByteLimit) {
        return std::unexpected(refusal(
            TableRefusal::TooLarge,
            opened ? "This file is larger than 1 MiB, the most that can be edited as a table."
                   : "This change would make the file larger than 1 MiB, the most that can be edited."
        ));
      }
      formats::Table table = formats::readTable(bytes, dialect);
      if (table.truncated) {
        return std::unexpected(refusal(
            TableRefusal::Truncated,
            opened ? "This file has more rows, columns or cells than can be edited as a table."
                   : "This change would give the table more rows, columns or cells than can be edited."
        ));
      }
      if (table.unterminatedQuote) {
        return std::unexpected(refusal(
            TableRefusal::UnterminatedQuote,
            opened ? "This file has a quote that is never closed, so it cannot be edited."
                   : "This change would leave a quote that is never closed."
        ));
      }
      return table;
    }

    TableError writerRefusal(formats::EditError error) {
      switch (error) {
      case formats::EditError::RowOutOfRange:
        return refusal(TableRefusal::RowOutOfRange, "That row is not in the table.");
      case formats::EditError::ColumnOutOfRange:
        return refusal(TableRefusal::ColumnOutOfRange, "That column is not in the table.");
      case formats::EditError::DuplicateCell:
        return refusal(TableRefusal::DuplicateCell, "The same cell was changed twice in one edit.");
      case formats::EditError::UnterminatedQuote:
        break;
      }
      return refusal(TableRefusal::UnterminatedQuote, "This file has a quote that is never closed.");
    }
  } // namespace

  TableSession::TableSession(std::string bytes, const formats::Dialect& dialect, formats::Table table)
      : m_dialect(dialect), m_historyBytes(bytes.size()), m_savedRevision(0), m_table(std::move(table)) {
    m_history.push_back({.bytes = std::move(bytes), .revision = 0});
  }

  std::expected<TableSession, TableError> TableSession::open(std::string bytes, const formats::Dialect& dialect) {
    auto table = readEditable(bytes, dialect, Source::OpenedFile);
    if (!table) {
      return std::unexpected(std::move(table.error()));
    }
    return TableSession(std::move(bytes), dialect, std::move(*table));
  }

  const std::string& TableSession::bytes() const { return m_history[m_position].bytes; }

  const formats::Table& TableSession::table() const { return m_table; }

  const formats::Dialect& TableSession::dialect() const { return m_dialect; }

  std::size_t TableSession::rowCount() const { return m_table.rows.size(); }

  std::size_t TableSession::columnCount() const { return m_table.columnCount; }

  std::uint64_t TableSession::revision() const { return m_history[m_position].revision; }

  bool TableSession::modified() const { return m_savedRevision != revision(); }

  void TableSession::markSaved(std::uint64_t revision) {
    const bool reachable = std::ranges::contains(m_history, revision, &Snapshot::revision);
    m_savedRevision = reachable ? std::optional(revision) : std::nullopt;
  }

  std::expected<void, TableError> TableSession::setCell(std::size_t row, std::size_t column, std::string_view text) {
    const std::array edit{formats::CellEdit{.row = row, .column = column, .text = std::string(text)}};
    auto written = formats::editCells(bytes(), m_dialect, edit);
    if (written) {
      // The writer accepted the row, so it is in the table; a missing cell reads as empty text.
      const auto cells = formats::rowCells(m_table, row);
      const std::string current =
          column < cells.size() ? formats::cellText(bytes(), m_dialect, cells[column], bytes().size()).text : "";
      if (current == text) {
        return {};
      }
    }
    return commit(std::move(written));
  }

  std::expected<void, TableError> TableSession::insertRow(std::size_t row) {
    const std::vector<std::string> cells(columnCount());
    return commit(formats::insertRow(bytes(), m_dialect, row, cells));
  }

  std::expected<void, TableError> TableSession::deleteRow(std::size_t row) {
    if (row == 0 && rowCount() == 1) {
      return std::unexpected(refusal(TableRefusal::LastRow, "The only row of a table cannot be deleted."));
    }
    return commit(formats::deleteRow(bytes(), m_dialect, row));
  }

  std::expected<void, TableError> TableSession::insertColumn(std::size_t column) {
    return commit(formats::insertColumn(bytes(), m_dialect, column));
  }

  std::expected<void, TableError> TableSession::deleteColumn(std::size_t column) {
    if (column == 0 && columnCount() == 1) {
      return std::unexpected(refusal(TableRefusal::LastColumn, "The only column of a table cannot be deleted."));
    }
    return commit(formats::deleteColumn(bytes(), m_dialect, column));
  }

  bool TableSession::canUndo() const { return m_position > 0; }

  bool TableSession::canRedo() const { return m_position + 1 < m_history.size(); }

  bool TableSession::undo() {
    if (!canUndo()) {
      return false;
    }
    moveTo(m_position - 1);
    return true;
  }

  bool TableSession::redo() {
    if (!canRedo()) {
      return false;
    }
    moveTo(m_position + 1);
    return true;
  }

  std::expected<void, TableError> TableSession::commit(std::expected<std::string, formats::EditError> written) {
    if (!written) {
      return std::unexpected(writerRefusal(written.error()));
    }
    std::string next = std::move(*written);
    if (next == bytes()) {
      return {};
    }
    auto table = readEditable(next, m_dialect, Source::Change);
    if (!table) {
      return std::unexpected(std::move(table.error()));
    }
    // A change drops the positions undo left behind.
    for (std::size_t i = m_position + 1; i < m_history.size(); ++i) {
      m_historyBytes -= m_history[i].bytes.size();
    }
    m_history.resize(m_position + 1);
    m_historyBytes += next.size();
    m_history.push_back({.bytes = std::move(next), .revision = m_nextRevision++});
    // The oldest positions go first; the new one always stays, as one file is at most kTableByteLimit bytes.
    std::size_t dropped = 0;
    while (dropped + 1 < m_history.size()
           && (m_history.size() - dropped > kUndoStepLimit + 1 || m_historyBytes > kHistoryByteLimit)) {
      m_historyBytes -= m_history[dropped].bytes.size();
      ++dropped;
    }
    m_history.erase(m_history.begin(), m_history.begin() + static_cast<std::ptrdiff_t>(dropped));
    m_position = m_history.size() - 1;
    m_table = std::move(*table);
    return {};
  }

  void TableSession::moveTo(std::size_t position) {
    m_position = position;
    m_table = formats::readTable(bytes(), m_dialect);
  }
} // namespace luciole::editing
