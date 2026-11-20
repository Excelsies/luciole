#include "editing/source_lines.h"

#include "formats/color_codes.h"
#include "formats/line_tokens.h"
#include "formats/styled_markdown.h"
#include "formats/text_facts.h"
#include "formats/utf8.h"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <utility>

namespace luciole::editing {
  namespace {
    namespace fmt = luciole::formats;

    constexpr std::uint32_t kControlPictures = 0x2400;
    constexpr std::uint32_t kDeletePicture = 0x2421;

    bool isControl(char c) {
      const auto byte = static_cast<unsigned char>(c);
      return (byte < 0x20U && c != '\t') || byte == 0x7FU;
    }

    std::size_t codePoints(std::string_view text) {
      return static_cast<std::size_t>(std::ranges::count_if(text, [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U;
      }));
    }

    // Runs of the tokens; bytes no token covers are Plain, so the runs always hold the whole text.
    std::vector<fmt::StyledRun> runsOf(std::string_view text, const fmt::TextLine& tokens) {
      std::vector<fmt::StyledRun> runs;
      std::size_t done = 0;
      for (const auto& token : tokens) {
        const auto begin = std::max<std::size_t>(token.offset, done);
        const auto end = std::min<std::size_t>(token.offset + token.length, text.size());
        if (begin >= end) {
          continue;
        }
        fmt::styled::appendRun(runs, text.substr(done, begin - done), fmt::StyleRole::Plain, false, false);
        fmt::styled::appendRun(runs, text.substr(begin, end - begin), fmt::tokenRole(token.tokenClass), false, false);
        done = end;
      }
      fmt::styled::appendRun(runs, text.substr(done), fmt::StyleRole::Plain, false, false);
      return runs;
    }
  } // namespace

  bool sameState(const LineState& a, const LineState& b) {
    const auto sameBlock = a.scan.block.has_value() == b.scan.block.has_value()
        && (!a.scan.block
            || (a.scan.block->end == b.scan.block->end && a.scan.block->tokenClass == b.scan.block->tokenClass));
    return sameBlock
        && a.scan.inTag == b.scan.inTag
        && a.scan.oldLeft == b.scan.oldLeft
        && a.scan.newLeft == b.scan.newLeft
        && a.fence == b.fence;
  }

  SourceStyles::SourceStyles(const formats::FileKind& kind, std::size_t lineCount, LineAt lineAt, Checked /*checked*/)
      : m_kind(kind), m_syntax(formats::textSyntax(kind.language)), m_options(formats::codeOptions(kind)),
        m_lineAt(std::move(lineAt)), m_lineCount(lineCount), m_states(1) {
    // JSON is colored line by line as it stands.
    m_syntax = m_syntax == formats::TextSyntax::Json ? formats::TextSyntax::JsonLines : m_syntax;
  }

  void SourceStyles::scanTo(std::size_t index) {
    // Each pass learns the start state of one more line; index is at most lineCount().
    while (m_states.size() <= index) {
      m_states.push_back(scan(cleanLine(m_lineAt(m_states.size() - 1)), m_states.back()));
    }
  }

