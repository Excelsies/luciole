#include "imaging/animation_head.h"

#include <cstddef>

namespace {
  // An unsigned number of `count` bytes at `at`, which the caller checked lie inside `bytes`.
  std::uint32_t number(std::string_view bytes, std::size_t at, std::size_t count, bool bigEndian) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const auto byte = static_cast<std::uint8_t>(bytes[at + (bigEndian ? i : count - 1 - i)]);
      value = (value << 8U) | byte;
    }
    return value;
  }

  // GIF: the NETSCAPE2.0 extension before the first image counts repeats after the first play; without it a GIF
  // plays once. Each pass moves past at least one byte.
  std::uint32_t gifPlays(std::string_view head) {
    std::size_t at = 13;
    if (head.size() < at) {
      return 1;
    }
    const auto flags = static_cast<std::uint8_t>(head[10]);
    if ((flags & 0x80U) != 0) {
      at += 3U * (2U << (flags & 7U));
    }
    while (at + 2 <= head.size() && head[at] == '\x21') {
      at += 2;
      const auto block = head.substr(at, 16); // size, name, then a loop sub-block of at least 3 bytes
      const bool loop = block.size() == 16 && block.starts_with("\x0BNETSCAPE2.0") && block[13] == '\x01';
      if (head[at - 1] == '\xFF' && loop && static_cast<std::uint8_t>(block[12]) >= 3) {
        const auto repeats = number(head, at + 14, 2, false);
        return repeats == 0 ? 0 : repeats + 1;
      }
      while (at < head.size() && head[at] != '\0') {
        at += static_cast<std::uint8_t>(head[at]) + 1U;
      }
      ++at;
    }
    return 1;
  }

  // APNG: acTL before the first IDAT; a PNG without one there is still. Each pass moves past one whole chunk.
  std::optional<std::uint32_t> pngPlays(std::string_view head) {
    for (std::size_t at = 8; at + 8 <= head.size();) {
      const std::size_t length = number(head, at, 4, true);
      const auto type = head.substr(at + 4, 4);
      if (type == "acTL" && length >= 8 && at + 16 <= head.size()) {
        return number(head, at + 12, 4, true);
      }
      if (type == "IDAT") {
        return std::nullopt;
      }
      if (length > head.size() - at - 8) {
        break;
      }
      at += 12 + length;
    }
    return 0;
  }

  // WebP: the ANIM chunk. A simple file, or an extended one without the animation flag, is still. Each pass moves
  // past one whole chunk.
  std::optional<std::uint32_t> webpPlays(std::string_view head) {
    constexpr std::uint8_t kAnimationFlag = 0x02;
    const auto first = head.substr(12, 4);
    if (first == "VP8 " || first == "VP8L") {
      return std::nullopt;
    }
    if (first == "VP8X" && head.size() > 20 && (static_cast<std::uint8_t>(head[20]) & kAnimationFlag) == 0) {
      return std::nullopt;
    }
    for (std::size_t at = 12; at + 8 <= head.size();) {
      const std::size_t length = number(head, at + 4, 4, false);
      if (head.substr(at, 4) == "ANIM" && length >= 6 && at + 14 <= head.size()) {
        return number(head, at + 12, 2, false);
      }
      if (length > head.size() - at - 8) {
        break;
      }
      at += 8 + length + (length & 1U);
    }
    return 0;
  }
} // namespace

std::optional<std::uint32_t> luciole::animationPlaysFromHead(std::string_view head) {
  if (head.starts_with("GIF8")) {
    return gifPlays(head);
  }
  if (head.starts_with("\x89PNG\r\n\x1A\n")) {
    return pngPlays(head);
  }
  if (head.size() >= 12 && head.starts_with("RIFF") && head.substr(8, 4) == "WEBP") {
    return webpPlays(head);
  }
  return 0;
}
