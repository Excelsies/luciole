#pragma once

#include "fs/cancel_token.h"
#include "fs/file_error.h"
#include "fs/location.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace luciole {
  // One revision of a user's file, taken from the open descriptor that read or wrote it.
  struct UserFileIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::int64_t modifiedSeconds = 0;
    std::int64_t modifiedNanoseconds = 0;
    std::int64_t changedSeconds = 0;
    std::int64_t changedNanoseconds = 0;
    bool operator==(const UserFileIdentity&) const = default;
  };

  struct UserFileContent {
    std::string bytes;
    bool truncated = false; // the file holds more than the limit; bytes is its first `limit` bytes
    UserFileIdentity identity;
  };

  enum class UserFileSaveStatus : std::uint8_t {
    Saved,
    SavedNotFlushed,   // the new bytes are in place but the folder could not be flushed to disk
    ChangedOnDisk,     // the file is not the revision that was read; nothing was written
    ReadOnlyFile,      // the process may not write the file
    ReadOnlyFolder,    // the folder is read-only or the process may not create files in it
    HardLinked,        // a replacement would split the file from its other names
    NotRegularFile,    // a folder, device, pipe or socket
    OwnerNotKept,      // the process may not give the new file the original owner, group or permissions
    AttributesNotKept, // an extended attribute or access control list could not be copied
    NoSpace,
    Cancelled,
    NotLocal, // only local files are saved
    Failed,   // message holds the system's reason
  };

  struct [[nodiscard]] UserFileSaveResult {
    UserFileSaveStatus status = UserFileSaveStatus::Failed;
    std::optional<UserFileIdentity> identity; // the saved revision: set for Saved and SavedNotFlushed
    std::string message;                      // empty for Saved
  };

  // Blocking; run on a worker. Follows symlinks, reads at most limit bytes of a regular file in reads of 64 KiB,
  // checking the token before each, and takes the identity from the same descriptor. A file that changes while it is
  // read is a Changed error.
  std::expected<UserFileContent, FileError>
  readUserFile(const Location& location, std::size_t limit, const CancelToken& token);

  // Blocking; run on a worker. Replaces the regular file at location, or the target of a symlink there, with
  // bytes, only when it still is the revision `expected` names. The new file is written beside it under a unique
  // temporary name with the original's mode, owner, group, extended attributes and access control lists, flushed,
  // renamed over the original and the folder flushed. Every refusal or failure leaves the folder as it was.
  // Cancellation is honored until the rename. Saves in this process check the revision and rename as one step, so
  // of two saves of one revision only the first succeeds; a writer in another process can still change the file
  // between that check and the rename. An append-only or immutable file is ReadOnlyFile. A file or folder that is not
  // writable is refused as such even when only the file's status-change time moved, as a change of permissions
  // does; a changed content, size, modification time or identity is ChangedOnDisk.
  UserFileSaveResult saveUserFile(
      const Location& location, std::string_view bytes, const UserFileIdentity& expected, const CancelToken& token
  );

  // Blocking; run on a worker. The refusal saveUserFile would give before writing anything, checked without writing:
  // a file that is not local, gone, not a regular file, hard-linked, read-only, or in a read-only folder. None when a
  // save may go ahead.
  std::optional<UserFileSaveResult> userFileSaveRefusal(const Location& location);

  // readUserFile on a worker; done runs on the calling thread's main context, also after cancellation.
  void startUserFileRead(
      Location location, std::size_t limit, const CancelToken& token,
      std::move_only_function<void(std::expected<UserFileContent, FileError>)> done
  );

  // saveUserFile on a worker; done always runs on the calling thread's main context, so a save that completed
  // after a late cancellation still reports Saved. Callers check their own lifetime in done.
  void startUserFileSave(
      Location location, std::string bytes, UserFileIdentity expected, const CancelToken& token,
      std::move_only_function<void(UserFileSaveResult)> done
  );
} // namespace luciole
