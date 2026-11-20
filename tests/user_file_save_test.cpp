#include "fs/user_file_save.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <gio/gio.h>
#include <iterator>
#include <optional>
#include <print>
#include <set>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <thread>
#include <unistd.h>
#include <vector>

// The kernel cannot be made to refuse these calls for an unprivileged test, so this executable interposes them.
// An armed error fails the next call once (fsync after fsyncPasses calls succeed); every other call reaches libc.
namespace {
  std::atomic<int> fchownError{0};
  std::atomic<int> fsetxattrError{0};
  std::atomic<int> renameatError{0};
  std::atomic<int> fsyncError{0};
  std::atomic<int> fsyncPasses{0};
  std::atomic<int> statxAttributes{0}; // reported as set on the next inspected file
  std::optional<luciole::CancelToken> cancelOnFsync;

  template <typename Function> Function libc(const char* name) {
    return reinterpret_cast<Function>(::dlsym(RTLD_NEXT, name));
  }

  bool injected(std::atomic<int>& armed) {
    const int error = armed.exchange(0);
    errno = error != 0 ? error : errno;
    return error != 0;
  }
} // namespace

extern "C" int fchown(int fd, uid_t owner, gid_t group) noexcept {
  static const auto real = libc<int (*)(int, uid_t, gid_t)>("fchown");
  return injected(fchownError) ? -1 : real(fd, owner, group);
}

extern "C" int fsetxattr(int fd, const char* name, const void* value, size_t size, int flags) noexcept {
  static const auto real = libc<int (*)(int, const char*, const void*, size_t, int)>("fsetxattr");
  return injected(fsetxattrError) ? -1 : real(fd, name, value, size, flags);
}

// libc names the last parameter `new`, which C++ reserves.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
extern "C" int renameat(int fromFolder, const char* from, int toFolder, const char* to) noexcept {
  static const auto real = libc<int (*)(int, const char*, int, const char*)>("renameat");
  return injected(renameatError) ? -1 : real(fromFolder, from, toFolder, to);
}

extern "C" int fsync(int fd) {
  static const auto real = libc<int (*)(int)>("fsync");
  if (cancelOnFsync) {
    cancelOnFsync->cancel();
  }
  const bool fail = fsyncError.load() != 0 && fsyncPasses.fetch_sub(1) <= 0;
  return fail && injected(fsyncError) ? -1 : real(fd);
}

extern "C" int statx(int dirfd, const char* path, int flags, unsigned int mask, struct statx* buf) noexcept {
  static const auto real = libc<int (*)(int, const char*, int, unsigned int, struct statx*)>("statx");
  const int result = real(dirfd, path, flags, mask, buf);
  const auto attributes = static_cast<std::uint64_t>(statxAttributes.exchange(0));
  if (result == 0) {
    buf->stx_attributes |= attributes;
    buf->stx_attributes_mask |= attributes;
  }
  return result;
}

namespace {
  namespace fs = std::filesystem;
  using luciole::CancelToken;
  using luciole::Location;
  using luciole::UserFileContent;
  using luciole::UserFileSaveResult;
  using Status = luciole::UserFileSaveStatus;

  constexpr std::size_t kLimit = 16 * 1024 * 1024;

  struct Fixture {
    luciole::test::TempFolder tempFolder{"luciole-user-save-test"};
    fs::path directory = tempFolder.path();
  };

