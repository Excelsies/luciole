#include "formats/delimited_detect.h"

#include "formats/delimited_scan.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace luciole::formats {
  namespace {
    using delimited::isLineBreak;
    using delimited::kByteOrderMark;
    using delimited::RecordScan;
    using delimited::scanRecord;
    using delimited::Syntax;
    using delimited::syntaxOf;
    using delimited::Terminator;

    constexpr std::array<Delimiter, 4> kDelimiters{
        Delimiter::Comma, Delimiter::Tab, Delimiter::Semicolon, Delimiter::Pipe
    };
    constexpr std::string_view kDelimiterChars = ",\t;|";
    constexpr std::size_t kScoredCellsPerRow = 64;
    constexpr std::size_t kHeaderSampleRows = 50;
    constexpr std::size_t kHeaderColumns = 64;

    bool cellEdge(char c) { return kDelimiterChars.contains(c) || isLineBreak(c); }

    // The quote character that most often both opens and closes a cell; double quotes win ties.
    Quote detectQuote(std::string_view sample) {
      std::array<std::size_t, 2> opens{};
      std::array<std::size_t, 2> closes{};
      for (std::size_t i = 0; i < sample.size(); ++i) {
        if (sample[i] != '"' && sample[i] != '\'') {
          continue;
        }
        const std::size_t index = sample[i] == '"' ? 0 : 1;
        opens[index] += i == 0 || cellEdge(sample[i - 1]) ? 1 : 0;
        closes[index] += i + 1 == sample.size() || cellEdge(sample[i + 1]) ? 1 : 0;
      }
      return std::min(opens[1], closes[1]) > std::min(opens[0], closes[0]) ? Quote::Single : Quote::Double;
    }

    // Backslash when a backslash escapes a quote inside a cell and no quote is doubled inside a cell.
    QuoteEscape detectEscape(std::string_view sample, char quote) {
      std::size_t escaped = 0;
      std::size_t doubled = 0;
      for (std::size_t i = 0; i + 1 < sample.size(); ++i) {
        if (sample[i] == '\\' && sample[i + 1] == quote && i + 2 < sample.size() && !cellEdge(sample[i + 2])) {
          ++escaped;
        }
        if (sample[i] == quote
            && sample[i + 1] == quote
            && i > 0
            && !cellEdge(sample[i - 1])
            && sample[i - 1] != quote) {
          ++doubled;
        }
      }
      return escaped > 0 && doubled == 0 ? QuoteEscape::Backslash : QuoteEscape::Doubled;
    }

    // A quoted, typed or plain cell; one holding a tab, or a comma, semicolon or pipe not followed by a space,
    // suggests the delimiter is wrong.
    bool cleanCell(std::string_view sample, const Cell& cell) {
      const std::string_view raw = sample.substr(cell.begin, cell.end - cell.begin);
      if (cell.quoted || cellType(raw) != CellType::Text) {
        return true;
      }
      for (std::size_t i = 0; i < raw.size(); ++i) {
        const bool separator = raw[i] == ',' || raw[i] == ';' || raw[i] == '|';
        if (raw[i] == '\t' || (separator && (i + 1 == raw.size() || raw[i + 1] != ' '))) {
          return false;
        }
      }
      return true;
    }

    // How often rows share the most common cell count, weighted by that count and by the share of clean cells.
    // Empty lines and a row cut by the end of the sample do not count.
    double delimiterScore(std::string_view sample, bool cut, const Syntax& syntax) {
      std::vector<std::size_t> counts;
      std::vector<Cell> cells;
      std::size_t clean = 0;
      std::size_t scored = 0;
      std::size_t pos = 0;
      while (pos < sample.size()) {
        cells.clear();
        const RecordScan record = scanRecord(sample, pos, syntax, cells, kScoredCellsPerRow);
        pos = record.end;
        if (record.fieldCount == 0 || (cut && pos >= sample.size())) {
          continue;
        }
        counts.push_back(record.fieldCount);
        scored += cells.size();
        clean += static_cast<std::size_t>(std::ranges::count_if(cells, [&](const Cell& cell) {
          return cleanCell(sample, cell);
        }));
      }
      if (counts.empty() || scored == 0) {
        return 0.0;
      }
      std::ranges::sort(counts);
      std::size_t mode = 0;
      std::size_t modeRows = 0;
      for (std::size_t i = 0; i < counts.size();) {
        std::size_t j = i;
        while (j < counts.size() && counts[j] == counts[i]) {
          ++j;
        }
        if (j - i >= modeRows) {
          modeRows = j - i;
          mode = counts[i];
        }
        i = j;
      }
      const double consistency = static_cast<double>(modeRows) / static_cast<double>(counts.size());
      const double width = static_cast<double>(mode - 1) / static_cast<double>(mode);
      return consistency * width * static_cast<double>(clean) / static_cast<double>(scored);
    }

    // The best scoring delimiter; earlier ones in kDelimiters win ties, and a comma wins when none scores.
    Delimiter detectDelimiter(std::string_view sample, bool cut, Dialect dialect) {
      Delimiter best = Delimiter::Comma;
      double bestScore = 0.0;
      for (const Delimiter delimiter : kDelimiters) {
        dialect.delimiter = delimiter;
        const double score = delimiterScore(sample, cut, syntaxOf(dialect));
        if (score > bestScore) {
          best = delimiter;
          bestScore = score;
        }
      }
      return best;
    }

    // The most common line ending outside quotes; LF, then CRLF, then CR win ties.
    RowEnding detectLineEnding(std::string_view sample, bool cut, const Dialect& dialect) {
      std::array<std::size_t, 3> counts{};
      std::vector<Cell> unused;
      const Syntax syntax = syntaxOf(dialect);
      std::size_t pos = 0;
      while (pos < sample.size()) {
        const RecordScan record = scanRecord(sample, pos, syntax, unused, 0);
        pos = record.end;
        if (cut && pos >= sample.size()) {
          break;
        }
        if (record.terminator != Terminator::None) {
          ++counts[static_cast<std::size_t>(record.terminator) - 1];
        }
      }
      const auto most = std::ranges::distance(counts.begin(), std::ranges::max_element(counts));
      return static_cast<RowEnding>(most);
    }

    // Columns vote: a typed column under a name of another kind, or same-length values under a name of another
    // length, say header; a name like its column says data. Without votes, distinct non-empty text is a header.
    bool detectHeader(std::string_view sample, const Dialect& dialect) {
      const TableLimits limits{
          .maxRows = kHeaderSampleRows + 1,
          .maxColumns = kHeaderColumns,
          .maxCells = (kHeaderSampleRows + 1) * kHeaderColumns,
      };
      const Table table = readTable(sample, dialect, limits);
      const auto first = rowCells(table, 0);
      std::vector<std::string> names;
      std::vector<CellType> types;
      for (const Cell& cell : first) {
        const CellText name = cellText(sample, dialect, cell, kMaxTypedBytes + 1);
        types.push_back(name.clipped ? CellType::Text : cellType(name.text));
        names.push_back(name.text);
      }
      int votes = 0;
      for (std::size_t column = 0; column < names.size(); ++column) {
        const ColumnSample values = columnSample(sample, dialect, table, column, 1, table.rows.size());
        if (values.type != CellType::Text) {
          const bool named = types[column] == CellType::Text && !names[column].empty();
          votes += types[column] == values.type ? -1 : (named ? 1 : 0);
        } else if (values.values >= 2 && values.commonLength) {
          votes += *values.commonLength == names[column].size() ? -1 : 1;
        }
      }
      if (votes != 0) {
        return votes > 0;
      }
      for (std::size_t column = 0; column < names.size(); ++column) {
        const auto repeats =
            std::ranges::count(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(column), names[column]);
        if (names[column].empty() || types[column] != CellType::Text || repeats > 0) {
          return false;
        }
      }
      return !names.empty();
    }
  } // namespace

  Dialect detectDialect(std::string_view bytes, const DialectHint& hint) {
    const bool mark = bytes.starts_with(kByteOrderMark);
    const std::string_view content = bytes.substr(mark ? kByteOrderMark.size() : 0);
    const std::string_view sample = content.substr(0, kDialectSampleBytes);
    const bool cut = content.size() > sample.size();
    // The sample starts after the mark, so it is read with byteOrderMark off.
    Dialect dialect;
    dialect.quote = detectQuote(sample);
    dialect.escape = detectEscape(sample, quoteChar(dialect.quote));
    dialect.delimiter = hint.delimiter ? *hint.delimiter : detectDelimiter(sample, cut, dialect);
    dialect.lineEnding = detectLineEnding(sample, cut, dialect);
    dialect.header = hint.header ? *hint.header : detectHeader(sample, dialect);
    dialect.byteOrderMark = mark;
    dialect.finalNewline = !content.empty() && isLineBreak(content.back());
    return dialect;
  }
} // namespace luciole::formats
