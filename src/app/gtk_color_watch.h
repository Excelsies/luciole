#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace luciole {
  class DirectoryWatch;

  // Watches the folders a reading of the desktop's GTK colors used, so a theming tool's rewrite of any of those
  // files calls `changed`: the folder of the root stylesheet (its nearest existing ancestor until it exists) and the
  // folder of each file read, at most kMostFolders in all.
  class GtkColorWatch {
  public:
    static constexpr std::size_t kMostFolders = 8;

    explicit GtkColorWatch(std::function<void()> changed);
    ~GtkColorWatch();
    GtkColorWatch(const GtkColorWatch&) = delete;
    GtkColorWatch& operator=(const GtkColorWatch&) = delete;
    // Watches for `root` and the files read from it, and stops watching folders no longer needed. An empty root
    // watches nothing.
    void follow(const std::filesystem::path& root, const std::vector<std::filesystem::path>& files);
    // The folders being watched.
    std::vector<std::filesystem::path> folders() const;

  private:
    struct Folder {
      std::filesystem::path path;
      std::unique_ptr<DirectoryWatch> monitor;
    };

    bool relevant(const std::filesystem::path& path) const;

    std::function<void()> m_changed;
    std::vector<Folder> m_folders;
    // The root, the files read from it and the root's folder.
    std::vector<std::filesystem::path> m_known;
    std::filesystem::path m_rootFolder;
  };
} // namespace luciole
