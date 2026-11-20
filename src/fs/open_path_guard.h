#pragma once

#include <string>
#include <string_view>

namespace luciole {
  // Says that a worker has a file or a folder open, from construction to destruction. A cancelled read or listing
  // goes on until its worker notices, so a drive's removal asks pathOpenWithin() before it lets the unmount go.
  // Any thread.
  class OpenPathGuard {
  public:
    explicit OpenPathGuard(std::string path);
    ~OpenPathGuard();
    OpenPathGuard(const OpenPathGuard&) = delete;
    OpenPathGuard& operator=(const OpenPathGuard&) = delete;
    OpenPathGuard(OpenPathGuard&&) = delete;
    OpenPathGuard& operator=(OpenPathGuard&&) = delete;

  private:
    std::string m_path;
  };

  // A guard holds `root`, or a path below it.
  bool pathOpenWithin(std::string_view root);
} // namespace luciole
