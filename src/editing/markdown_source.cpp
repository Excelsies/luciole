#include "editing/markdown_source.h"

#include "formats/ascii.h"
#include "formats/markdown.h"
#include "formats/markdown_lines.h"
#include "formats/markdown_text.h"
#include "formats/styled_markdown.h"

#include <algorithm>
#include <array>
#include <optional>

namespace luciole::editing {
  namespace {
    namespace fmt = luciole::formats;
    namespace md = luciole::formats::markdown;
    using fmt::StyleRole;

    constexpr std::string_view kEscapable = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    constexpr std::size_t kMaxScheme = 32;
    constexpr std::size_t kMaxEmphasisRun = 3;

    // Appends runs for consecutive byte ranges of one line.
    struct RunWriter {
      std::string_view line;
      std::vector<fmt::StyledRun>& runs;
      std::size_t done = 0;

      // Bytes [done, end) in this style; nothing when end is not past done.
      void add(std::size_t end, StyleRole role, bool italic = false, bool bold = false) {
        if (end <= done) {
          return;
        }
        fmt::styled::appendRun(runs, line.substr(done, end - done), role, italic, bold);
        done = end;
      }
    };

    // A styled range found by the inline scan.
    struct Span {
      std::size_t end = 0;
      StyleRole role = StyleRole::Plain;
      bool italic = false;
      bool bold = false;
    };

    // What an inline scan has learned cannot match later in the line.
    struct Misses {
      std::vector<std::size_t> ticks;                              // code span run lengths without a closer
      std::array<std::array<bool, kMaxEmphasisRun>, 2> emphasis{}; // '*' and '_' closers of 1 to 3 markers
      std::size_t lastBracket = std::string_view::npos;            // the line's last ']'
      std::size_t lastParen = std::string_view::npos;              // the line's last ')'
    };

    bool blankAt(std::string_view s, std::size_t i) { return i >= s.size() || fmt::isBlank(s[i]); }

    // Bytes of multi-byte characters count as word characters.
    bool wordAt(std::string_view s, std::size_t i) {
      if (i >= s.size()) {
        return false;
      }
      const char c = s[i];
      return fmt::isAsciiAlpha(c) || fmt::isAsciiDigit(c) || static_cast<unsigned char>(c) >= 0x80U;
    }

    std::size_t runEnd(std::string_view s, std::size_t i, char c) {
      while (i < s.size() && s[i] == c) {
        ++i;
      }
      return i;
    }

    bool closesFence(std::string_view line, MarkdownFence fence) {
      const auto indent = md::indentOf(line);
      if (indent > md::kMaxIndent) {
        return false;
      }
      const auto end = runEnd(line, indent, fence.ch);
      return end - indent >= fence.count && md::isBlank(line.substr(end));
    }

    // The end of the code span whose opening run of `run` backquotes ends at `from`.
    std::optional<std::size_t> codeSpanEnd(std::string_view s, std::size_t from, std::size_t run, Misses& misses) {
      if (std::ranges::contains(misses.ticks, run)) {
        return std::nullopt;
      }
      auto j = from;
      // Each pass moves j forward by at least one byte.
      while (j < s.size()) {
        if (s[j] != '`') {
          ++j;
          continue;
        }
        const auto end = runEnd(s, j, '`');
        if (end - j == run) {
          return end;
        }
        j = end;
      }
      misses.ticks.push_back(run);
      return std::nullopt;
    }

    // Emphasis or strong opened by the run of `c` at i: markers and text. A run of three or more is both. The closer is
    // a whole run of as many markers, or of three or more after a run of three or more.
    std::optional<Span> emphasisAt(std::string_view s, std::size_t i, std::size_t run, Misses& misses) {
      const char c = s[i];
      const auto after = i + run;
      if (blankAt(s, after) || (c == '_' && i > 0 && wordAt(s, i - 1))) {
        return std::nullopt;
      }
      const auto markers = std::min(run, kMaxEmphasisRun);
      auto& missed = misses.emphasis.at(c == '*' ? 0 : 1).at(markers - 1);
      if (missed) {
        return std::nullopt;
      }
      auto j = after + 1;
      // Each pass moves j forward by at least one byte.
      while (j < s.size()) {
        if (s[j] != c) {
          ++j;
          continue;
        }
        const auto end = runEnd(s, j, c);
        const auto closer = std::min(end - j, kMaxEmphasisRun);
        if (closer == markers && !blankAt(s, j - 1) && (c != '_' || !wordAt(s, end))) {
          return Span{
              .end = end,
              .role = markers == 1 ? StyleRole::Emphasis : StyleRole::Strong,
              .italic = markers != 2,
              .bold = markers != 1,
          };
        }
        j = end;
      }
      missed = true;
      return std::nullopt;
    }

    // The index of the bracket closing the one at `open`, looking at most `limit` bytes ahead.
    std::optional<std::size_t> closingBracket(std::string_view s, std::size_t open, char close, std::size_t limit) {
      const char opener = s[open];
      std::size_t depth = 0;
      const auto stop = std::min(s.size(), open + 1 + limit);
      for (auto j = open + 1; j < stop; ++j) {
        if (s[j] == '\\') {
          ++j;
        } else if (s[j] == opener) {
          ++depth;
        } else if (s[j] == close) {
          if (depth == 0) {
            return j;
          }
          --depth;
        }
      }
      return std::nullopt;
    }

