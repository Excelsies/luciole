#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace luciole::workbench {
  enum class HistoryChange : std::uint8_t { None, Row, All };

  // What an AudioHistory needs uploaded: nothing, one row, or every texel.
  struct HistoryUpdate {
    HistoryChange change = HistoryChange::None;
    std::size_t row = 0;
  };

  // A small RGBA8 image that a sound visual writes one row at a time and mirrors into a texture. It remembers what
  // changed since the last upload.
  class AudioHistory {
  public:
    static constexpr std::size_t kMaxSide = 1024;
    static constexpr std::size_t kBytesPerTexel = 4;

    // Throws std::invalid_argument when a side is 0 or above kMaxSide. Starts with every texel 0.
    AudioHistory(std::size_t width, std::size_t rows);

    std::size_t width() const { return m_width; }
    std::size_t rows() const { return m_rows; }
    // Every texel, row 0 first, four bytes each.
    std::span<const std::uint8_t> texels() const { return m_texels; }

    // Row `index` to write, marked as changed. Throws std::out_of_range past the last row.
    [[nodiscard]] std::span<std::uint8_t> writeRow(std::size_t index);
    // Sets every texel to 0; everything needs an upload.
    void clear();
    // What changed since the previous call: one row when a single row was written, everything after a clear, at
    // construction, or when more than one row was written.
    HistoryUpdate takeUpdate();

  private:
    std::size_t m_width;
    std::size_t m_rows;
    std::vector<std::uint8_t> m_texels;
    HistoryUpdate m_pending{.change = HistoryChange::All};
  };
} // namespace luciole::workbench
