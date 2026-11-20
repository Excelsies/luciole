#pragma once

#include "editing/markdown_source.h"
#include "formats/code_tokens.h"
#include "formats/file_kind.h"
#include "formats/styled_text.h"
#include "formats/text_lines.h"
#include "formats/text_tokens.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Styles for the lines of a text while it is edited: line N of the text is line N on screen and every character
// keeps its column.
namespace luciole::editing {
  // Styled lines kept between calls; the least recently asked for is dropped first.
  inline constexpr std::size_t kCachedStyledLines = 256;

  // Lines a text may have after an edit: a 1 MiB text holds at most this many.
  inline constexpr std::size_t kMaxSourceLines = std::size_t{1024} * 1024 + 1;

  // Line `index` of the text without its line break: valid UTF-8 without a line feed. The view only has to stay valid
  // until the next call.
  using LineAt = std::function<std::string_view(std::size_t index)>;

  struct LineRange {
    std::size_t first = 0;
    std::size_t count = 0;
    bool operator==(const LineRange&) const = default;
  };

  enum class StyleError : std::uint8_t {
    LineOutOfRange, // the index is not below lineCount()
    InvalidLine,    // the line is not valid UTF-8, or holds a line feed
    EditOutOfRange, // the removed lines run past the end of the text, or the text would pass kMaxSourceLines lines
  };

  // What a line leaves open for the next one.
  struct LineState {
    formats::tokenize::ScanState scan;
    MarkdownFence fence;
  };

  bool sameState(const LineState& a, const LineState& b);

  class SourceStyles {
  public:
    // Scans nothing yet: line(i) first scans the lines before i that were not scanned, to learn the state each one
    // starts in. `lineAt` must return std::string_view itself, so a callable returning a string by value, whose view
    // would dangle, does not compile.
    template <typename F>
    SourceStyles(const formats::FileKind& kind, std::size_t lineCount, F lineAt)
        : SourceStyles(kind, lineCount, LineAt(std::move(lineAt)), Checked{}) {
      static_assert(
          std::is_same_v<std::invoke_result_t<F&, std::size_t>, std::string_view>, "lineAt must return std::string_view"
      );
    }

    std::size_t lineCount() const { return m_lineCount; }

    // Runs that, joined, hold the line's code points in order, ASCII controls other than tab as their Control
    // Pictures symbols, colored by the tokenizer of the file's kind (JSON as it stands, Markdown as source). Swatches
    // are the line's color codes at code point columns, with color names only in CSS and SCSS. No length limit.
    std::expected<formats::StyledLine, StyleError> line(std::size_t index);

    // Call after lines [firstLine, firstLine + removedLines) were replaced by `insertedLines` lines. Rescans from
    // firstLine until a following line starts in the state it started in before, and returns the lines whose style
    // may have changed: the inserted lines and the following lines whose starting state changed. Lines past the last
    // one scanned so far, which line() never returned, are not scanned and not counted.
    std::expected<LineRange, StyleError>
    edited(std::size_t firstLine, std::size_t removedLines, std::size_t insertedLines);

    // Styled lines kept, at most kCachedStyledLines.
    std::size_t cachedLines() const { return m_cache.size(); }

  private:
    struct Checked {};

    SourceStyles(const formats::FileKind& kind, std::size_t lineCount, LineAt lineAt, Checked checked);

    struct Cached {
      std::size_t index = 0;
      std::uint64_t used = 0;
      formats::StyledLine line;
    };

    // The text with ASCII controls other than tab as Control Pictures; a view of m_clean or of the text itself.
    std::string_view cleanLine(std::string_view text);
    LineState scan(std::string_view text, const LineState& start);
    formats::StyledLine styled(std::string_view text, const LineState& start);
    void scanTo(std::size_t index);
    LineRange rescan(std::size_t firstLine, std::size_t removedLines, std::size_t insertedLines);
    void remember(std::size_t index, const formats::StyledLine& line);
    void moveCache(std::size_t firstLine, std::size_t removedLines, std::size_t insertedLines, LineRange changed);

    formats::FileKind m_kind;
    formats::TextSyntax m_syntax;
    formats::tokenize::CodeOptions m_options;
    LineAt m_lineAt;
    std::size_t m_lineCount = 0;
    std::vector<LineState> m_states; // the start state of each line scanned so far, and of the line after them
    std::vector<Cached> m_cache;
    std::uint64_t m_uses = 0;
    std::string m_clean;        // a line with its controls replaced
    formats::TextLine m_tokens; // tokens of the line being scanned
  };
} // namespace luciole::editing
