#pragma once

#include <cstddef>
#include <string_view>

// Unicode white space in UTF-8 text: U+0009 to U+000D, U+0020, U+00A0, U+1680, U+2000 to U+200A, U+2028, U+2029,
// U+202F, U+205F, U+3000 and the byte order mark U+FEFF.
namespace luciole::formats {
  // Bytes of the white space character starting at `pos`, or 0.
  inline std::size_t unicodeSpaceWidth(std::string_view text, std::size_t pos) {
    const auto byte = [&](std::size_t k) {
      return pos + k < text.size() ? static_cast<unsigned char>(text[pos + k]) : 0U;
    };
    const unsigned first = byte(0);
    const unsigned second = byte(1);
    const unsigned third = byte(2);
    if (first == 0x20U || (first >= 0x09U && first <= 0x0DU)) {
      return 1;
    }
    if (first == 0xC2U && second == 0xA0U) {
      return 2;
    }
    const bool generalSpace = first == 0xE2U
        && second == 0x80U
        && ((third >= 0x80U && third <= 0x8AU) || third == 0xA8U || third == 0xA9U || third == 0xAFU);
    const bool otherSpace = (first == 0xE1U && second == 0x9AU && third == 0x80U)
        || (first == 0xE2U && second == 0x81U && third == 0x9FU)
        || (first == 0xE3U && second == 0x80U && third == 0x80U)
        || (first == 0xEFU && second == 0xBBU && third == 0xBFU);
    return generalSpace || otherSpace ? 3 : 0;
  }

  // Bytes of white space at the start of text.
  inline std::size_t leadingUnicodeSpace(std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size()) { // each step moves forward by 1 to 3 bytes
      const std::size_t width = unicodeSpaceWidth(text, pos);
      if (width == 0) {
        break;
      }
      pos += width;
    }
    return pos;
  }

  // Bytes of white space at the end of text.
  inline std::size_t trailingUnicodeSpace(std::string_view text) {
    std::size_t end = text.size();
    while (end > 0) { // each step moves back by 1 to 3 bytes
      const std::string_view head = text.substr(0, end);
      std::size_t width = 0;
      for (std::size_t candidate = 1; candidate <= 3 && candidate <= end; ++candidate) {
        if (unicodeSpaceWidth(head, end - candidate) == candidate) {
          width = candidate;
          break;
        }
      }
      if (width == 0) {
        break;
      }
      end -= width;
    }
    return text.size() - end;
  }
} // namespace luciole::formats
