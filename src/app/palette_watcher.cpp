#include "app/palette_watcher.h"

#include "core/log.h"
#include "fs/directory_watch.h"
#include "ui/gtk_colors.h"
#include "ui/palette_file.h"

#include <glib.h>

namespace {
  constexpr auto kLog = Logger("palette");
}

luciole::PaletteWatcher::PaletteWatcher(ColorScheme scheme)
    : m_path(palettePath()), m_gtkCss(gtkCssPath()), m_scheme(scheme), m_gtk([this] { scheduleReload(); }) {
  watch();
  reload();
}

luciole::PaletteWatcher::~PaletteWatcher() {
  if (m_debounce) {
    g_source_destroy(m_debounce);
    g_source_unref(m_debounce);
  }
}

void luciole::PaletteWatcher::setScheme(ColorScheme scheme) {
  if (scheme == m_scheme) {
    return;
  }
  m_scheme = scheme;
  reload();
}

void luciole::PaletteWatcher::reload() {
  // A write that lands between a read and the watches it started is not seen by them: when the watched folders
  // changed, read once more. The second read starts no further pass, so this is at most two reads.
  if (apply()) {
    (void)apply();
  }
}

bool luciole::PaletteWatcher::apply() {
  const auto chosen = choosePalette(m_path, m_gtkCss, m_scheme);
  if (!chosen) {
    (void)m_gtk.follow({}, {});
    kLog.warn("{}", chosen.error());
    return false;
  }
  const bool watching = m_gtk.follow(chosen->readsDesktopColors ? m_gtkCss : std::filesystem::path{}, chosen->gtkPaths);
  if (m_source != chosen->source) {
    m_source = chosen->source;
    kLog.info("colors from {}", paletteSourceName(chosen->source));
  }
  setPalette(chosen->palette);
  return watching;
}

void luciole::PaletteWatcher::scheduleReload() {
  // Template writers may truncate, write and rename in quick succession; apply the settled file once.
  if (m_debounce) {
    g_source_destroy(m_debounce);
    g_source_unref(m_debounce);
  }
  m_debounce = g_timeout_source_new(100);
  g_source_set_callback(
      m_debounce,
      [](gpointer data) -> gboolean {
        auto* self = static_cast<PaletteWatcher*>(data);
        g_source_unref(std::exchange(self->m_debounce, nullptr));
        self->watch();
        self->reload();
        return G_SOURCE_REMOVE;
      },
      this, nullptr
  );
  g_source_attach(m_debounce, g_main_context_default());
}

void luciole::PaletteWatcher::watch() {
  const auto folder = m_path.parent_path();
  std::error_code error;
  const auto target = std::filesystem::is_directory(folder, error) ? folder : folder.parent_path();
  if (target == m_watched && m_monitor) {
    return;
  }
  m_watched = target;
  m_monitor = DirectoryWatch::start(target, [this, folder](const std::filesystem::path& path) {
    if (path == m_path || path == folder) {
      scheduleReload();
    }
  });
}
