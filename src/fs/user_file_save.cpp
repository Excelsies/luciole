#include "fs/user_file_save.h"

#include "fs/background_job.h"
#include "fs/posix_fd.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <string_view>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  using luciole::Fd;
  using luciole::UserFileIdentity;
  using luciole::UserFileSaveResult;
  using Status = luciole::UserFileSaveStatus;

  constexpr std::size_t kChunk = 1024 * 1024;
  // Small enough that a cancelled read of a file of a few hundred KiB stops before its end.
  constexpr std::size_t kReadChunk = std::size_t{64} * 1024;
  // Retries of one call interrupted by a signal, and of a size query that raced a growing attribute list.
  constexpr int kMaxRetries = 64;
  constexpr int kMaxNameAttempts = 32;

  std::string systemMessage(int error) { return std::error_code(error, std::generic_category()).message(); }

  UserFileSaveResult refuse(Status status, std::string message) {
    return {.status = status, .identity = std::nullopt, .message = std::move(message)};
  }

  UserFileSaveResult failure(std::string_view operation, int error) {
    const auto status = error == ENOSPC || error == EDQUOT ? Status::NoSpace : Status::Failed;
    return refuse(status, std::format("{}: {}", operation, systemMessage(error)));
  }

  // A permission or read-only filesystem error becomes `denied`; any other error is a failure.
  UserFileSaveResult refusal(Status denied, std::string_view operation, int error) {
    if (error == EACCES || error == EPERM || error == EROFS) {
      return refuse(denied, std::format("{}: {}", operation, systemMessage(error)));
    }
    return failure(operation, error);
  }

  UserFileIdentity identityOf(const struct stat& info) {
    return {
        .device = static_cast<std::uint64_t>(info.st_dev),
        .inode = static_cast<std::uint64_t>(info.st_ino),
        .size = static_cast<std::uint64_t>(info.st_size),
        .modifiedSeconds = info.st_mtim.tv_sec,
        .modifiedNanoseconds = info.st_mtim.tv_nsec,
        .changedSeconds = info.st_ctim.tv_sec,
        .changedNanoseconds = info.st_ctim.tv_nsec,
    };
  }

  ssize_t retryInterrupted(const std::function<ssize_t()>& call) {
    auto result = call();
    for (int attempt = 1; attempt < kMaxRetries && result < 0 && errno == EINTR; ++attempt) {
      result = call();
    }
    return result;
  }

  // Writes everything, checking the token between chunks; 0 on success, else the errno value or ECANCELED.
  int writeAll(int fd, std::string_view bytes, const luciole::CancelToken& token) {
    // Bounded: each pass writes at least one byte or returns.
    while (!bytes.empty()) {
      if (token.cancelled()) {
        return ECANCELED;
      }
      const auto size = std::min(bytes.size(), kChunk);
      const auto count = retryInterrupted([&] { return ::write(fd, bytes.data(), size); });
      if (count <= 0) {
        return count == 0 ? EIO : errno;
      }
      bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    return 0;
  }

  // An attribute value or name list, asked for its size first and asked again when it grew meanwhile.
  std::expected<std::string, int> attributeBytes(const std::function<ssize_t(char*, std::size_t)>& query) {
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
      const auto size = query(nullptr, 0);
      if (size <= 0) {
        return size == 0 ? std::expected<std::string, int>(std::string()) : std::unexpected(errno);
      }
      std::string buffer(static_cast<std::size_t>(size), '\0');
      const auto filled = query(buffer.data(), buffer.size());
      if (filled >= 0 || errno != ERANGE) {
        return filled >= 0 ? std::expected<std::string, int>(buffer.substr(0, static_cast<std::size_t>(filled)))
                           : std::unexpected(errno);
      }
    }
    return std::unexpected(ERANGE);
  }

  // Attribute names of an open file; an empty list when the filesystem has none.
  std::expected<std::vector<std::string>, int> attributeNames(int fd) {
    const auto list = attributeBytes([fd](char* buffer, std::size_t size) { return ::flistxattr(fd, buffer, size); });
    if (!list) {
      return list.error() == ENOTSUP ? std::expected<std::vector<std::string>, int>() : std::unexpected(list.error());
    }
    std::vector<std::string> names;
    std::string_view rest = *list;
    // Bounded by the list size: each pass consumes at least one byte.
    while (!rest.empty()) {
      const auto end = std::min(rest.find('\0'), rest.size());
      if (end > 0) {
        names.emplace_back(rest.substr(0, end));
      }
      rest.remove_prefix(std::min(end + 1, rest.size()));
    }
    return names;
  }

  // Makes the temporary file's extended attributes, access control lists included, equal to the original's.
  std::optional<UserFileSaveResult> copyAttributes(int from, int to) {
    const auto wanted = attributeNames(from);
    const auto present = attributeNames(to);
    if (!wanted || !present) {
      return failure("Cannot list the extended attributes", wanted ? present.error() : wanted.error());
    }
    for (const auto& name : *present) {
      if (!std::ranges::contains(*wanted, name) && ::fremovexattr(to, name.c_str()) != 0) {
        return refuse(Status::AttributesNotKept, std::format("Cannot remove {}: {}", name, systemMessage(errno)));
      }
    }
    for (const auto& name : *wanted) {
      const auto value = attributeBytes([from, &name](char* buffer, std::size_t size) {
        return ::fgetxattr(from, name.c_str(), buffer, size);
      });
      if (!value || ::fsetxattr(to, name.c_str(), value->data(), value->size(), 0) != 0) {
        const int error = value ? errno : value.error();
        return refuse(Status::AttributesNotKept, std::format("Cannot keep {}: {}", name, systemMessage(error)));
      }
    }
    return std::nullopt;
  }

  // A file in the target's folder that is removed again unless it was renamed over the target.
  class TemporaryFile {
  public:
    explicit TemporaryFile(int folder) : m_folder(folder) {}
    ~TemporaryFile() {
      if (!m_name.empty()) {
        ::unlinkat(m_folder, m_name.c_str(), 0);
      }
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    std::optional<UserFileSaveResult> create() {
      for (int attempt = 0; attempt < kMaxNameAttempts; ++attempt) {
        std::array<std::uint8_t, 8> nonce{};
        const auto filled = retryInterrupted([&] { return ::getrandom(nonce.data(), nonce.size(), GRND_NONBLOCK); });
        if (filled != static_cast<ssize_t>(nonce.size())) {
          return failure("Cannot choose a temporary name", filled < 0 ? errno : EIO);
        }
        std::uint64_t value = 0;
        for (const auto byte : nonce) {
          value = (value << 8U) | byte;
        }
        auto name = std::format(".luciole-save-{:016x}", value);
        const int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY;
        const int fd = ::openat(m_folder, name.c_str(), flags, 0600);
        if (fd >= 0) {
          m_name = std::move(name);
          m_file = Fd(fd);
          return std::nullopt;
        }
        if (errno != EEXIST) {
          return refusal(Status::ReadOnlyFolder, "Cannot create a file in the folder", errno);
        }
      }
      return refuse(Status::Failed, "Cannot find a free temporary name in the folder.");
    }

    int fd() const { return m_file.get(); }
    const std::string& name() const { return m_name; }
    // After the rename the name belongs to the saved file.
    void keep() { m_name.clear(); }
    int close() { return m_file.close(); }

  private:
    int m_folder;
    std::string m_name;
    Fd m_file;
  };

  struct Target {
    Fd folder;
    std::string name;
    Fd file;
    struct stat info{};
    // The file is exactly the revision expected, the status-change time included.
    bool exact = true;
  };

  // The content, size, modification time and identity of the file, without its status-change time, which a change of
  // permissions alone moves.
  bool sameRevision(UserFileIdentity found, UserFileIdentity expected) {
    found.changedSeconds = expected.changedSeconds;
    found.changedNanoseconds = expected.changedNanoseconds;
    return found == expected;
  }

  // With no expected revision, any revision of the file is accepted.
  std::expected<Target, UserFileSaveResult>
  openTarget(const std::string& path, const std::optional<UserFileIdentity>& expected) {
    const auto changed = [] { return refuse(Status::ChangedOnDisk, "The file changed on disk since it was read."); };
    std::error_code error;
    const auto resolved = std::filesystem::canonical(path, error);
    if (error) {
      return std::unexpected(
          error == std::errc::no_such_file_or_directory ? changed() : failure("Cannot resolve the file", error.value())
      );
    }
    Target target{.folder = Fd(), .name = resolved.filename().native(), .file = Fd()};
    if (target.name.empty()) {
      return std::unexpected(refuse(Status::NotRegularFile, "Only regular files are saved."));
    }
    target.folder = Fd(::open(resolved.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!target.folder.valid()) {
      return std::unexpected(failure("Cannot open the folder", errno));
    }
    if (::fstatat(target.folder.get(), target.name.c_str(), &target.info, AT_SYMLINK_NOFOLLOW) != 0) {
      return std::unexpected(errno == ENOENT ? changed() : failure("Cannot inspect the file", errno));
    }
    // A link here appeared after the path was resolved; it is refused like any other non-regular file.
    if (!S_ISREG(target.info.st_mode)) {
      return std::unexpected(refuse(Status::NotRegularFile, "Only regular files are saved."));
    }
    if (target.info.st_nlink > 1) {
      return std::unexpected(refuse(
          Status::HardLinked, std::format("The file has {} names; saving would separate them.", target.info.st_nlink)
      ));
    }
    if (expected && !sameRevision(identityOf(target.info), *expected)) {
      return std::unexpected(changed());
    }
    target.exact = !expected || identityOf(target.info) == *expected;
    const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY;
    target.file = Fd(::openat(target.folder.get(), target.name.c_str(), flags));
    struct stat opened{};
    if (!target.file.valid() || ::fstat(target.file.get(), &opened) != 0) {
      return std::unexpected(failure("Cannot open the file", errno));
    }
    if (expected && !sameRevision(identityOf(opened), *expected)) {
      return std::unexpected(changed());
    }
    target.exact = target.exact && (!expected || identityOf(opened) == *expected);
    return target;
  }

  std::optional<UserFileSaveResult> checkWritable(const Target& target) {
    if (::faccessat(target.folder.get(), ".", W_OK | X_OK, AT_EACCESS) != 0) {
      return refusal(Status::ReadOnlyFolder, "The folder is read-only", errno);
    }
    if (::faccessat(target.folder.get(), target.name.c_str(), W_OK, AT_EACCESS) != 0) {
      return refusal(Status::ReadOnlyFile, "The file is read-only", errno);
    }
    // The access check passes an append-only file, which the rename would still refuse.
    struct statx attributes{};
    if (::statx(target.file.get(), "", AT_EMPTY_PATH, 0, &attributes) != 0) {
      return failure("Cannot inspect the file", errno);
    }
    const std::uint64_t fixed = STATX_ATTR_APPEND | STATX_ATTR_IMMUTABLE;
    if ((attributes.stx_attributes & attributes.stx_attributes_mask & fixed) != 0) {
      return refuse(Status::ReadOnlyFile, "The file is append-only or immutable.");
    }
    return std::nullopt;
  }

  // Owner, group, attributes and mode, in that order: a change of owner clears set-ID bits and capabilities.
  std::optional<UserFileSaveResult> copyMetadata(const Target& target, int temporary) {
    if (::fchown(temporary, target.info.st_uid, target.info.st_gid) != 0) {
      return refusal(Status::OwnerNotKept, "This user cannot keep the file's owner and group", errno);
    }
    if (auto refused = copyAttributes(target.file.get(), temporary)) {
      return refused;
    }
    const auto mode = target.info.st_mode & 07777;
    struct stat written{};
    if (::fchmod(temporary, mode) != 0 || ::fstat(temporary, &written) != 0) {
      return failure("Cannot set the permissions", errno);
    }
    if ((written.st_mode & 07777) != mode
        || written.st_uid != target.info.st_uid
        || written.st_gid != target.info.st_gid) {
      return refuse(Status::OwnerNotKept, "This user cannot keep the file's permissions.");
    }
    return std::nullopt;
  }

  // Makes the last identity check and the rename one step for every save in this process.
  std::mutex replaceMutex;

  UserFileSaveResult replace(const Target& target, TemporaryFile& temporary, const UserFileIdentity& expected) {
    {
      const std::scoped_lock lock(replaceMutex);
      struct stat current{};
      if (::fstatat(target.folder.get(), target.name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
          || !S_ISREG(current.st_mode)
          || current.st_nlink != 1
          || identityOf(current) != expected) {
        return refuse(Status::ChangedOnDisk, "The file changed on disk while it was being saved.");
      }
      if (::renameat(target.folder.get(), temporary.name().c_str(), target.folder.get(), target.name.c_str()) != 0) {
        return refusal(Status::ReadOnlyFolder, "Cannot replace the file", errno);
      }
    }
    temporary.keep();
    struct stat saved{};
    if (::fstat(temporary.fd(), &saved) != 0) {
      return failure("The file was replaced but cannot be inspected", errno);
    }
    UserFileSaveResult result{.status = Status::Saved, .identity = identityOf(saved), .message = {}};
    if (retryInterrupted([&] { return ::fsync(target.folder.get()); }) != 0) {
      result.status = Status::SavedNotFlushed;
      result.message = std::format("The folder could not be flushed to disk: {}", systemMessage(errno));
    } else if (temporary.close() != 0) {
      result.status = Status::SavedNotFlushed;
      result.message = std::format("The saved file could not be closed: {}", systemMessage(errno));
    }
    return result;
  }

  luciole::FileError cancelledError() { return {.kind = luciole::FileErrorKind::Cancelled, .message = "Cancelled."}; }

  luciole::FileError readError(luciole::FileErrorKind kind, std::string message) {
    return {.kind = kind, .message = std::move(message)};
  }
} // namespace

std::expected<luciole::UserFileContent, luciole::FileError>
luciole::readUserFile(const Location& location, std::size_t limit, const CancelToken& token) {
  if (token.cancelled()) {
    return std::unexpected(cancelledError());
  }
  const auto path = location.isNative() ? location.nativePath() : std::nullopt;
  if (!path) {
    return std::unexpected(readError(FileErrorKind::Unsupported, "Only local files are read."));
  }
  struct stat named{};
  if (::stat(path->c_str(), &named) != 0) {
    return std::unexpected(errnoError(errno));
  }
  if (!S_ISREG(named.st_mode)) {
    return std::unexpected(readError(FileErrorKind::Unsupported, "Only regular files are read."));
  }
  const Fd file(::open(path->c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY));
  struct stat before{};
  if (!file.valid() || ::fstat(file.get(), &before) != 0) {
    return std::unexpected(errnoError(errno));
  }
  if (!S_ISREG(before.st_mode) || before.st_dev != named.st_dev || before.st_ino != named.st_ino) {
    return std::unexpected(readError(FileErrorKind::Changed, "The file was replaced while opening."));
  }
  UserFileContent content;
  content.bytes.reserve(std::min(limit, static_cast<std::size_t>(before.st_size)));
  // Bounded by limit: each pass adds at least one byte or ends.
  while (content.bytes.size() < limit) {
    if (token.cancelled()) {
      return std::unexpected(cancelledError());
    }
    const auto start = content.bytes.size();
    const auto want = std::min(kReadChunk, limit - start);
    content.bytes.resize(start + want);
    const auto count = retryInterrupted([&] { return ::read(file.get(), content.bytes.data() + start, want); });
    content.bytes.resize(start + static_cast<std::size_t>(std::max<ssize_t>(count, 0)));
    if (count <= 0) {
      if (count < 0) {
        return std::unexpected(errnoError(errno));
      }
      break;
    }
  }
  if (content.bytes.size() == limit) {
    char extra = 0;
    const auto count = retryInterrupted([&] { return ::read(file.get(), &extra, 1); });
    if (count < 0) {
      return std::unexpected(errnoError(errno));
    }
    content.truncated = count > 0;
  }
  struct stat after{};
  if (::fstat(file.get(), &after) != 0) {
    return std::unexpected(errnoError(errno));
  }
  content.identity = identityOf(after);
  if (content.identity != identityOf(before)) {
    return std::unexpected(readError(FileErrorKind::Changed, "The file changed while it was read."));
  }
  return content;
}

luciole::UserFileSaveResult luciole::saveUserFile(
    const Location& location, std::string_view bytes, const UserFileIdentity& expected, const CancelToken& token
) {
  if (token.cancelled()) {
    return refuse(Status::Cancelled, "Cancelled.");
  }
  const auto path = location.isNative() ? location.nativePath() : std::nullopt;
  if (!path) {
    return refuse(Status::NotLocal, "Only local files can be saved.");
  }
  auto target = openTarget(*path, expected);
  if (!target) {
    return std::move(target.error());
  }
  // Read-only is reported before a changed status-change time, which a change of permissions alone causes.
  if (auto refused = checkWritable(*target)) {
    return std::move(*refused);
  }
  if (!target->exact) {
    return refuse(Status::ChangedOnDisk, "The file changed on disk since it was read.");
  }
  TemporaryFile temporary(target->folder.get());
  if (auto refused = temporary.create()) {
    return std::move(*refused);
  }
  if (const int error = writeAll(temporary.fd(), bytes, token); error != 0) {
    return error == ECANCELED ? refuse(Status::Cancelled, "Cancelled.") : failure("Cannot write the file", error);
  }
  if (auto refused = copyMetadata(*target, temporary.fd())) {
    return std::move(*refused);
  }
  if (retryInterrupted([&] { return ::fsync(temporary.fd()); }) != 0) {
    return failure("Cannot flush the file to disk", errno);
  }
  if (token.cancelled()) {
    return refuse(Status::Cancelled, "Cancelled.");
  }
  return replace(*target, temporary, expected);
}

std::optional<luciole::UserFileSaveResult> luciole::userFileSaveRefusal(const Location& location) {
  const auto path = location.isNative() ? location.nativePath() : std::nullopt;
  if (!path) {
    return refuse(Status::NotLocal, "Only local files can be saved.");
  }
  const auto target = openTarget(*path, std::nullopt);
  if (!target) {
    return target.error();
  }
  return checkWritable(*target);
}

void luciole::startUserFileRead(
    Location location, std::size_t limit, const CancelToken& token,
    std::move_only_function<void(std::expected<UserFileContent, FileError>)> done
) {
  runJob<UserFileContent>(
      token,
      [location = std::move(location), limit, token] {
        auto content = readUserFile(location, limit, token);
        if (!content) {
          throw FileErrorException(std::move(content.error()));
        }
        return std::move(*content);
      },
      std::move(done)
  );
}

void luciole::startUserFileSave(
    Location location, std::string bytes, UserFileIdentity expected, const CancelToken& token,
    std::move_only_function<void(UserFileSaveResult)> done
) {
  runJob<UserFileSaveResult>(
      token,
      [location = std::move(location), bytes = std::move(bytes), expected, token] {
        return saveUserFile(location, bytes, expected, token);
      },
      [done = std::move(done)](JobResult<UserFileSaveResult> reply) mutable {
        if (reply) {
          done(std::move(*reply));
          return;
        }
        // Only a cancellation before the worker started, or an exception, arrives as an error.
        const auto cancelled = reply.error().kind == FileErrorKind::Cancelled;
        done(refuse(cancelled ? Status::Cancelled : Status::Failed, std::move(reply.error().message)));
      }
  );
}
