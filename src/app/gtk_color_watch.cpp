#include "app/gtk_color_watch.h"

#include "core/log.h"
#include "fs/directory_watch.h"

#include <algorithm>
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
} // namespace

luciole::GtkColorWatch::GtkColorWatch(std::function<void()> changed) : m_changed(std::move(changed)) {}

luciole::GtkColorWatch::~GtkColorWatch() = default;

bool luciole::GtkColorWatch::relevant(const fs::path& path) const {
  if (std::ranges::find(m_known, path) != m_known.end()) {
    return true;
  }
  // An ancestor of the root's folder appearing means the folder may now exist.
  const auto& root = m_rootFolder.native();
  const auto& name = path.native();
  return !name.empty() && root.starts_with(name) && root.size() > name.size() && root[name.size()] == '/';
}

void luciole::GtkColorWatch::follow(const fs::path& root, const std::vector<fs::path>& files) {
  std::vector<fs::path> wanted;
  m_known.clear();
  m_rootFolder.clear();
  if (!root.empty()) {
    m_rootFolder = root.parent_path();
    m_known = files;
    m_known.push_back(root);
    m_known.push_back(m_rootFolder);
    wanted.push_back(nearestExisting(m_rootFolder));
    for (const auto& file : files) {
      wanted.push_back(file.parent_path());
    }
    std::vector<fs::path> unique;
    for (auto& folder : wanted) {
      if (unique.size() < kMostFolders && std::ranges::find(unique, folder) == unique.end()) {
        unique.push_back(std::move(folder));
      }
    }
    wanted = std::move(unique);
  }
  std::vector<Folder> next;
  for (const auto& path : wanted) {
    const auto kept = std::ranges::find(m_folders, path, &Folder::path);
    if (kept != m_folders.end()) {
      next.push_back(std::move(*kept));
      continue;
    }
    auto monitor = DirectoryWatch::start(path, [this](const fs::path& changed) {
      if (relevant(changed)) {
        m_changed();
      }
    });
    if (!monitor) {
      kLog.warn("cannot watch {} for color changes", path.string());
      continue;
    }
    next.push_back({.path = path, .monitor = std::move(monitor)});
  }
  m_folders = std::move(next);
}

std::vector<fs::path> luciole::GtkColorWatch::folders() const {
  std::vector<fs::path> result;
  result.reserve(m_folders.size());
  for (const auto& folder : m_folders) {
    result.push_back(folder.path);
  }
  return result;
}
