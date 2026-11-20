#pragma once

#include "formats/color_codes.h"

#include <cstddef>
#include <optional>
#include <string_view>

// CSS color notations: the named colors and the rgb(), rgba(), hsl(), hsla() and hwb() functions.
namespace luciole::formats {
  // Longest color function read, from its name to its closing parenthesis.
  inline constexpr std::size_t kMaxColorFunctionBytes = 128;

  struct ColorNotation {
    Rgba color;
    std::size_t length = 0; // bytes from the start of the text
    bool operator==(const ColorNotation&) const = default;
  };

  // The color function `text` starts with, its name in any ASCII case, closed within kMaxColorFunctionBytes.
  // Arguments are numbers, percentages or angles, separated by commas, or by spaces with an optional "/ alpha";
  // hwb() takes spaces only. Out-of-range channels clamp and hues wrap. None unless a whole valid function starts
  // the text.
  std::optional<ColorNotation> cssColorFunction(std::string_view text);

  // The CSS color named `name`, "transparent" included, ignoring ASCII case.
  std::optional<Rgba> cssColorName(std::string_view name);
} // namespace luciole::formats