    // "[label](target)" or "![label](target)" at i.
    std::optional<Span> linkAt(std::string_view s, std::size_t i, const Misses& misses) {
      const auto open = s[i] == '!' ? i + 1 : i;
      if (open >= s.size()
          || s[open] != '['
          || misses.lastBracket == std::string_view::npos
          || open > misses.lastBracket) {
        return std::nullopt;
      }
      const auto label = closingBracket(s, open, ']', md::kMaxLabel);
      if (!label
          || *label + 1 >= s.size()
          || s[*label + 1] != '('
          || misses.lastParen == std::string_view::npos
          || *label > misses.lastParen) {
        return std::nullopt;
      }
      const auto target = closingBracket(s, *label + 1, ')', md::kMaxTarget);
      if (!target) {
        return std::nullopt;
      }
      return Span{.end = *target + 1, .role = StyleRole::Link};
    }

    // "<scheme:target>" at i.
    std::optional<Span> autolinkAt(std::string_view s, std::size_t i) {
      auto j = i + 1;
      if (j >= s.size() || !fmt::isAsciiAlpha(s[j])) {
        return std::nullopt;
      }
      const auto schemeStop = std::min(s.size(), j + kMaxScheme);
      while (j < schemeStop
             && (fmt::isAsciiAlpha(s[j]) || fmt::isAsciiDigit(s[j]) || std::string_view{"+.-"}.contains(s[j]))) {
        ++j;
      }
      if (j - i < 3 || j >= s.size() || s[j] != ':') {
        return std::nullopt;
      }
      while (j < s.size() && !fmt::isBlank(s[j]) && s[j] != '<' && s[j] != '>') {
        ++j;
      }
      if (j >= s.size() || s[j] != '>') {
        return std::nullopt;
      }
      return Span{.end = j + 1, .role = StyleRole::Link};
    }

    // Code spans, emphasis, strong, links and autolinks from `from` to the line end.
    void inlineRuns(RunWriter& w, std::size_t from) {
      const auto s = w.line;
      Misses misses;
      misses.lastBracket = s.rfind(']');
      misses.lastParen = s.rfind(')');
      auto i = from;
      // Each pass moves i forward by at least one byte.
      while (i < s.size()) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && kEscapable.contains(s[i + 1])) {
          i += 2;
          continue;
        }
        std::optional<Span> span;
        std::size_t skip = 1;
        if (c == '`') {
          skip = runEnd(s, i, '`') - i;
          if (const auto end = codeSpanEnd(s, i + skip, skip, misses)) {
            span = Span{.end = *end, .role = StyleRole::InlineCode};
          }
        } else if (c == '*' || c == '_') {
          skip = runEnd(s, i, c) - i;
          span = emphasisAt(s, i, skip, misses);
        } else if (c == '[' || c == '!') {
          span = linkAt(s, i, misses);
        } else if (c == '<') {
          span = autolinkAt(s, i);
        }
        if (span) {
          w.add(i, StyleRole::Plain);
          w.add(span->end, span->role, span->italic, span->bold);
          i = span->end;
        } else {
          i += skip;
        }
      }
      w.add(s.size(), StyleRole::Plain);
    }

    // Quote markers, then a heading, a rule, or a list marker and inline text.
    void blockRuns(RunWriter& w) {
      const auto line = w.line;
      std::size_t at = 0;
      // Each pass moves `at` past one '>'.
      while (md::startsQuote(line.substr(at))) {
        const auto marker = at + md::indentOf(line.substr(at));
        w.add(marker, StyleRole::Plain);
        w.add(marker + 1, StyleRole::Quote);
        at = marker + 1;
      }
      const auto rest = line.substr(at);
      if (md::atxOf(md::Line{.text = rest})) {
        w.add(line.size(), StyleRole::Heading);
        return;
      }
      if (md::isRule(rest) || md::allOf(rest, '=')) {
        w.add(line.size(), StyleRole::Punctuation);
        return;
      }
      if (const auto marker = md::markerOf(rest)) {
        const auto begin = at + marker->indent;
        const auto end = std::min(line.find(' ', begin), line.size());
        w.add(begin, StyleRole::Plain);
        w.add(end, StyleRole::Punctuation);
        at = end;
      }
      inlineRuns(w, at);
    }
  } // namespace

  MarkdownFence fenceAfter(std::string_view line, MarkdownFence fence) {
    if (fence.count > 0) {
      return closesFence(line, fence) ? MarkdownFence{} : fence;
    }
    if (const auto open = md::fenceOf(md::Line{.text = line})) {
      return {.ch = open->ch, .count = open->count};
    }
    return {};
  }

  std::vector<formats::StyledRun> markdownRuns(std::string_view line, MarkdownFence fence) {
    std::vector<formats::StyledRun> runs;
    RunWriter w{.line = line, .runs = runs};
    if (fence.count > 0 && !closesFence(line, fence)) {
      w.add(line.size(), StyleRole::InlineCode);
      return runs;
    }
    if (fence.count > 0) {
      const auto indent = md::indentOf(line);
      w.add(indent, StyleRole::Plain);
      w.add(runEnd(line, indent, fence.ch), StyleRole::Punctuation);
      w.add(line.size(), StyleRole::Plain);
      return runs;
    }
    if (const auto open = md::fenceOf(md::Line{.text = line})) {
      w.add(open->indent, StyleRole::Plain);
      w.add(open->indent + open->count, StyleRole::Punctuation);
      w.add(line.size(), StyleRole::InlineCode);
      return runs;
    }
    blockRuns(w);
    return runs;
  }
} // namespace luciole::editing
