#include "editing/text_motion.h"

#include "formats/ascii.h"
#include "formats/unicode_space.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace {
  using luciole::editing::TextBuffer;
  using luciole::editing::TextPosition;
  using luciole::editing::TextSelection;

  struct CodePoint {
    char32_t value = 0;
    std::size_t length = 1;
  };

  struct Range {
    char32_t first;
    char32_t last;
  };

  constexpr char32_t kZeroWidthJoiner = 0x200D;
  constexpr Range kRegionalIndicators{0x1F1E6, 0x1F1FF};

  constexpr std::array kExtenders{
      Range{0x0300, 0x036F}, Range{0x1AB0, 0x1AFF}, Range{0x1DC0, 0x1DFF},   Range{0x20D0, 0x20FF},
      Range{0xFE20, 0xFE2F}, Range{0xFE00, 0xFE0F}, Range{0xE0100, 0xE01EF}, Range{0x1F3FB, 0x1F3FF},
  };

  constexpr std::array kPunctuation{
      Range{0x00A1, 0x00A9}, Range{0x00AB, 0x00B4}, Range{0x00B6, 0x00B9}, Range{0x00BB, 0x00BF}, Range{0x00D7, 0x00D7},
      Range{0x00F7, 0x00F7}, Range{0x2010, 0x2027}, Range{0x2030, 0x205E}, Range{0x3001, 0x3003}, Range{0x3008, 0x3011},
      Range{0xFF01, 0xFF0F}, Range{0xFF1A, 0xFF20}, Range{0xFF3B, 0xFF40}, Range{0xFF5B, 0xFF65},
  };

  bool inRange(char32_t value, Range range) { return value >= range.first && value <= range.last; }

  template <std::size_t N> bool inAny(char32_t value, const std::array<Range, N>& ranges) {
    return std::ranges::any_of(ranges, [value](Range range) { return inRange(value, range); });
  }

  // The code point at `at` in valid UTF-8; reads never pass the end of `text`.
  CodePoint decodeAt(std::string_view text, std::size_t at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80U) {
      return {lead, 1};
    }
    const std::size_t declared = lead >= 0xF0U ? 4 : (lead >= 0xE0U ? 3 : 2);
    const auto length = std::min(declared, text.size() - at);
    char32_t value = lead & (0xFFU >> (declared + 1));
    for (std::size_t k = 1; k < length; ++k) {
      value = (value << 6U) | (static_cast<unsigned char>(text[at + k]) & 0x3FU);
    }
    return {value, length};
  }

  enum class CharClass : std::uint8_t { Space, Word, Punctuation };

  CharClass classAt(std::string_view text, std::size_t at) {
    if (luciole::formats::unicodeSpaceWidth(text, at) > 0) {
      return CharClass::Space;
    }
    const char byte = text[at];
    if (static_cast<unsigned char>(byte) < 0x80U) {
      const bool word = luciole::formats::isAsciiAlpha(byte) || luciole::formats::isAsciiDigit(byte) || byte == '_';
      return word ? CharClass::Word : CharClass::Punctuation;
    }
    return inAny(decodeAt(text, at).value, kPunctuation) ? CharClass::Punctuation : CharClass::Word;
  }

  // Every cluster boundary of a line, 0 and the line size included.
  std::vector<std::size_t> clusterBounds(std::string_view line) {
    std::vector<std::size_t> bounds{0};
    // Each cluster is at least one byte long.
    while (bounds.back() < line.size()) {
      bounds.push_back(luciole::editing::clusterEnd(line, bounds.back()));
    }
    return bounds;
  }

  // The index of the last boundary at or before `byte`.
  std::size_t boundAtOrBefore(const std::vector<std::size_t>& bounds, std::size_t byte) {
    const auto after = std::ranges::upper_bound(bounds, byte);
    return static_cast<std::size_t>(after - bounds.begin()) - 1;
  }
} // namespace

std::size_t luciole::editing::clusterEnd(std::string_view text, std::size_t byte) {
  if (byte >= text.size()) {
    return byte;
  }
  const auto base = decodeAt(text, byte);
  auto end = byte + base.length;
  if (inRange(base.value, kRegionalIndicators) && end < text.size()) {
    const auto pair = decodeAt(text, end);
    end += inRange(pair.value, kRegionalIndicators) ? pair.length : 0;
  }
  // Each pass consumes at least one byte.
  while (end < text.size()) {
    const auto next = decodeAt(text, end);
    if (inAny(next.value, kExtenders)) {
      end += next.length;
    } else if (next.value == kZeroWidthJoiner) {
      end += next.length;
      end += end < text.size() ? decodeAt(text, end).length : 0;
    } else {
      break;
    }
  }
  return end;
}

