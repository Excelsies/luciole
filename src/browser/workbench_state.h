#pragma once

#include "fs/location.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace luciole {
  // A folder or file that a part of a Workbench page points at.
  struct WorkbenchEntry {
    Location location;
    bool folder = false;
    bool operator==(const WorkbenchEntry&) const = default;
  };

  enum class WorkbenchPane : std::uint8_t { Instrument, JumpAhead, Ribbon, Strips, List, ColumnA, ColumnB, ColumnC };

  struct WorkbenchHover {
    WorkbenchEntry entry;
    WorkbenchPane pane = WorkbenchPane::List;
    bool operator==(const WorkbenchHover&) const = default;
  };

  enum class WorkbenchTargetSource : std::uint8_t { Cursor, Pointer };

  // State shared by the parts of one Workbench page: the hovered entry, what the columns ahead follow (the list's
  // cursor, or a row the pointer rested on), the entries the columns ahead are pinned to, and whether the instrument
  // covers the page. Time is passed in, so nothing here reads a clock. Every visible change bumps serial().
  //
  // The pointer rests when it stays within kRestRadius of where the wait began for kDwell; travel beyond it, or
  // another row, starts the wait again. A row of the list it rested on becomes the target; a row of column A or B
  // becomes that column's pin, which the next column follows, and a pin in column B also pins the folder B lists in
  // column A. Leaving the list and the columns for kRelease, or pointing at empty list space for as long, falls back
  // to the cursor. A click pin locks out pointer changes until unpin(), a cursor move or reset(). While a preview is
  // edited, setFrozen(true) holds the target against the pointer: it drops the wait and the release, and the pointer
  // starts none, so nothing is due. A new cursor, a pin, unpin() and reset() still change the target. While
  // holdCursor(true), a cursor that goes away with no new one is held, as when a save replaces the edited file and the
  // listing drops it with the list's cursor; ending the hold, or reset(), lets a held cursor go.
  class WorkbenchState {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kDwell{250};
    static constexpr std::chrono::milliseconds kRelease{300};
    static constexpr double kRestRadius = 6;
    static constexpr std::size_t kPinnedColumns = 2; // columns A and B steer the column after them

    std::uint64_t serial() const { return m_serial; }

    // The pane points at `entry`; unhover clears it only while that pane still owns the hover.
    void hover(WorkbenchPane pane, const WorkbenchEntry& entry);
    void unhover(WorkbenchPane pane);
    const std::optional<WorkbenchHover>& hovered() const { return m_hovered; }

    // The list's cursor entry, set every frame; a different entry forgets the pointer target, pins and waits.
    void setCursor(std::optional<WorkbenchEntry> entry);

    // The pointer is over a list row (none for empty space) or over a row of column 0, 1 or 2, at x, y.
    void pointerInList(std::optional<WorkbenchEntry> row, double x, double y, Clock::time_point now);
    void
    pointerInColumn(std::size_t column, std::optional<WorkbenchEntry> row, double x, double y, Clock::time_point now);
    // The pointer is outside the list and the columns.
    void pointerLeft(Clock::time_point now);
    // Applies a wait or a release that is due; every pointer call does this first.
    void advance(Clock::time_point now);
    // When advance() next has something to do; none while nothing waits.
    std::optional<Clock::time_point> deadline() const;

    // A click on a row of column 0 or 1 pins it, as a rest does, and locks out pointer changes; other columns are
    // ignored.
    void pin(std::size_t column, const WorkbenchEntry& entry);
    void unpin();
    std::optional<WorkbenchEntry> pinned(std::size_t column) const;
    bool locked() const { return m_locked; }
    void setFrozen(bool frozen);
    void holdCursor(bool hold);

    // The pointer target while there is one, else the cursor entry.
    std::optional<WorkbenchEntry> target() const;
    WorkbenchTargetSource targetSource() const;

    void setExpanded(bool expanded);
    bool expanded() const { return m_expanded; }

    // The folder changed: forgets the hover, the pointer target, pins, the lock and every wait.
    void reset();

  private:
    struct Wait {
      std::optional<std::size_t> column; // none for the list
      WorkbenchEntry entry;
      double x = 0;
      double y = 0;
      Clock::time_point start;
    };
    void rest(std::optional<std::size_t> column, const WorkbenchEntry& row, double x, double y, Clock::time_point now);
    void clearPins();
    void bump() { ++m_serial; }

    std::uint64_t m_serial = 0;
    std::optional<WorkbenchHover> m_hovered;
    std::optional<WorkbenchEntry> m_cursor;
    std::optional<WorkbenchEntry> m_pointer;
    std::array<std::optional<WorkbenchEntry>, kPinnedColumns> m_pins;
    std::optional<Wait> m_wait;
    std::optional<Clock::time_point> m_release;
    bool m_locked = false;
    bool m_frozen = false;
    bool m_holdCursor = false;
    bool m_cursorHeld = false; // the cursor went away and m_cursor still holds it
    bool m_expanded = false;
  };
} // namespace luciole
