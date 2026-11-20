#pragma once

#include "test_check.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

namespace luciole::test {
  // A private folder (mode 0700) named <prefix>-XXXXXX, by default under the system temporary directory. It is
  // removed with everything in it when its owner is destroyed, and at exit when the process ends first: a failed
  // check ends it through std::exit, which runs exit handlers but no destructors of local fixtures. A crash or a kill
  // runs neither, so each process that makes a folder also starts one watcher in its own session, which removes the
  // folders still held once the process has ended, however it ended. Only the process that made a folder removes
  // it, so a forked child leaves its parent's folders alone.
  class TempFolder {
  public:
    // Folders that may exist at once in one process.
    static constexpr std::size_t kMostAtOnce = 64;
    static constexpr std::size_t kLongestPrefix = 64;

    // `prefix` is 1 to kLongestPrefix lowercase letters, digits and dashes, and `parent` an absolute path to an
    // existing folder; anything else fails a check.
    explicit TempFolder(std::string_view prefix, const std::filesystem::path& parent = defaultParent()) {
      TEST_CHECK(validPrefix(prefix));
      std::error_code error;
      TEST_CHECK(parent.is_absolute() && std::filesystem::is_directory(parent, error) && !error);
      auto pattern = (parent / (std::string(prefix) + "-XXXXXX")).native();
      TEST_CHECK(!pattern.contains('\n')); // the watcher reads one path per line
      TEST_CHECK(::mkdtemp(pattern.data()) != nullptr);
      m_path = pattern;
      const auto slot = Table::instance().add(m_path);
      if (!slot) {
        removeNow();
      }
      TEST_CHECK(slot || slot.error() != Refusal::NoWatcher); // the watcher could not start or has gone
      TEST_CHECK(slot.has_value());                           // more than kMostAtOnce folders at once
      m_slot = *slot;
    }

    ~TempFolder() {
      removeNow();
      TEST_CHECK(Table::instance().drop(m_slot, m_path));
    }

    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    TempFolder(TempFolder&&) = delete;
    TempFolder& operator=(TempFolder&&) = delete;

    const std::filesystem::path& path() const noexcept { return m_path; }

  private:
    enum class Refusal : std::uint8_t { TableFull, NoWatcher };