std::size_t luciole::editing::clusterStart(std::string_view text, std::size_t byte) {
  const auto bounds = clusterBounds(text.substr(0, std::min(byte, text.size())));
  return bounds.size() > 1 ? bounds[bounds.size() - 2] : 0;
}

luciole::editing::TextPosition luciole::editing::clusterLeft(const TextBuffer& buffer, TextPosition position) {
  const auto at = buffer.clamp(position);
  if (at.byte == 0) {
    return at.line == 0 ? at : TextPosition{at.line - 1, buffer.line(at.line - 1).size()};
  }
  const auto bounds = clusterBounds(buffer.line(at.line));
  const auto before = std::ranges::lower_bound(bounds, at.byte) - 1;
  return {at.line, *before};
}

luciole::editing::TextPosition luciole::editing::clusterRight(const TextBuffer& buffer, TextPosition position) {
  const auto at = buffer.clamp(position);
  const auto line = buffer.line(at.line);
  if (at.byte >= line.size()) {
    return at.line + 1 < buffer.lineCount() ? TextPosition{at.line + 1, 0} : at;
  }
  const auto bounds = clusterBounds(line);
  return {at.line, bounds[boundAtOrBefore(bounds, at.byte) + 1]};
}

luciole::editing::TextPosition luciole::editing::wordLeft(const TextBuffer& buffer, TextPosition position) {
  const auto at = buffer.clamp(position);
  if (at.byte == 0) {
    return clusterLeft(buffer, at);
  }
  const auto line = buffer.line(at.line);
  const auto bounds = clusterBounds(line);
  auto index = static_cast<std::size_t>(std::ranges::lower_bound(bounds, at.byte) - bounds.begin());
  while (index > 0 && classAt(line, bounds[index - 1]) == CharClass::Space) {
    --index;
  }
  if (index > 0) {
    const auto run = classAt(line, bounds[index - 1]);
    while (index > 0 && classAt(line, bounds[index - 1]) == run) {
      --index;
    }
  }
  return {at.line, bounds[index]};
}

luciole::editing::TextPosition luciole::editing::wordRight(const TextBuffer& buffer, TextPosition position) {
  const auto at = buffer.clamp(position);
  const auto line = buffer.line(at.line);
  if (at.byte >= line.size()) {
    return clusterRight(buffer, at);
  }
  const auto bounds = clusterBounds(line);
  const auto last = bounds.size() - 1;
  auto index = boundAtOrBefore(bounds, at.byte);
  while (index < last && classAt(line, bounds[index]) == CharClass::Space) {
    ++index;
  }
  if (index < last) {
    const auto run = classAt(line, bounds[index]);
    while (index < last && classAt(line, bounds[index]) == run) {
      ++index;
    }
  }
  return {at.line, bounds[index]};
}

luciole::editing::TextSelection luciole::editing::wordRange(const TextBuffer& buffer, TextPosition position) {
  const auto at = buffer.clamp(position);
  const auto line = buffer.line(at.line);
  if (line.empty()) {
    return {at, at};
  }
  const auto bounds = clusterBounds(line);
  const auto last = bounds.size() - 1;
  const auto index = std::min(boundAtOrBefore(bounds, at.byte), last - 1);
  const auto run = classAt(line, bounds[index]);
  auto low = index;
  while (low > 0 && classAt(line, bounds[low - 1]) == run) {
    --low;
  }
  auto high = index + 1;
  while (high < last && classAt(line, bounds[high]) == run) {
    ++high;
  }
  return {{at.line, bounds[low]}, {at.line, bounds[high]}};
}

luciole::editing::TextPosition luciole::editing::lineStart(const TextBuffer& buffer, TextPosition position) {
  const auto at = buffer.clamp(position);
  const auto firstText = formats::skipBlanks(buffer.line(at.line), 0);
  return {at.line, at.byte == firstText ? 0 : firstText};
}

luciole::editing::TextPosition luciole::editing::lineEnd(const TextBuffer& buffer, TextPosition position) {
  const auto at = buffer.clamp(position);
  return {at.line, buffer.line(at.line).size()};
}

luciole::editing::TextPosition luciole::editing::documentStart(const TextBuffer&) { return {}; }

luciole::editing::TextPosition luciole::editing::documentEnd(const TextBuffer& buffer) {
  const auto last = buffer.lineCount() - 1;
  return {last, buffer.line(last).size()};
}

luciole::editing::TextSelection luciole::editing::lineRange(const TextBuffer& buffer, std::size_t line) {
  const auto index = std::min(line, buffer.lineCount() - 1);
  if (index + 1 < buffer.lineCount()) {
    return {{index, 0}, {index + 1, 0}};
  }
  return {{index, 0}, {index, buffer.line(index).size()}};
}

luciole::editing::TextSelection luciole::editing::selectAll(const TextBuffer& buffer) {
  return {documentStart(buffer), documentEnd(buffer)};
}
