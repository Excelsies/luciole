#pragma once

#include "editing/text_history.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The text model of the code editor: valid UTF-8 lines separated by LF, a selection, edits and undo.
namespace luciole::editing {
  inline constexpr std::size_t kEditByteLimit = std::size_t{1024} * 1024;
  inline constexpr std::size_t kEditLineBytes = std::size_t{16} * 1024;

  // The lines an edit changed, nothing when the text stayed the same, or why the edit was refused.
  using EditResult = std::expected<std::optional<LineSpan>, std::string>;

  // What an erase removes when the selection is empty. LineEdge erases to the line start or end, and the line
  // break when the cursor is already there.
  enum class EraseUnit : std::uint8_t { Cluster, Word, LineEdge };

  // One indent level: a tab, or `spaces` spaces. `spaces` is ignored when `tab` is set.
  struct IndentUnit {
    bool tab = false;
    std::size_t spaces = 4;
    bool operator==(const IndentUnit&) const = default;
  };

  class TextBuffer {
  public:
    // Refuses invalid UTF-8, CR and NUL bytes, text over kEditByteLimit and lines over kEditLineBytes. The
    // selection starts collapsed at the document start.
    static std::expected<TextBuffer, std::string> create(std::string_view text);

    // The lines joined by LF: the text the buffer was created with, after the edits.
    std::string text() const;
    std::size_t lineCount() const { return m_lines.size(); }
    // Throws std::out_of_range past the last line.
    std::string_view line(std::size_t index) const { return m_lines.at(index); }
    std::size_t byteCount() const { return m_bytes; }

    // The nearest position at or before `position` that is on a code point boundary.
    TextPosition clamp(TextPosition position) const;
    // The text from the earlier to the later of the two positions, lines joined by LF. Both are clamped first.
    std::string textBetween(TextPosition first, TextPosition second) const;
    TextSelection selection() const { return m_selection; }
    // Clamps both ends. A selection that differs from the current one ends the typing group.
    void setSelection(TextSelection selection);

    // Unless it shifts whole lines, an edit replaces a non-empty selection and leaves the cursor collapsed after the
    // change. Every edit refuses, whole, a change that would break a limit of create(). `nowMs` is a monotonic time
    // used to group typing.

    // CRLF and lone CR become LF; invalid UTF-8 and NUL are refused.
    [[nodiscard]] EditResult insertText(std::string_view text, std::uint64_t nowMs);
    // Inserts a line break and the blanks that start the current line before the cursor.
    [[nodiscard]] EditResult newline(std::uint64_t nowMs);
    [[nodiscard]] EditResult eraseBackward(EraseUnit unit, std::uint64_t nowMs);
    [[nodiscard]] EditResult eraseForward(EraseUnit unit, std::uint64_t nowMs);
    // A selection over several lines indents each of them except empty ones and a last line the selection only
    // reaches at byte 0. Otherwise it replaces the selection with one unit. A unit of zero spaces is refused.
    [[nodiscard]] EditResult indent(IndentUnit unit, std::uint64_t nowMs);
    // A non-empty selection removes one unit from the start of each line it covers (a tab, or up to `spaces`
    // spaces). An empty one removes one unit just before the cursor.
    [[nodiscard]] EditResult outdent(IndentUnit unit, std::uint64_t nowMs);

    // Reverts or reapplies one history step and restores the selection it held; nothing at the end of the log.
    [[nodiscard]] std::optional<LineSpan> undo();
    [[nodiscard]] std::optional<LineSpan> redo();

    // Grows by one with every content change, undo and redo included.
    std::uint64_t revision() const { return m_history.revision(); }
    // Whether the current state is not the saved one. A new buffer is not modified.
    bool modified() const { return m_history.modified(); }
    // Marks the state written at `revision` as saved; false when that state can no longer be reached.
    bool markSaved(std::uint64_t revision);

  private:
    TextBuffer() = default;

    std::optional<std::string> refusal(TextPosition start, TextPosition end, std::string_view text) const;
    LineSpan apply(TextPosition start, TextPosition end, std::string_view text);
    EditResult edit(
        TextSelection range, std::string_view text, std::optional<TextSelection> after, StepKind kind,
        std::uint64_t nowMs
    );
    EditResult erase(TextPosition target, StepKind kind, std::uint64_t nowMs);
    EditResult shiftLines(IndentUnit unit, bool inward, std::uint64_t nowMs);
    std::optional<LineSpan> replay(const HistoryStep* step, bool forward);

    std::vector<std::string> m_lines{std::string()};
    std::size_t m_bytes = 0;
    TextSelection m_selection;
    TextHistory m_history;
  };

  // The unit the document uses: a tab when most indented lines start with one, else the most common step among
  // 2, 4 and 8 spaces between consecutive non-blank lines (ties prefer 4, then 2), else 4 spaces.
  IndentUnit detectIndent(const TextBuffer& buffer);
} // namespace luciole::editing
