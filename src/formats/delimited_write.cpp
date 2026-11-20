#include "formats/delimited_write.h"

#include "formats/ascii.h"
#include "formats/delimited_scan.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace luciole::formats {
  namespace {
    using delimited::contentStart;
    using delimited::isLineBreak;
    using delimited::RecordScan;
    using delimited::scanRecord;
    using delimited::Syntax;
    using delimited::syntaxOf;
    using delimited::Terminator;

    constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

    // Counts as readTable makes them without limits, and the scan of `targetRow` and of the row before it.
    struct Layout {
      std::size_t rowCount = 0;
      std::size_t columnCount = 0;
      bool unterminated = false;
      std::optional<RecordScan> target;
      std::optional<RecordScan> beforeTarget;
    };

    Layout measureLayout(std::string_view bytes, const Dialect& dialect, std::size_t targetRow) {
      Layout layout;
      const Syntax syntax = syntaxOf(dialect);
      std::vector<Cell> unused;
      std::optional<RecordScan> previous;
      std::size_t pos = contentStart(bytes, dialect);
      // Every record consumes at least one byte.
      while (pos < bytes.size()) {
        const RecordScan record = scanRecord(bytes, pos, syntax, unused, 0);
        if (layout.rowCount == targetRow) {
          layout.target = record;
          layout.beforeTarget = previous;
        }
        layout.columnCount = std::max(layout.columnCount, record.fieldCount);
        layout.unterminated = layout.unterminated || record.unterminated;
        previous = record;
        ++layout.rowCount;
        pos = record.end;
      }
      return layout;
    }

    // The cell text as written, quoted when asked, when it holds a special character or when it starts or ends
    // with a space or tab.
    std::string cellBytes(std::string_view text, const Syntax& syntax, bool quote) {
      const std::array<char, 4> special{syntax.delimiter, syntax.quote, '\r', '\n'};
      const bool padded = !text.empty() && (isBlank(text.front()) || isBlank(text.back()));
      if (!quote
          && !padded
          && text.find_first_of(std::string_view(special.data(), special.size())) == std::string_view::npos) {
        return std::string(text);
      }
      std::string out;
      out.reserve(text.size() + 2);
      out.push_back(syntax.quote);
      for (const char c : text) {
        if (c == syntax.quote) {
          out.push_back(syntax.backslash ? '\\' : syntax.quote);
        } else if (syntax.backslash && c == '\\') {
          out.push_back('\\');
        }
        out.push_back(c);
      }
      out.push_back(syntax.quote);
      return out;
    }

    std::string_view lineEndingText(RowEnding ending) {
      switch (ending) {
      case RowEnding::Lf:
        return "\n";
      case RowEnding::CrLf:
        return "\r\n";
      case RowEnding::Cr:
        return "\r";
      }
      return "\n";
    }

    // Output built from the input in order: kept ranges are copied, dropped ranges are skipped and new text is
    // appended. A CR line ending that comes to stand before an LF it did not stand before in the input gets an LF
    // of its own, so the two are not read as one CRLF.
    struct Splice {
      std::string_view bytes;
      Syntax syntax;
      std::string out;
      std::size_t copied = 0;
      std::size_t inputEnd = 0; // the input position the output ends at, or kNoPosition after appended text

      static constexpr std::size_t kNoPosition = static_cast<std::size_t>(-1);

      void keep(std::size_t pos) {
        if (pos > copied) {
          put(bytes.substr(copied, pos - copied), copied == inputEnd);
          copied = pos;
          inputEnd = pos;
        }
      }
      void drop(std::size_t pos) { copied = std::max(copied, pos); }
      void append(std::string_view text) {
        if (!text.empty()) {
          put(text, false);
          inputEnd = kNoPosition;
        }
      }
      std::string finish() {
        keep(bytes.size());
        return std::move(out);
      }

    private:
      void put(std::string_view text, bool adjacent) {
        if (!adjacent && out.ends_with('\r') && text.starts_with('\n')) {
          out.push_back('\n');
        }
        out.append(text);
      }
    };

    Splice spliceOf(std::string_view bytes, const Dialect& dialect, std::size_t extra) {
      Splice splice{.bytes = bytes, .syntax = syntaxOf(dialect), .out = {}, .copied = 0, .inputEnd = 0};
      splice.out.reserve(bytes.size() + extra);
      return splice;
    }

    // Applies one row's edits, sorted by column. Missing cells before an edited one are added empty.
    void rewriteRow(
        Splice& splice, const RecordScan& record, std::span<const Cell> cells, std::span<const CellEdit> edits,
        std::span<const std::size_t> rowOrder
    ) {
      std::size_t finalCells = cells.size();
      for (const std::size_t index : rowOrder) {
        if (edits[index].column >= cells.size() && !edits[index].text.empty()) {
          finalCells = std::max(finalCells, edits[index].column + 1);
        }
      }
      std::size_t cellsNow = cells.size();
      for (const std::size_t index : rowOrder) {
        const CellEdit& edit = edits[index];
        if (edit.column < cells.size()) {
          const Cell& cell = cells[edit.column];
          splice.keep(cell.begin);
          splice.append(cellBytes(edit.text, splice.syntax, cell.quoted || (finalCells == 1 && edit.text.empty())));
          splice.drop(cell.end);
        } else if (!edit.text.empty()) {
          splice.keep(record.contentEnd);
          splice.append(std::string(edit.column - cellsNow + (cellsNow > 0 ? 1 : 0), splice.syntax.delimiter));
          splice.append(cellBytes(edit.text, splice.syntax, false));
          cellsNow = edit.column + 1;
        }
      }
    }
  } // namespace

  std::expected<std::string, EditError>
  editCells(std::string_view bytes, const Dialect& dialect, std::span<const CellEdit> edits) {
    const Layout layout = measureLayout(bytes, dialect, kNoRow);
    if (layout.unterminated) {
      return std::unexpected(EditError::UnterminatedQuote);
    }
    std::size_t extra = 0;
    for (const CellEdit& edit : edits) {
      if (edit.row >= layout.rowCount) {
        return std::unexpected(EditError::RowOutOfRange);
      }
      if (edit.column >= std::max<std::size_t>(layout.columnCount, 1)) {
        return std::unexpected(EditError::ColumnOutOfRange);
      }
      extra += edit.text.size() * 2 + edit.column + 2;
    }
    std::vector<std::size_t> order(edits.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    const auto place = [&](std::size_t index) { return std::pair{edits[index].row, edits[index].column}; };
    std::ranges::sort(order, {}, place);
    if (std::ranges::adjacent_find(order, {}, place) != order.end()) {
      return std::unexpected(EditError::DuplicateCell);
    }
    Splice splice = spliceOf(bytes, dialect, extra);
    std::vector<Cell> cells;
    std::size_t next = 0;
    std::size_t row = 0;
    std::size_t pos = contentStart(bytes, dialect);
    // Every record consumes at least one byte.
    while (pos < bytes.size() && next < order.size()) {
      const bool edited = edits[order[next]].row == row;
      cells.clear();
      const RecordScan record = scanRecord(bytes, pos, splice.syntax, cells, edited ? layout.columnCount : 0);
      if (edited) {
        std::size_t end = next;
        while (end < order.size() && edits[order[end]].row == row) {
          ++end;
        }
        rewriteRow(splice, record, cells, edits, std::span(order).subspan(next, end - next));
        next = end;
      }
      pos = record.end;
      ++row;
    }
    return splice.finish();
  }

  std::expected<std::string, EditError>
  insertRow(std::string_view bytes, const Dialect& dialect, std::size_t row, std::span<const std::string> cells) {
    const Layout layout = measureLayout(bytes, dialect, row);
    if (layout.unterminated) {
      return std::unexpected(EditError::UnterminatedQuote);
    }
    if (row > layout.rowCount) {
      return std::unexpected(EditError::RowOutOfRange);
    }
    Splice splice = spliceOf(bytes, dialect, 0);
    std::string line;
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (i > 0) {
        line.push_back(splice.syntax.delimiter);
      }
      line.append(cellBytes(cells[i], splice.syntax, cells.size() == 1 && cells[i].empty()));
    }
    const std::string_view ending = lineEndingText(dialect.lineEnding);
    const bool openLastRow =
        !layout.target && bytes.size() > contentStart(bytes, dialect) && !isLineBreak(bytes.back());
    splice.keep(layout.target ? layout.target->begin : bytes.size());
    if (openLastRow) {
      splice.append(ending);
    }
    splice.append(line);
    if (!openLastRow || line.empty()) {
      splice.append(ending);
    }
    return splice.finish();
  }

  std::expected<std::string, EditError> deleteRow(std::string_view bytes, const Dialect& dialect, std::size_t row) {
    const Layout layout = measureLayout(bytes, dialect, row);
    if (layout.unterminated) {
      return std::unexpected(EditError::UnterminatedQuote);
    }
    if (!layout.target) {
      return std::unexpected(EditError::RowOutOfRange);
    }
    const RecordScan& record = *layout.target;
    // An empty line cannot go without its line ending, so it keeps it.
    const bool last =
        record.terminator == Terminator::None && layout.beforeTarget && layout.beforeTarget->fieldCount > 0;
    Splice splice = spliceOf(bytes, dialect, 0);
    splice.keep(last ? layout.beforeTarget->contentEnd : record.begin);
    splice.drop(record.end);
    return splice.finish();
  }

  std::expected<std::string, EditError>
  insertColumn(std::string_view bytes, const Dialect& dialect, std::size_t column) {
    const Layout layout = measureLayout(bytes, dialect, kNoRow);
    if (layout.unterminated) {
      return std::unexpected(EditError::UnterminatedQuote);
    }
    if (column > layout.columnCount) {
      return std::unexpected(EditError::ColumnOutOfRange);
    }
    Splice splice = spliceOf(bytes, dialect, layout.rowCount);
    std::vector<Cell> cells;
    std::size_t pos = contentStart(bytes, dialect);
    // Every record consumes at least one byte.
    while (pos < bytes.size()) {
      cells.clear();
      const RecordScan record = scanRecord(bytes, pos, splice.syntax, cells, column + 1);
      if (record.fieldCount > 0 && record.fieldCount >= column) {
        splice.keep(column < record.fieldCount ? cells[column].begin : record.contentEnd);
        splice.out.push_back(splice.syntax.delimiter);
      }
      pos = record.end;
    }
    return splice.finish();
  }

  std::expected<std::string, EditError>
  deleteColumn(std::string_view bytes, const Dialect& dialect, std::size_t column) {
    const Layout layout = measureLayout(bytes, dialect, kNoRow);
    if (layout.unterminated) {
      return std::unexpected(EditError::UnterminatedQuote);
    }
    if (column >= layout.columnCount) {
      return std::unexpected(EditError::ColumnOutOfRange);
    }
    Splice splice = spliceOf(bytes, dialect, 2);
    const std::string_view ending = lineEndingText(dialect.lineEnding);
    const std::array<char, 2> emptyQuoted{splice.syntax.quote, splice.syntax.quote};
    std::vector<Cell> cells;
    std::size_t pos = contentStart(bytes, dialect);
    // Every record consumes at least one byte.
    while (pos < bytes.size()) {
      cells.clear();
      const RecordScan record = scanRecord(bytes, pos, splice.syntax, cells, column + 2);
      if (record.fieldCount == 1 && column == 0) {
        // The row becomes an empty line, which needs a line ending of its own.
        splice.keep(cells[0].begin);
        splice.drop(cells[0].end);
        splice.append(record.terminator == Terminator::None ? ending : std::string_view());
      } else if (record.fieldCount > column) {
        const bool first = column == 0;
        splice.keep(first ? cells[0].begin : cells[column - 1].end);
        splice.drop(first ? cells[1].begin : cells[column].end);
        // A row left with one empty cell keeps it quoted, so it does not read as an empty line.
        const Cell& other = cells[first ? 1 : 0];
        if (record.fieldCount == 2 && other.begin == other.end) {
          splice.append(std::string_view(emptyQuoted.data(), emptyQuoted.size()));
        }
      }
      pos = record.end;
    }
    return splice.finish();
  }
} // namespace luciole::formats
