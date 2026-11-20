#include "formats/text_lines.h"

#include "formats/ascii.h"

#include <algorithm>

namespace luciole::formats::tokenize {
  namespace {
    // Splits bytes into code points. A continuation byte joins the sequence its lead byte announced; any other byte,
    // a stray continuation byte included, starts a code point. So no code point spans more than four bytes.
    struct CodePointCounter {
      std::size_t pending = 0; // continuation bytes the last lead byte still expects

      bool starts(char c) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte & 0xC0U) == 0x80U && pending > 0) {
          --pending;
          return false;
        }
        pending = byte >= 0xF0U ? 3 : byte >= 0xE0U ? 2 : byte >= 0xC0U ? 1 : 0;
        return true;
      }
    };
  } // namespace

  bool isQuote(char c) { return c == '"' || c == '\'' || c == '`'; }

  std::size_t prefixBytes(std::string_view text, std::size_t count) {
    CodePointCounter counter;
    std::size_t seen = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (counter.starts(text[i])) {
        if (seen == count) {
          return i;
        }
        ++seen;
      }
    }
    return text.size();
  }

  std::size_t codePoints(std::string_view text, std::size_t limit) {
    CodePointCounter counter;
    std::size_t count = 0;
    for (const char c : text) {
      if (counter.starts(c) && ++count > limit) {
        break;
      }
    }
    return count;
  }

  std::size_t stringEnd(std::string_view s, std::size_t i) {
    if (i >= s.size()) {
      return s.size();
    }
    const char quote = s[i];
    for (std::size_t j = i + 1; j < s.size(); ++j) {
      if (s[j] == '\\') {
        ++j;
      } else if (s[j] == quote) {
        return j + 1;
      }
    }
    return s.size();
  }

  std::size_t inBlock(const LineWriter& w, std::size_t i, ScanState& state) {
    if (!state.block) {
      return i;
    }
    const auto end = w.s.find(state.block->end, i);
    const auto stop = end == std::string_view::npos ? w.s.size() : end + state.block->end.size();
    w.add(i, stop, state.block->tokenClass);
    if (end != std::string_view::npos) {
      state.block.reset();
    }
    return stop;
  }

  bool isNumber(std::string_view w) {
    std::size_t i = w.starts_with('-') || w.starts_with('+') ? 1 : 0;
    if (w.size() > i + 1 && w[i] == '0' && (w[i + 1] == 'x' || w[i + 1] == 'X')) {
      const auto digits = w.substr(i + 2);
      return !digits.empty() && std::ranges::all_of(digits, [](char c) { return isAsciiHexDigit(c) || c == '_'; });
    }
    if (i >= w.size() || !isAsciiDigit(w[i])) {
      return false;
    }
    while (i < w.size() && (isAsciiDigit(w[i]) || w[i] == '_')) {
      ++i;
    }
    const auto digitsFrom = [&](std::size_t from) {
      i = from;
      while (i < w.size() && isAsciiDigit(w[i])) {
        ++i;
      }
      return i > from;
    };
    if (i < w.size() && w[i] == '.' && !digitsFrom(i + 1)) {
      return false;
    }
    if (i < w.size() && (w[i] == 'e' || w[i] == 'E')) {
      const bool sign = i + 1 < w.size() && (w[i + 1] == '-' || w[i + 1] == '+');
      if (!digitsFrom(i + (sign ? 2 : 1))) {
        return false;
      }
    }
    return i == w.size();
  }

  std::size_t skipJsonSpace(std::string_view s, std::size_t i) {
    while (i < s.size() && kJsonSpace.contains(s[i])) {
      ++i;
    }
    return i;
  }

  SplitLines splitLines(std::string_view text) {
    SplitLines out;
    std::size_t pos = 0;
    const auto keep = [&](std::size_t end) {
      if (out.lines.size() < kMaxTextLines) {
        out.lines.push_back({.begin = pos, .end = pos + prefixBytes(text.substr(pos, end - pos), kMaxLineWidth + 1)});
      }
      ++out.total;
    };
    // Each pass moves pos past one line break.
    while (pos < text.size()) {
      const auto lineBreak = text.find_first_of("\r\n", pos);
      if (lineBreak == std::string_view::npos) {
        break;
      }
      keep(lineBreak);
      const bool crlf = text[lineBreak] == '\r' && lineBreak + 1 < text.size() && text[lineBreak + 1] == '\n';
      pos = lineBreak + (crlf ? 2 : 1);
    }
    if (pos < text.size() || out.total == 0) {
      keep(text.size());
    }
    return out;
  }

  bool cutWidth(std::string_view text, TextLine& line) {
    std::size_t room = kMaxLineWidth;
    for (std::size_t k = 0; k < line.size(); ++k) {
      auto& token = line[k];
      const bool space = token.tokenClass == TokenClass::Space;
      const bool inside = !space && token.offset <= text.size();
      const auto bytes = inside ? text.substr(token.offset, token.length) : std::string_view{};
      const auto width = space ? std::size_t{token.length} : codePoints(bytes, room);
      if (width <= room) {
        room -= width;
        continue;
      }
      auto cut = token.offset;
      if (room > 0) {
        token.length = static_cast<std::uint32_t>(space ? room : prefixBytes(bytes, room));
        cut += space ? 0 : token.length;
      }
      line.resize(room > 0 ? k + 1 : k);
      line.push_back({.offset = cut, .length = 0, .tokenClass = TokenClass::More});
      return true;
    }
    return false;
  }
} // namespace luciole::formats::tokenize
