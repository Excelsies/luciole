#pragma once

#include <cstdint>
#include <string>

namespace luciole::formats {
  // Appends the UTF-8 bytes of a code point; a value past U+10FFFF is written as U+FFFD.
  inline void appendUtf8(std::string& out, std::uint32_t code) {
    constexpr std::uint32_t kLastCodePoint = 0x10FFFF;
    constexpr std::uint32_t kReplacement = 0xFFFD;
    code = code > kLastCodePoint ? kReplacement : code;
    if (code < 0x80) {
      out += static_cast<char>(code);
    } else if (code < 0x800) {
      out += static_cast<char>(0xC0 | (code >> 6));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
      out += static_cast<char>(0xE0 | (code >> 12));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (code >> 18));
      out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }
} // namespace luciole::formats
