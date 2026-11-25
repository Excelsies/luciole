#pragma once

#include "render/core/color.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  // Bounds on what one reading follows and keeps.
  inline constexpr int kGtkMaxImportDepth = 4;
  inline constexpr std::size_t kGtkMaxFiles = 16;
  inline constexpr std::size_t kGtkMaxFileBytes = 1U << 20U;
  inline constexpr int kGtkMaxReferenceDepth = 8;

  // Color names as `@define-color` writes them: `--accent-bg-color` is `accent_bg_color`.
  using GtkColorTable = std::map<std::string, Color, std::less<>>;

  struct GtkColors {
    GtkColorTable colors;
    // The files whose text was read, the root first.
    std::vector<std::filesystem::path> files;
  };

  // The text of a regular file of at most kGtkMaxFileBytes; nullopt for anything else.
  std::optional<std::string> readGtkFile(const std::filesystem::path& path);

  using GtkFileReader = std::function<std::optional<std::string>(const std::filesystem::path&)>;

  // Reads the colors a GTK 4 stylesheet defines: `@define-color NAME VALUE;` and `--name: VALUE;`, in document
  // order with later definitions winning, following `@import` of local files. A value is `#rgb`, `#rrggbb`,
  // `#rrggbbaa`, `@name` or `var(--name)`; a name with any other value is left out. Unknown constructs are
  // skipped, never reported. Reading a path that `read` refuses yields no colors for it.
  GtkColors readGtkColors(const std::filesystem::path& root, const GtkFileReader& read);

  // $XDG_CONFIG_HOME/gtk-4.0/gtk.css
  std::filesystem::path gtkCssPath();
} // namespace luciole
