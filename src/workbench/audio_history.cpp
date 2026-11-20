#include "workbench/audio_history.h"

#include <algorithm>
#include <stdexcept>

namespace luciole::workbench {
  AudioHistory::AudioHistory(std::size_t width, std::size_t rows) : m_width(width), m_rows(rows) {
    if (width == 0 || rows == 0 || width > kMaxSide || rows > kMaxSide) {
      throw std::invalid_argument("a sound history side must be between 1 and 1024 texels");
    }
    m_texels.assign(width * rows * kBytesPerTexel, 0);
  }

  std::span<std::uint8_t> AudioHistory::writeRow(std::size_t index) {
    if (index >= m_rows) {
      throw std::out_of_range("a sound history row past the last one");
    }
    if (m_pending.change == HistoryChange::None) {
      m_pending = {.change = HistoryChange::Row, .row = index};
    } else if (m_pending.change == HistoryChange::Row && m_pending.row != index) {
      m_pending = {.change = HistoryChange::All};
    }
    const std::size_t stride = m_width * kBytesPerTexel;
    return std::span(m_texels).subspan(index * stride, stride);
  }

  void AudioHistory::clear() {
    std::ranges::fill(m_texels, std::uint8_t{0});
    m_pending = {.change = HistoryChange::All};
  }

  HistoryUpdate AudioHistory::takeUpdate() {
    const HistoryUpdate update = m_pending;
    m_pending = {};
    return update;
  }
} // namespace luciole::workbench