  std::string_view SourceStyles::cleanLine(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() && !isControl(text[first])) {
      ++first;
    }
    if (first == text.size()) {
      return text;
    }
    m_clean.assign(text.substr(0, first));
    for (const char c : text.substr(first)) {
      if (!isControl(c)) {
        m_clean += c;
      } else {
        const auto byte = static_cast<unsigned char>(c);
        formats::appendUtf8(m_clean, byte == 0x7FU ? kDeletePicture : kControlPictures + byte);
      }
    }
    return m_clean;
  }

  LineState SourceStyles::scan(std::string_view text, const LineState& start) {
    LineState state = start;
    if (m_syntax == formats::TextSyntax::Markdown) {
      state.fence = fenceAfter(text, start.fence);
      return state;
    }
    m_tokens.clear();
    formats::tokenize::tokenizeLine({.s = text, .base = 0, .out = &m_tokens}, m_syntax, m_options, state.scan);
    return state;
  }

  formats::StyledLine SourceStyles::styled(std::string_view text, const LineState& start) {
    formats::StyledLine line;
    if (m_syntax == formats::TextSyntax::Markdown) {
      line.runs = markdownRuns(text, start.fence);
    } else {
      static_cast<void>(scan(text, start));
      line.runs = runsOf(text, m_tokens);
    }
    const bool names = m_kind.language == formats::Language::Css || m_kind.language == formats::Language::Scss;
    for (const auto& match : formats::findLineColors(text, m_kind, names)) {
      line.swatches.push_back(
          {.color = match.color, .column = static_cast<std::uint32_t>(codePoints(text.substr(0, match.offset)))}
      );
    }
    return line;
  }

  std::expected<formats::StyledLine, StyleError> SourceStyles::line(std::size_t index) {
    if (index >= lineCount()) {
      return std::unexpected(StyleError::LineOutOfRange);
    }
    ++m_uses;
    const auto cached = std::ranges::find(m_cache, index, &Cached::index);
    if (cached != m_cache.end()) {
      cached->used = m_uses;
      return cached->line;
    }
    scanTo(index);
    const auto raw = m_lineAt(index);
    if (!formats::textFacts(raw).validUtf8 || raw.contains('\n')) {
      return std::unexpected(StyleError::InvalidLine);
    }
    auto line = styled(cleanLine(raw), m_states[index]);
    remember(index, line);
    return line;
  }

  void SourceStyles::remember(std::size_t index, const formats::StyledLine& line) {
    if (m_cache.size() < kCachedStyledLines) {
      m_cache.push_back({.index = index, .used = m_uses, .line = line});
      return;
    }
    auto& oldest = *std::ranges::min_element(m_cache, {}, &Cached::used);
    oldest = {.index = index, .used = m_uses, .line = line};
  }

  std::expected<LineRange, StyleError>
  SourceStyles::edited(std::size_t firstLine, std::size_t removedLines, std::size_t insertedLines) {
    if (firstLine > lineCount() || removedLines > lineCount() - firstLine) {
      return std::unexpected(StyleError::EditOutOfRange);
    }
    const auto kept = lineCount() - removedLines;
    if (insertedLines > kMaxSourceLines || kept > kMaxSourceLines - insertedLines) {
      return std::unexpected(StyleError::EditOutOfRange);
    }
    m_lineCount = kept + insertedLines;
    const auto known = m_states.size() - 1;
    LineRange changed{.first = firstLine, .count = insertedLines};
    if (firstLine + removedLines > known) {
      // No line after the edit has a known start state; each is scanned when asked for.
      m_states.resize(std::min(firstLine, known) + 1);
    } else {
      changed = rescan(firstLine, removedLines, insertedLines);
    }
    moveCache(firstLine, removedLines, insertedLines, changed);
    return changed;
  }

  LineRange SourceStyles::rescan(std::size_t firstLine, std::size_t removedLines, std::size_t insertedLines) {
    // The state the first line after the edit started in before it.
    const auto oldNext = m_states[firstLine + removedLines];
    const auto from = m_states.begin() + static_cast<std::ptrdiff_t>(firstLine) + 1;
    m_states.erase(from, from + static_cast<std::ptrdiff_t>(removedLines));
    m_states.insert(m_states.begin() + static_cast<std::ptrdiff_t>(firstLine) + 1, insertedLines, LineState{});
    // m_states now holds the old states of the following lines; each is replaced as the scan reaches it.
    const auto followers = firstLine + insertedLines;
    const auto last = m_states.size() - 1;
    auto state = m_states[firstLine];
    auto line = firstLine;
    bool same = false;
    // Each pass moves `line` forward by one; the last line with a known state ends the loop.
    for (; line <= last; ++line) {
      const auto before = line == followers ? oldNext : m_states[line];
      m_states[line] = state;
      same = line >= followers && sameState(state, before);
      if (same || line == last) {
        break;
      }
      state = scan(cleanLine(m_lineAt(line)), state);
    }
    // A changed state at `last` changes that line too, unless `last` is the end of the text.
    const auto end = same ? line : std::min(line + 1, m_lineCount);
    return {.first = firstLine, .count = end - firstLine};
  }

  void SourceStyles::moveCache(
      std::size_t firstLine, std::size_t removedLines, std::size_t insertedLines, LineRange changed
  ) {
    std::erase_if(m_cache, [&](const Cached& entry) {
      return entry.index >= firstLine && entry.index < firstLine + removedLines;
    });
    for (auto& entry : m_cache) {
      if (entry.index >= firstLine + removedLines) {
        entry.index = entry.index - removedLines + insertedLines;
      }
    }
    std::erase_if(m_cache, [&](const Cached& entry) {
      return entry.index >= changed.first && entry.index < changed.first + changed.count;
    });
  }
} // namespace luciole::editing
