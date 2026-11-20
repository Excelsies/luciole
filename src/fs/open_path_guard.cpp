#include "fs/open_path_guard.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace {
  std::mutex g_lock;
  std::vector<std::string> g_open; // one entry per live guard
} // namespace

luciole::OpenPathGuard::OpenPathGuard(std::string path) : m_path(std::move(path)) {
  const std::scoped_lock lock(g_lock);
  g_open.push_back(m_path);
}

luciole::OpenPathGuard::~OpenPathGuard() {
  const std::scoped_lock lock(g_lock);
  if (const auto found = std::ranges::find(g_open, m_path); found != g_open.end()) {
    g_open.erase(found);
  }
}

bool luciole::pathOpenWithin(std::string_view root) {
  while (root.size() > 1 && root.ends_with('/')) {
    root.remove_suffix(1);
  }
  const std::scoped_lock lock(g_lock);
  return std::ranges::any_of(g_open, [root](const std::string& path) {
    return path == root
        || (path.size() > root.size() && path.starts_with(root) && (root == "/" || path[root.size()] == '/'));
  });
}
