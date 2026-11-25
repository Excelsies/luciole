#include "app/gtk_color_watch.h"

#include "core/log.h"
#include "fs/directory_watch.h"

#include <algorithm>
#include <optional>
#include <sys/stat.h>
#include <system_error>
#include <utility>

namespace {
  namespace fs = std::filesystem;
  constexpr auto kLog = Logger("palette");

  // The folder itself, or the closest ancestor that exists; each pass climbs one level, so the path's depth bounds it.
  fs::path nearestExisting(fs::path folder) {
    std::error_code error;
    while (!fs::is_directory(folder, error) && folder.has_parent_path() && folder != folder.parent_path()) {
      folder = folder.parent_path();
    }
    return folder;
  }

  // Which directory a path names right now; a replaced or repointed folder has another identity.
  std::optional<std::pair<dev_t, ino_t>> identify(const fs::path& folder) {
    struct stat info{};
    if (::stat(folder.c_str(), &info) != 0) {
      return std::nullopt;
    }
    return std::pair{info.st_dev, info.st_ino};
  }
} // namespace

luciole::GtkColorWatch::GtkColorWatch(std::function<void()> changed) : m_changed(std::move(changed)) {}

luciole::GtkColorWatch::~GtkColorWatch() = default;

bool luciole::GtkColorWatch::relevant(const fs::path& path) const {
  if (std::ranges::find(m_known, path) != m_known.end()) {
    return true;
  }
  // A folder that does not exist yet appears as one of its ancestors' children.
  const auto& name = path.native();
  return !name.empty() && std::ranges::any_of(m_known, [&name](const fs::path& known) {
    const auto& full = known.native();
    return full.size() > name.size() && full.starts_with(name) && full[name.size()] == '/';
  });
}

bool luciole::GtkColorWatch::follow(const fs::path& root, const std::vector<fs::path>& paths) {
  std::vector<fs::path> wanted;
  m_known.clear();
  if (!root.empty()) {
    m_known.push_back(root);
    std::vector<fs::path> folders{root.parent_path()};
    for (const auto& path : paths) {
      m_known.push_back(path);
      folders.push_back(path.parent_path());
      // A link is edited through its target, which may be anywhere.
      std::error_code error;
      const auto target = fs::weakly_canonical(path, error);
      if (!error && target != path) {
        m_known.push_back(target);
        // The same folder under another name (a linked parent) is watched once.
        std::error_code same;
        if (!fs::equivalent(target.parent_path(), path.parent_path(), same)) {
          folders.push_back(target.parent_path());
        }
      }
    }
    for (const auto& folder : folders) {
      auto existing = nearestExisting(folder);
      if (std::ranges::find(wanted, existing) == wanted.end()) {
        wanted.push_back(std::move(existing));
      }
    }
    const bool cut = wanted.size() > kMostFolders;
    if (cut && !m_cutLogged) {
      kLog.warn("watching {} of {} folders that hold desktop color files", kMostFolders, wanted.size());
    }
    m_cutLogged = cut;
    wanted.resize(std::min(wanted.size(), kMostFolders));
  }
  std::vector<Folder> next;
  std::vector<fs::path> failed;
  bool changed = wanted != m_wanted;
  for (const auto& path : wanted) {
    const auto identity = identify(path);
    const auto kept = std::ranges::find(m_folders, path, &Folder::path);
    if (kept != m_folders.end() && identity && *identity == kept->identity) {
      next.push_back(std::move(*kept));
      continue;
    }
    // A new folder, or one replaced since its monitor started, which that monitor no longer sees.
    changed = changed || kept != m_folders.end();
    std::unique_ptr<DirectoryWatch> monitor;
    if (identity) {
      monitor = DirectoryWatch::start(path, [this](const fs::path& where) {
        if (relevant(where)) {
          m_changed();
        }
      });
    }
    if (!monitor) {
      if (std::ranges::find(m_failed, path) == m_failed.end()) {
        kLog.warn("cannot watch {} for color changes", path.string());
      }
      failed.push_back(path);
      continue;
    }
    next.push_back({.path = path, .identity = *identity, .monitor = std::move(monitor)});
  }
  m_folders = std::move(next);
  m_failed = std::move(failed);
  m_wanted = std::move(wanted);
  return changed;
}

std::vector<fs::path> luciole::GtkColorWatch::folders() const {
  std::vector<fs::path> result;
  result.reserve(m_folders.size());
  for (const auto& folder : m_folders) {
    result.push_back(folder.path);
  }
  return result;
}
