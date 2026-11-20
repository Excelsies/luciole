#include "formats/table_grid.h"

#include <algorithm>
#include <utility>

namespace luciole::formats {
  namespace {
    constexpr std::string_view kReplacement = "\xEF\xBF\xBD";

    bool continuation(unsigned char byte, unsigned char low = 0x80, unsigned char high = 0xBF) {
      return byte >= low && byte <= high;
    }

    // Bytes in the valid UTF-8 sequence at the start of `text`, or 0. Strict: no overlong forms, surrogates or
    // values past U+10FFFF.
    std::size_t sequenceLength(std::string_view text) {
      const auto at = [&](std::size_t i) { return i < text.size() ? static_cast<unsigned char>(text[i]) : 0; };
      const unsigned char lead = at(0);
      if (lead >= 0xC2 && lead <= 0xDF) {
        return continuation(at(1)) ? 2 : 0;
      }
      if (lead >= 0xE0 && lead <= 0xEF) {
        const unsigned char low = lead == 0xE0 ? 0xA0 : 0x80;
        const unsigned char high = lead == 0xED ? 0x9F : 0xBF;
        return continuation(at(1), low, high) && continuation(at(2)) ? 3 : 0;
      }
      if (lead >= 0xF0 && lead <= 0xF4) {
        const unsigned char low = lead == 0xF0 ? 0x90 : 0x80;
        const unsigned char high = lead == 0xF4 ? 0x8F : 0xBF;
        return continuation(at(1), low, high) && continuation(at(2)) && continuation(at(3)) ? 4 : 0;
      }
      return 0;
    }
  } // namespace

  std::string_view gridCellText(const TableGrid& grid, std::size_t row, std::size_t column) {
    if (row >= grid.rowCount || column >= grid.columnCount) {
      return {};
    }
    const std::size_t index = (row * grid.columnCount) + column;
    if (index >= grid.ends.size()) {
      return {};
    }
    const std::size_t begin = index == 0 ? 0 : grid.ends[index - 1];
    const std::size_t end = grid.ends[index];
    if (begin > end || end > grid.text.size()) {
      return {};
    }
    return std::string_view(grid.text).substr(begin, end - begin);
  }

  GridCellKind gridCellKind(const TableGrid& grid, std::size_t row, std::size_t column) {
    if (row >= grid.rowCount || column >= grid.columnCount) {
      return GridCellKind::Text;
    }
    const std::size_t index = (row * grid.columnCount) + column;
    return index < grid.kinds.size() ? grid.kinds[index] : GridCellKind::Text;
  }

  std::string columnLetters(std::size_t index) {
    std::string out;
    // Each step divides by 26, so a 64-bit index takes at most 14.
    for (std::size_t step = 0; step < 16; ++step) {
      out.insert(out.begin(), static_cast<char>('A' + (index % 26)));
      if (index < 26) {
        break;
      }
      index = (index / 26) - 1;
    }
    return out;
  }

  std::string gridColumnName(const TableGrid& grid, std::size_t column) {
    if (column < grid.columnNames.size() && !grid.columnNames[column].empty()) {
      return grid.columnNames[column];
    }
    return columnLetters(grid.firstSourceColumn + column);
  }

  std::string gridText(std::string_view text) {
    std::string out;
    out.reserve(std::min(text.size(), kMaxGridCellBytes));
    for (std::size_t i = 0; i < text.size();) {
      const auto byte = static_cast<unsigned char>(text[i]);
      std::string_view piece = text.substr(i, 1);
      std::size_t advance = 1;
      if (byte < 0x20 || byte == 0x7F) {
        piece = " ";
      } else if (byte >= 0x80) {
        advance = sequenceLength(text.substr(i));
        const bool c1Control = advance == 2 && byte == 0xC2 && static_cast<unsigned char>(text[i + 1]) <= 0x9F;
        piece = advance == 0 ? kReplacement : c1Control ? " " : text.substr(i, advance);
        advance = std::max<std::size_t>(advance, 1);
      }
      if (out.size() + piece.size() > kMaxGridCellBytes) {
        break;
      }
      out += piece;
      i += advance;
    }
    return out;
  }

  TableGridBuilder::TableGridBuilder(std::size_t rows, std::size_t columns)
      : m_requestedRows(rows), m_requestedColumns(columns), m_rows(std::min(rows, kMaxGridRows)),
        m_columns(std::min(columns, kMaxGridColumns)) {
    if (m_columns > 0) {
      m_rows = std::min(m_rows, kMaxGridCells / m_columns);
    }
    m_grid.rowsCut = m_rows < rows;
    m_grid.columnsCut = m_columns < columns;
    m_grid.truncated = m_grid.rowsCut || m_grid.columnsCut;
    m_grid.ends.reserve(m_rows * m_columns);
    m_grid.kinds.reserve(m_rows * m_columns);
  }

  bool TableGridBuilder::set(std::size_t row, std::size_t column, GridCellKind kind, std::string_view text) {
    if (row >= m_requestedRows || column >= m_requestedColumns) {
      return false;
    }
    if (m_started && (row < m_lastRow || (row == m_lastRow && column <= m_lastColumn))) {
      return false;
    }
    m_started = true;
    m_lastRow = row;
    m_lastColumn = column;
    if (m_full || row >= m_rows || column >= m_columns) {
      return true;
    }
    fillTo((row * m_columns) + column);
    std::string shown = gridText(text);
    if (shown.size() > kMaxGridTextBytes - m_grid.text.size()) {
      m_full = true;
      m_cutRow = row;
      m_grid.truncated = true;
      m_grid.rowsCut = true;
      return true;
    }
    m_grid.text += shown;
    m_grid.ends.push_back(static_cast<std::uint32_t>(m_grid.text.size()));
    m_grid.kinds.push_back(kind);
    return true;
  }

  TableGrid TableGridBuilder::finish() {
    const std::size_t rows = m_full ? m_cutRow : m_rows;
    const std::size_t cells = rows * m_columns;
    if (m_grid.ends.size() > cells) {
      m_grid.ends.resize(cells);
      m_grid.kinds.resize(cells);
      m_grid.text.resize(cells == 0 ? 0 : m_grid.ends.back());
    }
    fillTo(cells);
    m_grid.rowCount = rows;
    m_grid.columnCount = m_columns;
    return std::move(m_grid);
  }

  void TableGridBuilder::fillTo(std::size_t cell) {
    const auto end = static_cast<std::uint32_t>(m_grid.text.size());
    m_grid.ends.resize(std::max(m_grid.ends.size(), cell), end);
    m_grid.kinds.resize(std::max(m_grid.kinds.size(), cell), GridCellKind::Text);
  }
} // namespace luciole::formats
