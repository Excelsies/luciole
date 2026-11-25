#pragma once

#include "app/gtk_color_watch.h"
#include "model/directory_options.h"
#include "ui/palette_source.h"

#include <filesystem>
#include <glib.h>
#include <memory>
#include <optional>

namespace luciole {
  class DirectoryWatch;

  // Applies the color scheme's table of $XDG_CONFIG_HOME/luciole/colors.toml and re-applies it whenever it is
  // written, so a Noctalia template keeps the window in sync with the desktop theme. Without the file, Auto follows
  // the desktop's GTK 4 colors when they are usable and the built-in palette otherwise; Dark and Light use the
  // built-in palette. An invalid file is logged and leaves the current palette in place.
  class PaletteWatcher {
  public:
    explicit PaletteWatcher(ColorScheme scheme);
    ~PaletteWatcher();
    PaletteWatcher(const PaletteWatcher&) = delete;
    PaletteWatcher& operator=(const PaletteWatcher&) = delete;
    // Re-applies colors.toml when the scheme changes.
    void setScheme(ColorScheme scheme);

  private:
    void reload();
    void scheduleReload();
    // Watches the config folder, or its parent until the folder exists.
    void watch();

    std::filesystem::path m_path;
    std::filesystem::path m_gtkCss;
    ColorScheme m_scheme;
    GtkColorWatch m_gtk;
    std::optional<PaletteSource> m_source;
    std::filesystem::path m_watched;
    std::unique_ptr<DirectoryWatch> m_monitor;
    GSource* m_debounce = nullptr;
  };
} // namespace luciole