  std::string contents(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  void write(const fs::path& path, const std::string& text, mode_t mode = 0644) {
    TEST_CHECK((std::ofstream(path, std::ios::binary | std::ios::trunc) << text).good());
    TEST_CHECK(::chmod(path.c_str(), mode) == 0);
  }

  std::set<std::string> names(const fs::path& directory) {
    std::set<std::string> result;
    for (const auto& item : fs::directory_iterator(directory)) {
      result.insert(item.path().filename().native());
    }
    return result;
  }

  Location at(const fs::path& path) { return Location::fromPath(path.native()); }

  UserFileContent load(const fs::path& path) {
    auto content = luciole::readUserFile(at(path), kLimit, {});
    TEST_CHECK(content.has_value());
    return std::move(*content);
  }

  UserFileSaveResult save(const fs::path& path, const std::string& bytes, const UserFileContent& read) {
    return luciole::saveUserFile(at(path), bytes, read.identity, {});
  }

  mode_t modeOf(const fs::path& path) {
    struct stat info{};
    TEST_CHECK(::lstat(path.c_str(), &info) == 0);
    return info.st_mode & 07777;
  }

  void testReadIdentityAndLimit() {
    const Fixture fixture;
    const auto path = fixture.directory / "notes.txt";
    write(path, "0123456789");
    const auto whole = load(path);
    struct stat info{};
    TEST_CHECK(::stat(path.c_str(), &info) == 0);
    TEST_CHECK(whole.bytes == "0123456789" && !whole.truncated);
    const auto& id = whole.identity;
    TEST_CHECK(id.device == info.st_dev && id.inode == info.st_ino && id.size == 10);
    TEST_CHECK(id.modifiedSeconds == info.st_mtim.tv_sec && id.modifiedNanoseconds == info.st_mtim.tv_nsec);
    TEST_CHECK(id.changedSeconds == info.st_ctim.tv_sec && id.changedNanoseconds == info.st_ctim.tv_nsec);
    const auto cut = luciole::readUserFile(at(path), 4, {});
    TEST_CHECK(cut && cut->bytes == "0123" && cut->truncated && cut->identity == whole.identity);
    const auto exact = luciole::readUserFile(at(path), 10, {});
    TEST_CHECK(exact && exact->bytes == "0123456789" && !exact->truncated);
    const auto empty = luciole::readUserFile(at(path), 0, {});
    TEST_CHECK(empty && empty->bytes.empty() && empty->truncated);
    write(fixture.directory / "zero", "");
    const auto zero = load(fixture.directory / "zero");
    TEST_CHECK(zero.bytes.empty() && !zero.truncated && zero.identity.size == 0);
    const auto missing = luciole::readUserFile(at(fixture.directory / "missing"), kLimit, {});
    TEST_CHECK(!missing && missing.error().kind == luciole::FileErrorKind::Missing);
    CancelToken cancelled;
    cancelled.cancel();
    const auto stopped = luciole::readUserFile(at(path), kLimit, cancelled);
    TEST_CHECK(!stopped && stopped.error().kind == luciole::FileErrorKind::Cancelled);
  }

  void testModesKept() {
    const Fixture fixture;
    for (const mode_t mode : std::array<mode_t, 4>{0644, 0600, 0640, 0755}) {
      const auto path = fixture.directory / std::format("mode-{:o}", mode);
      write(path, "old\n", mode);
      const auto result = save(path, "new content\n", load(path));
      TEST_CHECK(result.status == Status::Saved && result.message.empty());
      TEST_CHECK(contents(path) == "new content\n" && modeOf(path) == mode);
      struct stat info{};
      TEST_CHECK(::stat(path.c_str(), &info) == 0 && info.st_uid == ::geteuid() && info.st_nlink == 1);
    }
    const auto readOnly = fixture.directory / "read-only";
    write(readOnly, "keep\n", 0444);
    const auto before = names(fixture.directory);
    const auto refused = save(readOnly, "changed\n", load(readOnly));
    TEST_CHECK(refused.status == Status::ReadOnlyFile && !refused.identity && !refused.message.empty());
    TEST_CHECK(contents(readOnly) == "keep\n" && modeOf(readOnly) == 0444 && names(fixture.directory) == before);
  }

  void testGroupKept() {
    const Fixture fixture;
    const auto path = fixture.directory / "shared";
    write(path, "old", 0664);
    const int count = ::getgroups(0, nullptr);
    std::vector<gid_t> groups(static_cast<std::size_t>(std::max(count, 0)));
    TEST_CHECK(::getgroups(count, groups.data()) == count);
    const auto other = std::ranges::find_if(groups, [](gid_t group) { return group != ::getegid(); });
    if (other == groups.end()) {
      std::println("skip: the user has no supplementary group to test group ownership with");
      return;
    }
    TEST_CHECK(::chown(path.c_str(), static_cast<uid_t>(-1), *other) == 0);
    TEST_CHECK(save(path, "new", load(path)).status == Status::Saved);
    struct stat info{};
    TEST_CHECK(::stat(path.c_str(), &info) == 0 && info.st_gid == *other && (info.st_mode & 07777) == 0664);
  }

  std::optional<std::string> attribute(const fs::path& path, const char* name) {
    std::array<char, 256> buffer{};
    const auto size = ::lgetxattr(path.c_str(), name, buffer.data(), buffer.size());
    if (size < 0) {
      return std::nullopt;
    }
    return std::string(buffer.data(), static_cast<std::size_t>(size));
  }

  void testExtendedAttributesKept() {
    const Fixture fixture;
    const auto path = fixture.directory / "tagged";
    write(path, "old");
    constexpr std::string_view kTag = "blue \0 binary";
    if (::setxattr(path.c_str(), "user.luciole.tag", kTag.data(), kTag.size(), 0) != 0) {
      std::println("skip: this filesystem refuses user extended attributes: {}", std::strerror(errno));
      return;
    }
    TEST_CHECK(::setxattr(path.c_str(), "user.empty", "", 0, 0) == 0);
    TEST_CHECK(save(path, "new", load(path)).status == Status::Saved);
    TEST_CHECK(contents(path) == "new" && attribute(path, "user.luciole.tag") == std::string(kTag));
    TEST_CHECK(attribute(path, "user.empty") == std::string());
  }

  // A POSIX ACL in the kernel's little-endian xattr layout: version 2, then (tag, permissions, id) entries for the
  // owner (rw), user 0 (r), the group (r), the mask (r) and others (none).
  constexpr std::string_view kAccessControlList{
      "\x02\0\0\0\x01\0\x06\0\xff\xff\xff\xff\x02\0\x04\0\0\0\0\0\x04\0\x04\0\xff\xff\xff\xff"
      "\x10\0\x04\0\xff\xff\xff\xff\x20\0\0\0\xff\xff\xff\xff",
      44
  };
  constexpr const char* kAccess = "system.posix_acl_access";

  void testAccessControlListsKept() {
    const Fixture fixture;
    const auto path = fixture.directory / "acl";
    write(path, "old", 0640);
    if (::setxattr(path.c_str(), kAccess, kAccessControlList.data(), kAccessControlList.size(), 0) != 0) {
      std::println("skip: this filesystem refuses POSIX access control lists: {}", std::strerror(errno));
      return;
    }
    const auto mode = modeOf(path);
    const auto stored = attribute(path, kAccess);
    TEST_CHECK(stored.has_value());
    TEST_CHECK(save(path, "new", load(path)).status == Status::Saved);
    TEST_CHECK(attribute(path, kAccess) == stored && modeOf(path) == mode);
    // A folder's default ACL is given to every new file in it; a saved file must not gain one it did not have.
    const auto folder = fixture.directory / "inheriting";
    fs::create_directory(folder);
    const auto list = kAccessControlList;
    TEST_CHECK(::setxattr(folder.c_str(), "system.posix_acl_default", list.data(), list.size(), 0) == 0);
    write(fixture.directory / "plain", "old", 0640);
    fs::rename(fixture.directory / "plain", folder / "plain");
    TEST_CHECK(!attribute(folder / "plain", kAccess));
    TEST_CHECK(save(folder / "plain", "new", load(folder / "plain")).status == Status::Saved);
    TEST_CHECK(contents(folder / "plain") == "new" && modeOf(folder / "plain") == 0640);
    TEST_CHECK(!attribute(folder / "plain", kAccess));
  }

  void testSymlinkWrittenThrough() {
    const Fixture fixture;
    fs::create_directory(fixture.directory / "real");
    const auto target = fixture.directory / "real" / "target.txt";
    write(target, "old", 0640);
    const auto link = fixture.directory / "link.txt";
    fs::create_symlink("real/target.txt", link);
    const auto read = load(link);
    TEST_CHECK(read.bytes == "old");
    TEST_CHECK(save(link, "through the link", read).status == Status::Saved);
    TEST_CHECK(fs::is_symlink(link) && fs::read_symlink(link) == "real/target.txt");
    TEST_CHECK(contents(target) == "through the link" && modeOf(target) == 0640);
    TEST_CHECK((names(fixture.directory) == std::set<std::string>{"link.txt", "real"}));
    TEST_CHECK((names(fixture.directory / "real") == std::set<std::string>{"target.txt"}));
  }

  void testHardLinkRefused() {
    const Fixture fixture;
    const auto first = fixture.directory / "first";
    const auto second = fixture.directory / "second";
    write(first, "old");
    fs::create_hard_link(first, second);
    const auto before = names(fixture.directory);
    const auto result = save(first, "new", load(first));
    TEST_CHECK(result.status == Status::HardLinked && !result.identity && !result.message.empty());
    TEST_CHECK(contents(first) == "old" && contents(second) == "old" && fs::hard_link_count(first) == 2);
    TEST_CHECK(names(fixture.directory) == before);
  }

  void testChangedOnDiskRefused() {
    const Fixture fixture;
    const auto path = fixture.directory / "changing";
    using namespace std::chrono_literals;
    // Each external change waits past the coarsest timestamp tick, so a refusal never depends on clock luck.
    const auto refusedAfter = [&](const std::function<void()>& change, const std::string& expected) {
      write(path, "original");
      const auto read = load(path);
      std::this_thread::sleep_for(20ms);
      change();
      const auto before = names(fixture.directory);
      const auto result = save(path, "edited!!", read);
      TEST_CHECK(result.status == Status::ChangedOnDisk && !result.identity && !result.message.empty());
      TEST_CHECK(contents(path) == expected && names(fixture.directory) == before);
    };
    refusedAfter([&] { write(path, "different content"); }, "different content");
    refusedAfter([&] { write(path, "ORIGINAL"); }, "ORIGINAL");
    const timespec longAgo{.tv_sec = 1000000000, .tv_nsec = 0};
    const std::array past{longAgo, longAgo};
    refusedAfter([&] { TEST_CHECK(::utimensat(AT_FDCWD, path.c_str(), past.data(), 0) == 0); }, "original");
    refusedAfter([&] { TEST_CHECK(::chmod(path.c_str(), 0644) == 0); }, "original");
    refusedAfter(
        [&] {
          write(fixture.directory / "replacement", "original");
          fs::rename(fixture.directory / "replacement", path);
        },
        "original"
    );
    refusedAfter([&] { fs::remove(path); }, "");
    TEST_CHECK(!fs::exists(path) && names(fixture.directory).empty());
  }

  void testReadOnlyFolderRefused() {
    const Fixture fixture;
    const auto folder = fixture.directory / "locked";
    fs::create_directory(folder);
    const auto path = folder / "file";
    write(path, "old");
    const auto read = load(path);
    TEST_CHECK(::chmod(folder.c_str(), 0555) == 0);
    const auto result = save(path, "new", read);
    TEST_CHECK(::chmod(folder.c_str(), 0300) == 0);
    const auto unreadable = save(path, "new", read);
    TEST_CHECK(::chmod(folder.c_str(), 0755) == 0);
    TEST_CHECK(unreadable.status == Status::Failed && !unreadable.identity && !unreadable.message.empty());
    TEST_CHECK(result.status == Status::ReadOnlyFolder && !result.identity && !result.message.empty());
    TEST_CHECK((contents(path) == "old" && names(folder) == std::set<std::string>{"file"}));
  }

  void testNotRegularAndNotLocalRefused() {
    const Fixture fixture;
    const auto pipe = fixture.directory / "pipe";
    TEST_CHECK(::mkfifo(pipe.c_str(), 0644) == 0);
    TEST_CHECK(!luciole::readUserFile(at(pipe), kLimit, {}));
    TEST_CHECK(!luciole::readUserFile(at(fixture.directory), kLimit, {}));
    const auto plain = fixture.directory / "plain";
    write(plain, "old");
    const auto identity = load(plain).identity;
    TEST_CHECK(luciole::saveUserFile(at(pipe), "x", identity, {}).status == Status::NotRegularFile);
    TEST_CHECK(luciole::saveUserFile(at(fixture.directory), "x", identity, {}).status == Status::NotRegularFile);
    TEST_CHECK(luciole::saveUserFile(Location::fromPath("/"), "x", identity, {}).status == Status::NotRegularFile);
    const auto remote = Location::fromUri("sftp://example.invalid/home/file.txt");
    TEST_CHECK(luciole::saveUserFile(remote, "x", identity, {}).status == Status::NotLocal);
    const auto remoteRead = luciole::readUserFile(remote, kLimit, {});
    TEST_CHECK(!remoteRead && remoteRead.error().kind == luciole::FileErrorKind::Unsupported);
    TEST_CHECK((names(fixture.directory) == std::set<std::string>{"pipe", "plain"}));
  }

  void testFailureLeavesNothing() {
    const Fixture fixture;
    const auto path = fixture.directory / "big";
    write(path, "old");
    const auto read = load(path);
    const auto before = names(fixture.directory);
    rlimit original{};
    TEST_CHECK(::getrlimit(RLIMIT_FSIZE, &original) == 0);
    const auto previous = std::signal(SIGXFSZ, SIG_IGN);
    const rlimit limited{.rlim_cur = 64 * 1024, .rlim_max = original.rlim_max};
    TEST_CHECK(::setrlimit(RLIMIT_FSIZE, &limited) == 0);
    const auto result = save(path, std::string(256 * 1024, 'x'), read);
    TEST_CHECK(::setrlimit(RLIMIT_FSIZE, &original) == 0);
    std::signal(SIGXFSZ, previous);
    TEST_CHECK(result.status == Status::Failed && !result.message.empty() && !result.identity);
    TEST_CHECK(contents(path) == "old" && names(fixture.directory) == before);
    CancelToken cancelled;
    cancelled.cancel();
    const auto stopped = luciole::saveUserFile(at(path), "new", read.identity, cancelled);
    TEST_CHECK(stopped.status == Status::Cancelled && contents(path) == "old");
    TEST_CHECK(names(fixture.directory) == before);
  }

  void testSystemRefusals() {
    const Fixture fixture;
    const auto path = fixture.directory / "file";
    write(path, "old");
    const bool attributes = ::setxattr(path.c_str(), "user.kept", "1", 1, 0) == 0;
    const auto before = names(fixture.directory);
    const auto refusedWith = [&](std::atomic<int>& armed, int error, Status status) {
      const auto read = load(path);
      armed = error;
      const auto result = save(path, "new", read);
      armed = 0;
      TEST_CHECK(result.status == status && !result.identity && !result.message.empty());
      TEST_CHECK(contents(path) == "old" && names(fixture.directory) == before);
    };
    refusedWith(fchownError, EPERM, Status::OwnerNotKept);
    refusedWith(fchownError, EIO, Status::Failed);
    refusedWith(renameatError, EACCES, Status::ReadOnlyFolder);
    refusedWith(renameatError, EIO, Status::Failed);
    refusedWith(fsyncError, ENOSPC, Status::NoSpace);
    refusedWith(fsyncError, EIO, Status::Failed);
    // An append-only or immutable file cannot be replaced, even in a writable folder.
    refusedWith(statxAttributes, STATX_ATTR_APPEND, Status::ReadOnlyFile);
    refusedWith(statxAttributes, STATX_ATTR_IMMUTABLE, Status::ReadOnlyFile);
    // Cancellation is honored after the new file is flushed and before it replaces the original.
    cancelOnFsync = CancelToken{};
    const auto late = luciole::saveUserFile(at(path), "late", load(path).identity, *cancelOnFsync);
    cancelOnFsync.reset();
    TEST_CHECK(late.status == Status::Cancelled && contents(path) == "old" && names(fixture.directory) == before);
    if (attributes) {
      refusedWith(fsetxattrError, EPERM, Status::AttributesNotKept);
    } else {
      std::println("skip: this filesystem refuses user extended attributes");
    }
    // The second flush is the folder's: the file is replaced, and the result says it may not be on disk yet.
    fsyncPasses = 1;
    fsyncError = EIO;
    const auto unflushed = save(path, "new", load(path));
    fsyncError = 0;
    TEST_CHECK(unflushed.status == Status::SavedNotFlushed && unflushed.identity && !unflushed.message.empty());
    TEST_CHECK(contents(path) == "new" && names(fixture.directory) == before);
    TEST_CHECK(luciole::saveUserFile(at(path), "next", *unflushed.identity, {}).status == Status::Saved);
  }

  void testLargeSaveAndChainedSaves() {
    const Fixture fixture;
    const auto path = fixture.directory / "large.bin";
    write(path, "small", 0600);
    std::string large(8 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < large.size(); ++i) {
      large[i] = static_cast<char>((i * 131) % 251);
    }
    const auto first = save(path, large, load(path));
    TEST_CHECK(first.status == Status::Saved && first.identity && contents(path) == large && modeOf(path) == 0600);
    const auto reread = load(path);
    TEST_CHECK(reread.bytes == large && reread.identity == *first.identity);
    const auto second = luciole::saveUserFile(at(path), "second", *first.identity, {});
    TEST_CHECK(second.status == Status::Saved && contents(path) == "second");
    const auto third = luciole::saveUserFile(at(path), "third", *second.identity, {});
    TEST_CHECK(third.status == Status::Saved && contents(path) == "third");
    // The identity of a replaced revision no longer matches.
    const auto stale = luciole::saveUserFile(at(path), "stale", *first.identity, {});
    TEST_CHECK(stale.status == Status::ChangedOnDisk && contents(path) == "third");
    TEST_CHECK((names(fixture.directory) == std::set<std::string>{"large.bin"}));
  }

  // Two threads save the same revision at once: exactly one replaces the file and the other is refused.
  void testConcurrentSavesOfOneRevision() {
    const Fixture fixture;
    const auto path = fixture.directory / "contended";
    const std::array<std::string, 2> texts{"first", "second"};
    for (int round = 0; round < 300; ++round) {
      write(path, "base");
      const auto read = load(path);
      std::barrier start(2);
      std::array<std::optional<UserFileSaveResult>, 2> results;
      const auto run = [&](std::size_t index) {
        start.arrive_and_wait();
        results.at(index) = save(path, texts.at(index), read);
      };
      std::thread first(run, 0);
      run(1);
      first.join();
      const std::size_t winner = results[0]->status == Status::Saved ? 0 : 1;
      TEST_CHECK(results.at(winner)->status == Status::Saved);
      TEST_CHECK(results.at(1 - winner)->status == Status::ChangedOnDisk);
      TEST_CHECK(contents(path) == texts.at(winner) && results.at(winner)->identity == load(path).identity);
    }
    TEST_CHECK((names(fixture.directory) == std::set<std::string>{"contended"}));
  }

  // Starts a worker job and runs the main context until the job delivers its reply.
  template <typename Reply, typename Start> Reply delivered(const Start& start) {
    std::optional<Reply> reply;
    start([&](Reply value) { reply = std::move(value); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!reply) {
      while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::move(*reply);
  }

  void testWorkerJobs() {
    using Read = std::expected<UserFileContent, luciole::FileError>;
    const Fixture fixture;
    const auto path = fixture.directory / "async.txt";
    write(path, "before");
    const auto read = delivered<Read>([&](auto done) { startUserFileRead(at(path), kLimit, {}, std::move(done)); });
    TEST_CHECK(read && read->bytes == "before");
    const auto saved = delivered<UserFileSaveResult>([&](auto done) {
      startUserFileSave(at(path), "after", read->identity, {}, std::move(done));
    });
    TEST_CHECK(saved.status == Status::Saved && contents(path) == "after");
    CancelToken cancelled;
    cancelled.cancel();
    const auto stopped = delivered<UserFileSaveResult>([&](auto done) {
      startUserFileSave(at(path), "never", *saved.identity, cancelled, std::move(done));
    });
    TEST_CHECK(stopped.status == Status::Cancelled && contents(path) == "after");
  }
} // namespace

int main() {
  testReadIdentityAndLimit();
  testModesKept();
  testGroupKept();
  testExtendedAttributesKept();
  testAccessControlListsKept();
  testSymlinkWrittenThrough();
  testHardLinkRefused();
  testChangedOnDiskRefused();
  testReadOnlyFolderRefused();
  testNotRegularAndNotLocalRefused();
  testFailureLeavesNothing();
  testSystemRefusals();
  testLargeSaveAndChainedSaves();
  testConcurrentSavesOfOneRevision();
  testWorkerJobs();
  return 0;
}
