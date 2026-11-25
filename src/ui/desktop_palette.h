#pragma once

#include "ui/gtk_colors.h"
#include "ui/palette.h"

#include <optional>

namespace luciole {
  // A complete palette from GTK color names. nullopt unless window_bg_color, window_fg_color and accent_bg_color
  // are defined. Colors the table names are used as given; the other roles are derived from them.
  std::optional<Palette> desktopPalette(const GtkColorTable& names);
} // namespace luciole
