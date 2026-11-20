#pragma once

#include "formats/file_kind.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace luciole::formats {
  inline constexpr std::size_t kMaxColorsPerLine = 64;
  inline constexpr std::size_t kMaxColorsPerFile = 4000;

  // Straight (not premultiplied) 8-bit sRGB and alpha.
  struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
    bool operator==(const Rgba&) const = default;
  };

  struct ColorMatch {
    std::size_t offset = 0; // bytes into the scanned line or text
    std::size_t length = 0;
    Rgba color;
    bool operator==(const ColorMatch&) const = default;
  };

  // Languages whose lines are scanned whole: CSS, SCSS, JSON, JSON Lines, TOML, YAML, INI, desktop entries, XML
  // and HTML. Code is scanned only inside string literals on the line, before its line comment, plus 0x literals.
  // Other languages have no colors.
  bool colorLanguage(Language language);

  // Color codes in one line of a file of this kind, at most kMaxColorsPerLine:
  // - #rgb, #rgba, #rrggbb and #rrggbbaa;
  // - rgb(), rgba(), hsl() and hsla() with commas or spaces and an optional "/ alpha", hwb() with spaces; numbers,
  //   percentages and deg, rad, grad or turn hues; out-of-range channels clamp and hues wrap;
  // - 0xRRGGBB and 0xAARRGGBB in code, with an optional u or l integer suffix outside the match;
  // - CSS color names and "transparent", ignoring ASCII case, only when allowNames is set.
  // A '#' code is refused when a word character, '#', '&', '/' or '\' precedes it, when a word character or '-'
  // follows it, inside a URL, url() or href and src values, at the start of a line outside CSS, SCSS and HTML, before
  // a selector character ("{.[>+~:") in those three, and, outside color languages, when it is all digits and not the
  // whole string.
  std::vector<ColorMatch> findLineColors(std::string_view line, const FileKind& kind, bool allowNames);

  // Line by line at LF, CRLF and CR; offsets are into text; at most kMaxColorsPerFile.
  std::vector<ColorMatch> findColors(std::string_view text, const FileKind& kind, bool allowNames);
} // namespace luciole::formats
