#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace luciole {
  class DirectoryWatch;

  // Watches the folders a reading of the desktop's GTK colors used, so a theming tool's rewrite of any of those
  // files calls `changed`: the folder of the root stylesheet (its nearest existing ancestor until it exists) and the
  // folder of each file tried, read or not (the closest existing ancestor of a folder that is missing), and the
  // folder of the target of any link among them (once when it is the same directory), at most kMostFolders in all.
  // A folder that is replaced by another at the same path is watched again from the next `follow`.
  class GtkColorWatch {
  public:
    static constexpr std::size_t kMostFolders = 8;

    explicit GtkColorWatch(std::function<void()> changed);
    ~GtkColorWatch();
    GtkColorWatch(const GtkColorWatch&) = delete;
    GtkColorWatch& operator=(const GtkColorWatch&) = delete;
    // Watches for `root` and the files tried from it, and stops watching folders no longer needed. An empty root
    // watches nothing. True when the folders wanted differ from last time or a monitor was replaced.
    bool follow(const std::filesystem::path& root, const std::vector<std::filesystem::path>& paths);
    // The folders being watched.
    std::vector<std::filesystem::path> folders() const;

  private:
    struct Folder {
      std::filesystem::path path;
      std::pair<dev_t, ino_t> identity;
      std::unique_ptr<DirectoryWatch> monitor;
    };

    bool relevant(const std::filesystem::path& path) const;

    std::function<void()> m_changed;
    std::vector<Folder> m_folders;
    // The folders wanted last time, and those among them that could not be watched.
    std::vector<std::filesystem::path> m_wanted;
    std::vector<std::filesystem::path> m_failed;
    // The root, the files tried from it and the targets of links among them.
    std::vector<std::filesystem::path> m_known;
    bool m_cutLogged = false;
  };
} // namespace luciole
