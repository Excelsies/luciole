#include "formats/delimited_scan.h"

#include <algorithm>

namespace luciole::formats::delimited {
  namespace {
    Terminator terminatorAt(std::string_view bytes, std::size_t pos) {
      if (pos >= bytes.size()) {
        return Terminator::None;
      }
      if (bytes[pos] == '\n') {
        return Terminator::Lf;
      }
      if (bytes[pos] != '\r') {
        return Terminator::None;
      }
      return pos + 1 < bytes.size() && bytes[pos + 1] == '\n' ? Terminator::CrLf : Terminator::Cr;
    }

    std::size_t terminatorLength(Terminator terminator) {
      switch (terminator) {
      case Terminator::None:
        return 0;
      case Terminator::CrLf:
        return 2;
      case Terminator::Lf:
      case Terminator::Cr:
        return 1;
      }
      return 0;
    }

    // One cell starting at `pos`. A quoted cell runs to its closing quote, then like an unquoted cell to the
    // next delimiter or line ending. Each loop advances at least one byte, so both end within the input.
    Cell scanCell(std::string_view bytes, std::size_t pos, const Syntax& syntax, bool& unterminated) {
      const std::size_t size = bytes.size();
      Cell cell{.begin = pos, .end = pos, .quoted = pos < size && bytes[pos] == syntax.quote};
      std::size_t i = pos;
      if (cell.quoted) {
        ++i;
        bool closed = false;
        while (i < size && !closed) {
          const char c = bytes[i];
          if (syntax.backslash && c == '\\') {
            i = std::min(i + 2, size);
          } else if (c == syntax.quote && !syntax.backslash && i + 1 < size && bytes[i + 1] == syntax.quote) {
            i += 2;
          } else {
            closed = c == syntax.quote;
            ++i;
          }
        }
        if (!closed) {
          unterminated = true;
          cell.end = size;
          return cell;
        }
      }
      while (i < size && bytes[i] != syntax.delimiter && !isLineBreak(bytes[i])) {
        ++i;
      }
      cell.end = i;
      return cell;
    }
  } // namespace

  Syntax syntaxOf(const Dialect& dialect) {
    return {
        .delimiter = delimiterChar(dialect.delimiter),
        .quote = quoteChar(dialect.quote),
        .backslash = dialect.escape == QuoteEscape::Backslash,
    };
  }

  std::size_t contentStart(std::string_view bytes, const Dialect& dialect) {
    return dialect.byteOrderMark && bytes.starts_with(kByteOrderMark) ? kByteOrderMark.size() : 0;
  }

  bool isLineBreak(char c) { return c == '\n' || c == '\r'; }

  RecordScan scanRecord(
      std::string_view bytes, std::size_t pos, const Syntax& syntax, std::vector<Cell>& cells, std::size_t maxStore
  ) {
    RecordScan record{.begin = pos};
    std::size_t i = pos;
    if (pos < bytes.size() && !isLineBreak(bytes[pos])) {
      const std::size_t limit = bytes.size() - pos + 1;
      for (std::size_t step = 0; step < limit; ++step) {
        const Cell cell = scanCell(bytes, i, syntax, record.unterminated);
        if (record.fieldCount < maxStore) {
          cells.push_back(cell);
        }
        ++record.fieldCount;
        i = cell.end;
        if (i >= bytes.size() || bytes[i] != syntax.delimiter) {
          break;
        }
        ++i;
      }
    }
    record.contentEnd = i;
    record.terminator = terminatorAt(bytes, i);
    record.end = i + terminatorLength(record.terminator);
    return record;
  }
} // namespace luciole::formats::delimited
