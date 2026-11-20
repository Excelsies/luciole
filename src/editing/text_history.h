#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

// Positions and selections in editor text, and the operation log a TextBuffer keeps for undo and redo.
namespace luciole::editing {
  // A line index and a byte offset in that line.
  struct TextPosition {
    std::size_t line = 0;
    std::size_t byte = 0;
    auto operator<=>(const TextPosition&) const = default;
  };

  // The anchor stays where the selection began; the cursor is the end that moves.
  struct TextSelection {
    TextPosition anchor;
    TextPosition cursor;
    bool operator==(const TextSelection&) const = default;
    bool empty() const { return anchor == cursor; }
    TextPosition start() const { return std::min(anchor, cursor); }
    TextPosition end() const { return std::max(anchor, cursor); }
  };

  // Lines firstLine to firstLine + removedLines - 1 were replaced by insertedLines lines.
  struct LineSpan {
    std::size_t firstLine = 0;
    std::size_t removedLines = 0;
    std::size_t insertedLines = 0;
    bool operator==(const LineSpan&) const = default;
  };

  // The position after `text` (LF separated) when it is inserted at `start`.
  TextPosition endOfText(TextPosition start, std::string_view text);

  // One undoable change: `removed` began at `start` and was replaced by `inserted`.
  struct HistoryStep {
    TextPosition start;
    std::string removed;
    std::string inserted;
    TextSelection before;
    TextSelection after;
  };

  // Typing joins single-cluster inserts that continue at the cursor; the erase kinds join consecutive erases in
  // one direction. Other never joins.
  enum class StepKind : std::uint8_t { Typing, EraseBackward, EraseForward, Other };

  inline constexpr std::size_t kHistorySteps = 1000;
  inline constexpr std::size_t kHistoryBytes = std::size_t{8} * 1024 * 1024;
  inline constexpr std::uint64_t kTypingGroupMs = 1000;
  // markSaved recognizes this many of the latest revisions.
  inline constexpr std::size_t kRevisionMemory = 4096;

  // The undo log. Each content state reachable by undo and redo has an id; a revision maps to the state the
  // buffer held at that revision, so a save can be matched to a state after more edits.
  class TextHistory {
  public:
    TextHistory();

    // Records a change the buffer has applied. It joins the previous step when both are of the same joining kind,
    // the new one continues where the previous ended, no more than kTypingGroupMs passed and nothing closed the
    // group. Steps after the current position are discarded; the oldest steps are dropped past kHistorySteps or
    // kHistoryBytes of removed and inserted text.
    void record(HistoryStep step, StepKind kind, std::uint64_t nowMs);
    // The step to revert, or null at the oldest position. The caller applies it.
    const HistoryStep* undoStep();
    // The step to apply again, or null at the newest position. The caller applies it.
    const HistoryStep* redoStep();
    // The next step starts a new group.
    void closeGroup();
    // Marks the state the buffer held at `revision` as the saved one and returns true when that state is still
    // reachable. Otherwise no state is saved and modified() stays true until a later markSaved succeeds.
    bool markSaved(std::uint64_t revision);
    bool modified() const;
    // Grows by one with every recorded step, undo and redo.
    std::uint64_t revision() const { return m_revision; }

  private:
    bool joins(const HistoryStep& step, StepKind kind, std::uint64_t nowMs) const;
    void join(const HistoryStep& step, StepKind kind);
    void dropOldest();
    void advanceRevision();
    bool reachable(std::uint64_t stateId) const;

    std::deque<HistoryStep> m_steps;
    std::deque<std::uint64_t> m_stateIds; // the state after 0, 1, ... m_steps.size() steps
    std::size_t m_current = 0;            // steps applied
    std::size_t m_storedBytes = 0;
    std::uint64_t m_nextStateId = 1;
    std::optional<std::uint64_t> m_savedStateId;
    std::optional<StepKind> m_openGroup;
    std::uint64_t m_groupTimeMs = 0;
    std::uint64_t m_revision = 0;
    std::deque<std::uint64_t> m_revisionStates; // state id at each revision from m_firstLoggedRevision on
    std::uint64_t m_firstLoggedRevision = 0;
  };
} // namespace luciole::editing
