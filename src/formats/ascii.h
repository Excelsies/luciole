#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace luciole::formats {
  // ASCII byte classes; bytes of multi-byte UTF-8 sequences match none of them.
  inline bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

  inline bool isAsciiAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

  inline bool isAsciiHexDigit(char c) { return isAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

  // A space or a tab.
  inline bool isBlank(char c) { return c == ' ' || c == '\t'; }

  // An ASCII capital as its small letter; every other byte as it is.
  inline char toAsciiLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

  // Whether `text` equals `lowercase` when ASCII case is ignored; `lowercase` holds no ASCII capitals.
  inline bool equalsIgnoringCase(std::string_view text, std::string_view lowercase) {
    return text.size() == lowercase.size() && std::ranges::equal(text, lowercase, {}, toAsciiLower);
  }

  // The first index at or after `from` that is not a space or a tab; `from` itself when it is past the end.
  inline std::size_t skipBlanks(std::string_view text, std::size_t from) {
    auto at = from;
    while (at < text.size() && isBlank(text[at])) {
      ++at;
    }
    return at;
  }
} // namespace luciole::formats
