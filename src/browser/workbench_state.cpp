#include "browser/workbench_state.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

namespace {
  bool anyPinned(std::span<const std::optional<luciole::WorkbenchEntry>> pins) {
    return std::ranges::any_of(pins, [](const auto& pin) { return pin.has_value(); });
  }

  // Pins `entry` in `column`; pins after it belonged to the column that changed. The folder the column lists is
  // pinned in the column before, so a changing pick there cannot take the column away.
  void
  pinAt(std::span<std::optional<luciole::WorkbenchEntry>> pins, std::size_t column, luciole::WorkbenchEntry entry) {
    if (column > 0) {
      if (auto folder = entry.location.parent()) {
        pins[column - 1] = luciole::WorkbenchEntry{.location = std::move(*folder), .folder = true};
      }
    }
    pins[column] = std::move(entry);
    std::ranges::fill(pins.subspan(column + 1), std::nullopt);
  }
} // namespace

void luciole::WorkbenchState::hover(WorkbenchPane pane, const WorkbenchEntry& entry) {
  WorkbenchHover next{.entry = entry, .pane = pane};
  if (m_hovered != next) {
    m_hovered = std::move(next);
    bump();
  }
}

void luciole::WorkbenchState::unhover(WorkbenchPane pane) {
  if (m_hovered && m_hovered->pane == pane) {
    m_hovered.reset();
    bump();
  }
}

void luciole::WorkbenchState::setCursor(std::optional<WorkbenchEntry> entry) {
  if (m_holdCursor && !entry && m_cursor) {
    m_cursorHeld = true;
    return;
  }
  m_cursorHeld = false;
  if (m_cursor == entry) {
    return;
  }
  m_cursor = std::move(entry);
  m_pointer.reset();
  clearPins();
  m_locked = false;
  m_wait.reset();
  m_release.reset();
  bump();
}

void luciole::WorkbenchState::rest(
    std::optional<std::size_t> column, const WorkbenchEntry& row, double x, double y, Clock::time_point now
) {
  if (m_frozen) {
    return;
  }
  if (m_wait && m_wait->column == column && m_wait->entry == row) {
    if (std::hypot(x - m_wait->x, y - m_wait->y) <= kRestRadius) {
      return;
    }
  }
  m_wait = Wait{.column = column, .entry = row, .x = x, .y = y, .start = now};
}

void luciole::WorkbenchState::pointerInList(
    std::optional<WorkbenchEntry> row, double x, double y, Clock::time_point now
) {
  advance(now);
  if (!row) {
    m_wait.reset();
    if (!m_release && !m_frozen) {
      m_release = now + kRelease;
    }
    return;
  }
  m_release.reset();
  if (m_pointer == row) {
    m_wait.reset();
    return;
  }
  rest(std::nullopt, *row, x, y, now);
}

void luciole::WorkbenchState::pointerInColumn(
    std::size_t column, std::optional<WorkbenchEntry> row, double x, double y, Clock::time_point now
) {
  advance(now);
  m_release.reset();
  if (column >= kPinnedColumns || !row || m_pins[column] == row) {
    m_wait.reset();
    return;
  }
  rest(column, *row, x, y, now);
}

void luciole::WorkbenchState::pointerLeft(Clock::time_point now) {
  advance(now);
  m_wait.reset();
  if (!m_release && !m_frozen) {
    m_release = now + kRelease;
  }
}

void luciole::WorkbenchState::advance(Clock::time_point now) {
  const bool held = m_locked || m_frozen;
  if (m_wait && now >= m_wait->start + kDwell) {
    auto wait = std::move(*m_wait);
    m_wait.reset();
    if (held) {
      return;
    }
    if (!wait.column) {
      m_pointer = std::move(wait.entry);
      clearPins();
      bump();
      return;
    }
    pinAt(m_pins, *wait.column, std::move(wait.entry));
    bump();
    return;
  }
  if (m_release && now >= *m_release) {
    m_release.reset();
    if (held || (!m_pointer && !anyPinned(m_pins))) {
      return;
    }
    m_pointer.reset();
    clearPins();
    bump();
  }
}

std::optional<luciole::WorkbenchState::Clock::time_point> luciole::WorkbenchState::deadline() const {
  if (m_wait) {
    return m_wait->start + kDwell;
  }
  return m_release;
}

void luciole::WorkbenchState::pin(std::size_t column, const WorkbenchEntry& entry) {
  if (column >= kPinnedColumns) {
    return;
  }
  m_wait.reset();
  pinAt(m_pins, column, entry);
  m_locked = true;
  bump();
}

void luciole::WorkbenchState::unpin() {
  if (!m_locked && !anyPinned(m_pins)) {
    return;
  }
  m_locked = false;
  clearPins();
  bump();
}

std::optional<luciole::WorkbenchEntry> luciole::WorkbenchState::pinned(std::size_t column) const {
  return column < kPinnedColumns ? m_pins[column] : std::nullopt;
}

void luciole::WorkbenchState::setFrozen(bool frozen) {
  if (m_frozen != frozen) {
    m_frozen = frozen;
    if (frozen) {
      m_wait.reset();
      m_release.reset();
    }
    bump();
  }
}

void luciole::WorkbenchState::holdCursor(bool hold) {
  m_holdCursor = hold;
  if (!hold && m_cursorHeld) {
    setCursor(std::nullopt);
  }
}

std::optional<luciole::WorkbenchEntry> luciole::WorkbenchState::target() const {
  return m_pointer ? m_pointer : m_cursor;
}

luciole::WorkbenchTargetSource luciole::WorkbenchState::targetSource() const {
  return m_pointer ? WorkbenchTargetSource::Pointer : WorkbenchTargetSource::Cursor;
}

void luciole::WorkbenchState::setExpanded(bool expanded) {
  if (m_expanded != expanded) {
    m_expanded = expanded;
    bump();
  }
}

void luciole::WorkbenchState::reset() {
  if (std::exchange(m_cursorHeld, false)) {
    m_cursor.reset(); // a held cursor belongs to the folder left
  }
  m_hovered.reset();
  m_pointer.reset();
  clearPins();
  m_locked = false;
  m_wait.reset();
  m_release.reset();
  bump();
}

void luciole::WorkbenchState::clearPins() { std::ranges::fill(m_pins, std::nullopt); }
