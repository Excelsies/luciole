#include "formats/delimited_table.h"

#include "formats/ascii.h"
#include "formats/delimited_scan.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace luciole::formats {
  namespace {
    using delimited::contentStart;
    using delimited::RecordScan;
    using delimited::scanRecord;
    using delimited::Syntax;
    using delimited::syntaxOf;

    // Shortens `text` so it does not end inside a UTF-8 sequence.
    void trimPartialCodePoint(std::string& text) {
      std::size_t lead = text.size();
      for (std::size_t back = 0; back < 4 && lead > 0; ++back) {
        --lead;
        if ((static_cast<unsigned char>(text[lead]) & 0xC0U) != 0x80U) {
          break;
        }
      }
      if (lead >= text.size()) {
        return;
      }
      const auto first = static_cast<unsigned char>(text[lead]);
      std::size_t length = 1;
      if ((first & 0xE0U) == 0xC0U) {
        length = 2;
      } else if ((first & 0xF0U) == 0xE0U) {
        length = 3;
      } else if ((first & 0xF8U) == 0xF0U) {
        length = 4;
      }
      if (text.size() - lead < length) {
        text.resize(lead);
      }
    }

    std::size_t digitRun(std::string_view text, std::size_t pos) {
      std::size_t end = pos;
      while (end < text.size() && isAsciiDigit(text[end])) {
        ++end;
      }
      return end - pos;
    }

    // H:MM, H:MM:SS or H:MM:SS.fraction at `pos`, then an optional Z, +HH, +HHMM or +HH:MM. Returns its end.
    std::optional<std::size_t> matchTime(std::string_view text, std::size_t pos) {
      const std::size_t hours = digitRun(text, pos);
      if (hours < 1 || hours > 2) {
        return std::nullopt;
      }
      pos += hours;
      if (pos >= text.size() || text[pos] != ':' || digitRun(text, pos + 1) != 2) {
        return std::nullopt;
      }
      pos += 3;
      if (pos < text.size() && text[pos] == ':') {
        if (digitRun(text, pos + 1) != 2) {
          return std::nullopt;
        }
        pos += 3;
        if (pos < text.size() && text[pos] == '.') {
          const std::size_t fraction = digitRun(text, pos + 1);
          if (fraction == 0) {
            return std::nullopt;
          }
          pos += 1 + fraction;
        }
      }
      if (pos < text.size() && text[pos] == 'Z') {
        return pos + 1;
      }
      if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        if (digitRun(text, pos + 1) != 2 && digitRun(text, pos + 1) != 4) {
          return std::nullopt;
        }
        pos += 1 + digitRun(text, pos + 1);
        if (pos < text.size() && text[pos] == ':' && digitRun(text, pos + 1) == 2) {
          pos += 3;
        }
      }
      return pos;
    }

    // YYYY-M-D or D-M-YY(YY), with '-', '/' or '.' used for both separators. Returns its end.
    std::optional<std::size_t> matchDate(std::string_view text) {
      const std::size_t first = digitRun(text, 0);
      if (first == 0 || first >= text.size()) {
        return std::nullopt;
      }
      const char separator = text[first];
      if (separator != '-' && separator != '/' && separator != '.') {
        return std::nullopt;
      }
      const std::size_t second = digitRun(text, first + 1);
      const std::size_t secondEnd = first + 1 + second;
      if (second < 1 || second > 2 || secondEnd >= text.size() || text[secondEnd] != separator) {
        return std::nullopt;
      }
      const std::size_t third = digitRun(text, secondEnd + 1);
      const bool yearFirst = first == 4 && third >= 1 && third <= 2;
      const bool yearLast = first <= 2 && (third == 2 || third == 4);
      if (!yearFirst && !yearLast) {
        return std::nullopt;
      }
      return secondEnd + 1 + third;
    }

    bool dateLike(std::string_view text) {
      if (matchTime(text, 0) == text.size()) {
        return true;
      }
      const auto date = matchDate(text);
      if (!date) {
        return false;
      }
      if (*date == text.size()) {
        return true;
      }
      return (text[*date] == 'T' || text[*date] == ' ') && matchTime(text, *date + 1) == text.size();
    }

    // Digits in groups split by '.' or ','. A separator used more than once separates thousands, so three
    // digits follow each of its uses.
    bool groupedDigits(std::string_view text) {
      const auto dots = std::ranges::count(text, '.');
      const auto commas = std::ranges::count(text, ',');
      if (text.empty() || (dots > 1 && commas > 1)) {
        return false;
      }
      const char thousands = dots > 1 ? '.' : (commas > 1 ? ',' : '\0');
      std::size_t run = 0;
      char last = '\0';
      for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (isAsciiDigit(c)) {
          ++run;
          continue;
        }
        if ((c != '.' && c != ',') || (run == 0 && i > 0) || (last != '\0' && last == thousands && run != 3)) {
          return false;
        }
        last = c;
        run = 0;
      }
      return run > 0 && (last == '\0' || last != thousands || run == 3);
    }

    // An optional sign, grouped digits, an optional exponent and an optional percent sign.
    bool numberLike(std::string_view text) {
      if (text.ends_with('%')) {
        text.remove_suffix(1);
      }
      if (text.starts_with('+') || text.starts_with('-')) {
        text.remove_prefix(1);
      }
      const auto exponent = text.find_first_of("eE");
      if (exponent == std::string_view::npos) {
        return groupedDigits(text);
      }
      std::string_view power = text.substr(exponent + 1);
      if (power.starts_with('+') || power.starts_with('-')) {
        power.remove_prefix(1);
      }
      return !power.empty() && digitRun(power, 0) == power.size() && groupedDigits(text.substr(0, exponent));
    }

  } // namespace

  char delimiterChar(Delimiter delimiter) {
    switch (delimiter) {
    case Delimiter::Comma:
      return ',';
    case Delimiter::Tab:
      return '\t';
    case Delimiter::Semicolon:
      return ';';
    case Delimiter::Pipe:
      return '|';
    }
    return ',';
  }

  char quoteChar(Quote quote) { return quote == Quote::Single ? '\'' : '"'; }

  Table readTable(std::string_view bytes, const Dialect& dialect, const TableLimits& limits) {
    Table table;
    const Syntax syntax = syntaxOf(dialect);
    std::size_t pos = contentStart(bytes, dialect);
    // Every record consumes at least one byte.
    while (pos < bytes.size()) {
      if (table.rows.size() >= limits.maxRows || table.cells.size() >= limits.maxCells) {
        table.truncated = true;
        break;
      }
      const std::size_t first = table.cells.size();
      const std::size_t room = std::min(limits.maxColumns, limits.maxCells - first);
      const RecordScan record = scanRecord(bytes, pos, syntax, table.cells, room);
      const std::size_t stored = table.cells.size() - first;
      table.rows.push_back({.firstCell = first, .cellCount = stored});
      table.columnCount = std::max(table.columnCount, stored);
      table.truncated = table.truncated || record.fieldCount > stored;
      table.unterminatedQuote = table.unterminatedQuote || record.unterminated;
      pos = record.end;
    }
    return table;
  }

  std::span<const Cell> rowCells(const Table& table, std::size_t row) {
    if (row >= table.rows.size()) {
      return {};
    }
    const Row& entry = table.rows[row];
    if (entry.firstCell > table.cells.size() || entry.cellCount > table.cells.size() - entry.firstCell) {
      return {};
    }
    return std::span<const Cell>(table.cells).subspan(entry.firstCell, entry.cellCount);
  }

  CellText cellText(std::string_view bytes, const Dialect& dialect, const Cell& cell, std::size_t maxBytes) {
    CellText result;
    const std::size_t end = std::min(cell.end, bytes.size());
    if (cell.begin >= end) {
      return result;
    }
    if (!cell.quoted) {
      result.clipped = end - cell.begin > maxBytes;
      result.text.assign(bytes.substr(cell.begin, std::min(end - cell.begin, maxBytes)));
    } else {
      const Syntax syntax = syntaxOf(dialect);
      bool closed = false;
      std::size_t i = cell.begin + 1;
      // Each step consumes at least one byte of the cell.
      while (i < end) {
        char c = bytes[i];
        std::size_t step = 1;
        bool emit = true;
        if (!closed && syntax.backslash && c == '\\') {
          emit = i + 1 < end;
          c = emit ? bytes[i + 1] : c;
          step = 2;
        } else if (!closed && c == syntax.quote) {
          const bool doubled = !syntax.backslash && i + 1 < end && bytes[i + 1] == syntax.quote;
          closed = !doubled;
          emit = doubled;
          step = doubled ? 2 : 1;
        }
        if (emit && result.text.size() >= maxBytes) {
          result.clipped = true;
          break;
        }
        if (emit) {
          result.text.push_back(c);
        }
        i += step;
      }
    }
    if (result.clipped) {
      trimPartialCodePoint(result.text);
    }
    return result;
  }

  CellType cellType(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
      return CellType::Text;
    }
    const std::string_view trimmed = text.substr(first, text.find_last_not_of(" \t") - first + 1);
    if (trimmed.size() > kMaxTypedBytes) {
      return CellType::Text;
    }
    if (dateLike(trimmed)) {
      return CellType::Date;
    }
    return numberLike(trimmed) ? CellType::Number : CellType::Text;
  }

  std::vector<CellType>
  columnTypes(std::string_view bytes, const Dialect& dialect, const Table& table, std::size_t sampleRows) {
    std::vector<CellType> types(table.columnCount, CellType::Text);
    const std::size_t first = dialect.header ? 1 : 0;
    if (first >= table.rows.size()) {
      return types;
    }
    const std::size_t last = first + std::min(sampleRows, table.rows.size() - first);
    for (std::size_t column = 0; column < types.size(); ++column) {
      types[column] = columnSample(bytes, dialect, table, column, first, last).type;
    }
    return types;
  }

  ColumnSample columnSample(
      std::string_view bytes, const Dialect& dialect, const Table& table, std::size_t column, std::size_t first,
      std::size_t last
  ) {
    ColumnSample sample;
    bool numbers = true;
    bool dates = true;
    for (std::size_t row = first; row < last; ++row) {
      const auto cells = rowCells(table, row);
      if (column >= cells.size()) {
        continue;
      }
      const CellText value = cellText(bytes, dialect, cells[column], kMaxTypedBytes + 1);
      if (value.text.empty()) {
        continue;
      }
      const CellType type = value.clipped ? CellType::Text : cellType(value.text);
      numbers = numbers && type == CellType::Number;
      dates = dates && type == CellType::Date;
      if (sample.values == 0) {
        sample.commonLength = value.text.size();
      } else if (sample.commonLength != value.text.size()) {
        sample.commonLength.reset();
      }
      ++sample.values;
    }
    if (sample.values > 0 && numbers) {
      sample.type = CellType::Number;
    } else if (sample.values > 0 && dates) {
      sample.type = CellType::Date;
    }
    return sample;
  }
} // namespace luciole::formats
