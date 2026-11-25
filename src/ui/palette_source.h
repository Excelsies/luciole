#pragma once

#include "model/directory_options.h"
#include "ui/palette.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace luciole {
  enum class PaletteSource : std::uint8_t { ColorFile, DesktopColors, BuiltIn };

  std::string_view paletteSourceName(PaletteSource source);

  struct ChosenPalette {
    Palette palette;
    PaletteSource source;
    // True when the desktop's GTK colors were consulted, so their files matter to the caller.
    bool readsDesktopColors = false;
    // Every GTK file tried, read or not, the root first; empty unless readsDesktopColors.
    std::vector<std::filesystem::path> gtkPaths;
  };

  // Auto: colorFile when it exists, else the desktop's colors from gtkCss when usable, else the built-in palette.
  // Dark and Light: loadPalette(colorFile, scheme). An invalid colorFile is an error.
  std::expected<ChosenPalette, std::string>
  choosePalette(const std::filesystem::path& colorFile, const std::filesystem::path& gtkCss, ColorScheme scheme);
} // namespace luciole
