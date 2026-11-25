#include "ui/palette_source.h"

#include "ui/desktop_palette.h"
#include "ui/palette_file.h"

#include <system_error>
#include <utility>

std::string_view luciole::paletteSourceName(PaletteSource source) {
  switch (source) {
  case PaletteSource::ColorFile:
    return "colors.toml";
  case PaletteSource::DesktopColors:
    return "desktop GTK colors";
  case PaletteSource::BuiltIn:
    return "built-in palette";
  }
  std::unreachable();
}

std::expected<luciole::ChosenPalette, std::string> luciole::choosePalette(
    const std::filesystem::path& colorFile, const std::filesystem::path& gtkCss, ColorScheme scheme
) {
  std::error_code error;
  const bool absent = !std::filesystem::exists(colorFile, error) && !error;
  if (scheme == ColorScheme::Auto && absent) {
    auto desktop = readGtkColors(gtkCss);
    auto palette = desktopPalette(desktop.colors);
    return ChosenPalette{
        .palette = palette ? *palette : defaultPalette(scheme),
        .source = palette ? PaletteSource::DesktopColors : PaletteSource::BuiltIn,
        .readsDesktopColors = true,
        .gtkPaths = std::move(desktop.tried),
    };
  }
  auto palette = loadPalette(colorFile, scheme);
  if (!palette) {
    return std::unexpected(std::move(palette.error()));
  }
  return ChosenPalette{
      .palette = *palette,
      .source = absent ? PaletteSource::BuiltIn : PaletteSource::ColorFile,
      .readsDesktopColors = false,
      .gtkPaths = {},
  };
}
