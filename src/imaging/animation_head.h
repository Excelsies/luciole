#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace luciole {
  // Times an animated image asks to play, read from its first bytes by bounded walks over its blocks; 0 forever.
  // None when those bytes show a still PNG (no acTL before the first IDAT) or a still WebP (a simple file, or an
  // extended one without the animation flag). A GIF without a loop extension plays once; other content loops forever.
  std::optional<std::uint32_t> animationPlaysFromHead(std::string_view head);
} // namespace luciole
