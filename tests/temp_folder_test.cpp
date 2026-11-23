#include "temp_folder.h"
#include "test_check.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <print>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// A temporary folder goes with its owner, and with the process however it ends first.
namespace {
  namespace fs = std::filesystem;
  using luciole::test::TempFolder;

  struct Child {
    int exitCode = -1;  // -1 when the child did not exit normally
    int signal = 0;     // the signal that ended it, if one did
    std::string output; // what the child wrote to its standard output and error
  };

  // A forked child running `body`, its standard output and error coming back on `output`.
  struct Started {
    pid_t pid = -1;
    int output = -1;
  };

  // Starts `body` in a forked child in a process group of its own; the child exits 0 if `body` returns.
  Started startChild(const std::function<void()>& body) {
    std::array<int, 2> pipe{};
    TEST_CHECK(::pipe(pipe.data()) == 0);
    std::fflush(nullptr); // the child must not write what this process has buffered
    const pid_t pid = ::fork();
    TEST_CHECK(pid >= 0);
    if (pid == 0) {
      ::setpgid(0, 0);
      ::close(pipe[0]);
      ::dup2(pipe[1], STDOUT_FILENO);
      ::dup2(pipe[1], STDERR_FILENO);
      ::close(pipe[1]);
      body();
      std::exit(EXIT_SUCCESS);
    }
    ::close(pipe[1]);
    return {.pid = pid, .output = pipe[0]};
  }

  // Reads what the child writes, up to `stop` (a line end when it is '\n') or the end of its output, at most 64 KiB.
  std::string readFrom(const Started& started, char stop = '\0') {
    constexpr std::size_t kMostOutput = 64 * 1024;
    std::string output;
    std::array<char, 1> byte{};
    while (output.size() < kMostOutput && (stop == '\0' || !output.ends_with(stop))) {
      const ssize_t got = ::read(started.output, byte.data(), byte.size());
      if (got < 0 && errno == EINTR) {
        continue;
      }
      if (got <= 0) {
        break;
      }
      output += byte[0];
    }
    return output;
  }

  // Waits for the child to end; `signal` is set when a signal ended it.
  Child finish(const Started& started, std::string output) {
    output += readFrom(started);
    ::close(started.output);
    int status = 0;
    TEST_CHECK(::waitpid(started.pid, &status, 0) == started.pid);
    return {
        .exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1,
        .signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0,
        .output = std::move(output),
    };
  }

  // Runs `body` in a forked child to its end.
  Child runChild(const std::function<void()>& body) { return finish(startChild(body), {}); }