    // Reads lines from its standard input until the other end closes: "+<path>" holds a folder, "-<path>" lets it
    // go. Holds at most kMostAtOnce paths, as the table does; each "-" walks them once. Then removes what it holds.
    // Signals from the terminal or a test runner's group do not reach it.
    static constexpr const char* kWatcherScript = R"(trap '' HUP INT TERM
echo ready
exec >/dev/null
while IFS= read -r line; do
  case $line in
    +*) set -- "$@" "${line#+}" ;;
    -*) gone=${line#-}; left=$#
        while [ "$left" -gt 0 ]; do
          left=$((left - 1)); held=$1; shift
          [ "$held" = "$gone" ] || set -- "$@" "$held"
        done ;;
  esac
done
[ $# -eq 0 ] || { chmod -R u+rwx -- "$@"; rm -rf -- "$@"; }
)";

    // The folders still to remove at exit, in a fixed table whose exit handler is installed once, and this process's
    // watcher. Nothing here fails a check while holding the lock, as the exit handler takes it too.
    class Table {
    public:
      static Table& instance() {
        static Table table;
        // Installed after the table exists, so it runs before the table is destroyed.
        static const bool installed = std::atexit([] { instance().removeAll(); }) == 0;
        TEST_CHECK(installed);
        return table;
      }

      // Places left by another process, as in a forked child, are free here.
      std::expected<std::size_t, Refusal> add(const std::filesystem::path& path) {
        const std::scoped_lock lock(m_mutex);
        const pid_t self = ::getpid();
        auto* const free = std::ranges::find_if(m_entries, [self](const Entry& entry) {
          return entry.path.empty() || entry.owner != self;
        });
        if (free == m_entries.end()) {
          return std::unexpected(Refusal::TableFull);
        }
        if (!watching() || !tell('+', path)) {
          return std::unexpected(Refusal::NoWatcher);
        }
        *free = {.path = path, .owner = self};
        return static_cast<std::size_t>(free - m_entries.begin());
      }

      // Forgets a folder this process made and has removed; false when the watcher could not be told.
      bool drop(std::size_t slot, const std::filesystem::path& path) {
        const std::scoped_lock lock(m_mutex);
        auto& entry = m_entries[slot];
        if (entry.owner != ::getpid() || entry.path != path) {
          return true;
        }
        entry = {};
        return tell('-', path);
      }

    private:
      struct Entry {
        std::filesystem::path path;
        pid_t owner = 0;
      };

      void removeAll() {
        const std::scoped_lock lock(m_mutex);
        const pid_t self = ::getpid();
        for (auto& entry : m_entries) {
          if (!entry.path.empty() && entry.owner == self) {
            removeTree(entry.path);
            // The process is ending: a watcher not told only removes a folder that is already gone.
            static_cast<void>(tell('-', entry.path));
          }
          entry = {};
        }
      }

      // Starts this process's watcher unless it runs; a forked child closes the one it inherited and starts its own.
      bool watching() {
        const pid_t self = ::getpid();
        if (m_watcher >= 0 && m_watcherOwner == self) {
          return true;
        }
        if (m_watcher >= 0) {
          ::close(m_watcher);
        }
        m_watcher = startWatcher();
        m_watcherOwner = self;
        return m_watcher >= 0;
      }

      bool tell(char sign, const std::filesystem::path& path) const {
        if (m_watcher < 0 || m_watcherOwner != ::getpid()) {
          return false;
        }
        const std::string line = sign + path.native() + '\n';
        std::size_t sent = 0;
        // Each pass sends some bytes or retries after a signal; a blocked or closed socket ends it.
        for (int attempt = 0; attempt < 64 && sent < line.size(); ++attempt) {
          const ssize_t got = ::send(m_watcher, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
          if (got < 0 && errno != EINTR) {
            return false;
          }
          sent += got > 0 ? static_cast<std::size_t>(got) : 0;
        }
        return sent == line.size();
      }

      // The socket to the new watcher, which programs this process starts do not inherit, or -1. Two forks put the
      // watcher in a session of its own and make it no child of the test. It answers once its shell runs.
      static int startWatcher() {
        std::array<int, 2> ends{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends.data()) != 0) {
          return -1;
        }
        const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        const pid_t middle = null < 0 ? -1 : ::fork();
        if (middle == 0) {
          // Only calls that are safe after a fork until exec, as the test may run threads.
          if (::setsid() < 0) {
            ::_exit(1);
          }
          const pid_t watcher = ::fork();
          if (watcher != 0) {
            ::_exit(watcher > 0 ? 0 : 1);
          }
          if (::dup2(ends[1], STDIN_FILENO) < 0
              || ::dup2(ends[1], STDOUT_FILENO) < 0
              || ::dup2(null, STDERR_FILENO) < 0
              || ::close_range(3, ~0U, 0) != 0) {
            ::_exit(127);
          }
          ::execl("/bin/sh", "sh", "-c", kWatcherScript, "luciole-temp-folder", nullptr);
          ::_exit(127);
        }
        ::close(ends[1]);
        if (null >= 0) {
          ::close(null);
        }
        if (middle < 0 || !exitedCleanly(middle) || !answered(ends[0])) {
          ::close(ends[0]);
          return -1;
        }
        return ends[0];
      }

      static bool exitedCleanly(pid_t pid) {
        int status = 0;
        // Retries only when a signal interrupts the wait.
        for (int attempt = 0; attempt < 64; ++attempt) {
          if (::waitpid(pid, &status, 0) == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
          }
          if (errno != EINTR) {
            return false;
          }
        }
        return false;
      }

      // Whether "ready" arrives within ten seconds.
      static bool answered(int socket) {
        const timeval limit{.tv_sec = 10, .tv_usec = 0};
        if (::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit) != 0) {
          return false;
        }
        constexpr std::string_view kReady = "ready\n";
        std::array<char, kReady.size()> got{};
        std::size_t have = 0;
        // Each pass reads some bytes; a timeout, an error or the end of the stream stops it.
        for (int attempt = 0; attempt < 64 && have < got.size(); ++attempt) {
          const ssize_t read = ::recv(socket, got.data() + have, got.size() - have, 0);
          if (read == 0 || (read < 0 && errno != EINTR)) {
            return false;
          }
          have += read > 0 ? static_cast<std::size_t>(read) : 0;
        }
        return std::string_view(got.data(), have) == kReady;
      }

      std::mutex m_mutex;
      std::array<Entry, kMostAtOnce> m_entries{};
      int m_watcher = -1;
      pid_t m_watcherOwner = 0;
    };

    static bool validPrefix(std::string_view prefix) {
      return !prefix.empty() && prefix.size() <= kLongestPrefix && std::ranges::all_of(prefix, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
      });
    }

    static std::filesystem::path defaultParent() {
      std::error_code error;
      auto parent = std::filesystem::temp_directory_path(error);
      TEST_CHECK(!error);
      return parent;
    }

    // Gives the owner back its rights on every folder it reaches, entering each one once it may, then removes the
    // tree. The walk visits each entry once and stops at its first error, so it ends with the tree. Nothing throws,
    // as this runs in an exit handler.
    static void removeTree(const std::filesystem::path& root) {
      namespace fs = std::filesystem;
      std::error_code ignored;
      fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, ignored);
      std::error_code walk;
      fs::recursive_directory_iterator entry(root, fs::directory_options::skip_permission_denied, walk);
      for (; !walk && entry != fs::recursive_directory_iterator(); entry.increment(walk)) {
        if (entry->is_directory(ignored) && !entry->is_symlink(ignored)) {
          fs::permissions(entry->path(), fs::perms::owner_all, fs::perm_options::add, ignored);
        }
      }
      fs::remove_all(root, ignored);
    }

    void removeNow() const {
      if (m_owner == ::getpid()) {
        removeTree(m_path);
      }
    }

    std::filesystem::path m_path;
    pid_t m_owner = ::getpid();
    std::size_t m_slot = 0;
  };
} // namespace luciole::test
