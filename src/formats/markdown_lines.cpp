#include "formats/markdown_lines.h"

#include "formats/ascii.h"
#include "formats/unicode_space.h"

#include <algorithm>

namespace luciole::formats::markdown {
  namespace {
    constexpr std::size_t kMaxMarkerGap = 4;
    constexpr std::size_t kMaxDigits = 9;
    constexpr std::size_t kMaxAtxLevel = 6;
    constexpr std::size_t kMinFence = 3;
    constexpr std::size_t kMinRule = 3;
    constexpr std::uint32_t kDecimalBase = 10;
  } // namespace

  std::optional<Fence> fenceOf(const Line& line) {
    const std::size_t indent = indentOf(line.text);
    const char ch = charAt(line.text, indent);
    if (indent > kMaxIndent || (ch != '`' && ch != '~')) {
      return std::nullopt;
    }
    std::size_t count = 0;
    while (charAt(line.text, indent + count) == ch) {
      ++count;
    }
    const Line info = trimLine(sliceLine(line, indent + count));
    if (count < kMinFence || (ch == '`' && info.text.contains('`'))) {
      return std::nullopt;
    }
    Line lang = info;
    lang.text = info.text.substr(0, info.text.find_first_of(" \t"));
    return Fence{.ch = ch, .count = count, .indent = indent, .lang = lang};
  }

  std::optional<Atx> atxOf(const Line& line) {
    const std::size_t indent = indentOf(line.text);
    if (indent > kMaxIndent || charAt(line.text, indent) != '#') {
      return std::nullopt;
    }
    std::size_t level = 0;
    while (charAt(line.text, indent + level) == '#') {
      ++level;
    }
    const std::size_t after = indent + level;
    if (level > kMaxAtxLevel || (after < line.text.size() && line.text[after] != ' ')) {
      return std::nullopt;
    }
    Line text = trimLine(sliceLine(line, after));
    std::size_t keep = text.text.size();
    while (keep > 0 && text.text[keep - 1] == '#') {
      --keep;
    }
    if (keep < text.text.size() && (keep == 0 || text.text[keep - 1] == ' ')) {
      text.text = text.text.substr(0, keep);
      text.text.remove_suffix(trailingUnicodeSpace(text.text));
    }
    return Atx{.level = static_cast<std::uint8_t>(level), .text = text};
  }

  bool isRule(std::string_view line) {
    if (indentOf(line) > kMaxIndent) {
      return false;
    }
    const std::string_view text = trimmed(line);
    const char mark = charAt(text, 0);
    if (mark != '-' && mark != '*' && mark != '_') {
      return false;
    }
    std::size_t count = 0;
    for (const char c : text) {
      if (c == mark) {
        ++count;
      } else if (c != ' ' && c != '\t') {
        return false;
      }
    }
    return count >= kMinRule;
  }

  bool allOf(std::string_view line, char ch) {
    const std::string_view text = trimmed(line);
    return !text.empty() && indentOf(line) <= kMaxIndent && std::ranges::all_of(text, [ch](char c) { return c == ch; });
  }

  std::optional<Marker> markerOf(std::string_view line) {
    const std::size_t indent = indentOf(line);
    const char c = charAt(line, indent);
    Marker marker{.indent = indent, .bullet = c};
    std::size_t end = indent;
    if (c == '-' || c == '*' || c == '+') {
      end = indent + 1;
    } else if (isAsciiDigit(c)) {
      while (end < line.size() && end - indent < kMaxDigits && isAsciiDigit(line[end])) {
        marker.number = (marker.number * kDecimalBase) + static_cast<std::uint32_t>(line[end] - '0');
        ++end;
      }
      if (charAt(line, end) != '.' && charAt(line, end) != ')') {
        return std::nullopt;
      }
      marker.ordered = true;
      marker.bullet = line[end];
      ++end;
    } else {
      return std::nullopt;
    }
    if (end < line.size() && line[end] != ' ') {
      return std::nullopt;
    }
    const std::size_t gap = indentOf(line.substr(end));
    marker.content = end + (gap == 0 || gap > kMaxMarkerGap || end + gap >= line.size() ? 1 : gap);
    return marker;
  }

  bool startsQuote(std::string_view line) {
    const std::size_t indent = indentOf(line);
    return indent <= kMaxIndent && charAt(line, indent) == '>';
  }

  bool interrupts(const Line& line) {
    if (fenceOf(line) || atxOf(line) || startsQuote(line.text) || isRule(line.text)) {
      return true;
    }
    const std::optional<Marker> marker = markerOf(line.text);
    return marker && !isBlank(sliceLine(line, marker->content).text) && (!marker->ordered || marker->number == 1);
  }
} // namespace luciole::formats::markdown