  // The lines of `text`, without their line ends.
  std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
      const auto end = text.find('\n', start);
      out.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
      start = end == std::string::npos ? text.size() : end + 1;
    }
    return out;
  }

  // Folders a test took its own rights from: one it cannot read or enter and one it cannot write to.
  void lock(const fs::path& folder) {
    fs::create_directories(folder / "unreadable" / "inside");
    std::ofstream(folder / "unreadable" / "inside" / "file.txt") << "locked away";
    fs::create_directories(folder / "readonly");
    std::ofstream(folder / "readonly" / "file.txt") << "kept";
    fs::permissions(folder / "unreadable" / "inside", fs::perms::none);
    fs::permissions(folder / "unreadable", fs::perms::none);
    fs::permissions(folder / "readonly", fs::perms::owner_read | fs::perms::owner_exec);
  }

  // A private folder under the temporary directory named after the prefix, removed with its owner.
  void aFolderGoesWithItsOwner() {
    fs::path path;
    {
      const TempFolder folder("luciole-temp-folder-test");
      path = folder.path();
      TEST_CHECK(path.parent_path() == fs::temp_directory_path());
      TEST_CHECK(path.filename().string().starts_with("luciole-temp-folder-test-"));
      TEST_CHECK(path.filename().string().size() == std::string("luciole-temp-folder-test-").size() + 6);
      struct stat info{};
      TEST_CHECK(::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode));
      TEST_CHECK((info.st_mode & 0777) == 0700);
      fs::create_directories(path / "a" / "b");
      std::ofstream(path / "a" / "b" / "c.txt") << "c";
      lock(path);
    }
    TEST_CHECK(!fs::exists(path));
  }

  // A folder made in a given parent folder; a relative, missing or unreachable parent fails a check.
  void aFolderInAGivenParent() {
    const TempFolder outer("luciole-temp-folder-test");
    fs::path path;
    {
      const TempFolder inner("luciole-temp-folder-inner", outer.path());
      path = inner.path();
      TEST_CHECK(path.parent_path() == outer.path());
      TEST_CHECK(fs::is_directory(path));
    }
    TEST_CHECK(!fs::exists(path) && fs::is_directory(outer.path()));
    lock(outer.path());
    const auto unreachable = outer.path() / "unreadable" / "inside";
    std::vector<fs::path> refused{fs::path("relative"), outer.path() / "missing"};
    if (::geteuid() != 0) { // permission bits do not stop the superuser
      refused.push_back(unreachable);
    } else {
      std::println("skipped unreachable parent: running as root");
    }
    for (const fs::path& parent : refused) {
      const auto child = runChild([&] {
        const TempFolder folder("luciole-temp-folder-test", parent);
        std::println("made {}", folder.path().native());
      });
      TEST_CHECK(child.exitCode == EXIT_FAILURE && !child.output.contains("made"));
    }
  }

  // Folders made one after another reuse their places in the exit table, far past its size.
  void manyFoldersOneAfterAnother() {
    for (std::size_t i = 0; i < 4 * TempFolder::kMostAtOnce; ++i) {
      const TempFolder folder("luciole-temp-folder-test");
      TEST_CHECK(fs::is_directory(folder.path()));
    }
    std::vector<std::unique_ptr<TempFolder>> held;
    held.reserve(TempFolder::kMostAtOnce);
    for (std::size_t i = 0; i < TempFolder::kMostAtOnce; ++i) {
      held.push_back(std::make_unique<TempFolder>("luciole-temp-folder-test"));
    }
    std::vector<fs::path> paths;
    paths.reserve(held.size());
    for (const auto& folder : held) {
      paths.push_back(folder->path());
    }
    held.clear();
    for (const auto& path : paths) {
      TEST_CHECK(!fs::exists(path));
    }
  }

  // A child that fails a check while its fixture holds a folder exits through the check, and the folder is gone.
  void aFailedCheckRemovesTheFolder() {
    const auto child = runChild([] {
      const TempFolder folder("luciole-temp-folder-test");
      fs::create_directories(folder.path() / "inner");
      std::ofstream(folder.path() / "inner" / "file.txt") << "kept until exit";
      lock(folder.path());
      std::println("{}", folder.path().native());
      std::fflush(stdout);
      TEST_CHECK(1 + 1 == 3);
    });
    TEST_CHECK(child.exitCode == EXIT_FAILURE);
    const auto said = lines(child.output);
    TEST_CHECK(said.size() == 2);
    TEST_CHECK(said[1].ends_with("check failed: 1 + 1 == 3"));
    const fs::path path = said[0];
    TEST_CHECK(path.parent_path() == fs::temp_directory_path());
    TEST_CHECK(path.filename().string().starts_with("luciole-temp-folder-test-"));
    TEST_CHECK(!fs::exists(path));
  }

  // A forked child that exits leaves the parent's folder alone.
  void aChildLeavesTheParentsFolder() {
    const TempFolder folder("luciole-temp-folder-test");
    const auto child = runChild([] { TEST_CHECK(1 + 1 == 3); });
    TEST_CHECK(child.exitCode == EXIT_FAILURE);
    TEST_CHECK(fs::is_directory(folder.path()));
  }

  // More folders at once than the exit table holds is a failed check, and all of them are still removed, the one
  // refused too. The parent's folder holds no place in the child's table.
  void tooManyAtOnceFails() {
    const TempFolder outer("luciole-temp-folder-test");
    const auto child = runChild([&] {
      std::vector<std::unique_ptr<TempFolder>> held;
      held.reserve(TempFolder::kMostAtOnce + 1);
      for (std::size_t i = 0; i <= TempFolder::kMostAtOnce; ++i) {
        held.push_back(std::make_unique<TempFolder>("luciole-temp-folder-many", outer.path()));
        std::println("{}", held.back()->path().native());
        std::fflush(stdout);
      }
    });
    TEST_CHECK(child.exitCode == EXIT_FAILURE);
    const auto said = lines(child.output);
    TEST_CHECK(said.size() == TempFolder::kMostAtOnce + 1);
    TEST_CHECK(said.back().contains("check failed"));
    for (std::size_t i = 0; i < TempFolder::kMostAtOnce; ++i) {
      TEST_CHECK(fs::path(said[i]).parent_path() == outer.path());
      TEST_CHECK(!fs::exists(said[i]));
    }
    TEST_CHECK(fs::is_empty(outer.path()));
  }

  // Whether `path` is gone within ten seconds; the watcher removes it once it sees its process end.
  bool goneSoon(const fs::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (fs::exists(path)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
  }

  enum class Ending : std::uint8_t { Abort, Terminate, KillGroup };

  // A child that holds a folder and ends without running its exit handlers: it aborts, as a fatal GLib warning
  // does, or a test runner sends its process group SIGTERM or SIGKILL. The folder is still removed.
  void aCrashOrAKillRemovesTheFolder(Ending ending) {
    const auto started = startChild([ending] {
      const TempFolder folder("luciole-temp-folder-test");
      fs::create_directories(folder.path() / "inner");
      std::ofstream(folder.path() / "inner" / "file.txt") << "kept until the end";
      lock(folder.path());
      std::println("{}", folder.path().native());
      std::fflush(stdout);
      if (ending == Ending::Abort) {
        const rlimit noCore{};
        ::setrlimit(RLIMIT_CORE, &noCore); // no core file for a crash made on purpose
        std::abort();
      }
      ::pause();
    });
    const auto said = readFrom(started, '\n');
    TEST_CHECK(said.ends_with('\n'));
    const fs::path path = said.substr(0, said.size() - 1);
    TEST_CHECK(path.parent_path() == fs::temp_directory_path());
    TEST_CHECK(fs::is_directory(path));
    if (ending != Ending::Abort) {
      TEST_CHECK(::killpg(started.pid, ending == Ending::Terminate ? SIGTERM : SIGKILL) == 0);
    }
    const auto child = finish(started, said);
    const int expected = ending == Ending::Abort ? SIGABRT : ending == Ending::Terminate ? SIGTERM : SIGKILL;
    TEST_CHECK(child.exitCode == -1 && child.signal == expected);
    TEST_CHECK(goneSoon(path));
  }

  // A prefix that is empty, too long or names another folder is refused before anything is made.
  void badPrefixesFail() {
    const std::array<std::string, 6> refused{
        "", "../luciole-escape", "a/b", "..", "Luciole-upper", std::string(TempFolder::kLongestPrefix + 1, 'x'),
    };
    for (const auto& prefix : refused) {
      const auto child = runChild([&] {
        const TempFolder folder(prefix);
        std::println("made {}", folder.path().native());
      });
      TEST_CHECK(child.exitCode == EXIT_FAILURE);
      TEST_CHECK(child.output.contains("check failed"));
      TEST_CHECK(!child.output.contains("made"));
    }
    const TempFolder longest(std::string(TempFolder::kLongestPrefix, 'x'));
    TEST_CHECK(fs::is_directory(longest.path()));
  }
} // namespace

int main() {
  aFolderGoesWithItsOwner();
  aFolderInAGivenParent();
  manyFoldersOneAfterAnother();
  aFailedCheckRemovesTheFolder();
  aChildLeavesTheParentsFolder();
  tooManyAtOnceFails();
  aCrashOrAKillRemovesTheFolder(Ending::Abort);
  aCrashOrAKillRemovesTheFolder(Ending::Terminate);
  aCrashOrAKillRemovesTheFolder(Ending::KillGroup);
  badPrefixesFail();
  std::println("temp folder checks passed");
  return 0;
}
