#pragma once

#include "fs/directory_entries.h"
#include "fs/location.h"
#include "fs/user_file_save.h"

#include <cstdint>
#include <optional>

namespace luciole {
  // What the listing says of a file edited in a column preview, entry by entry as the columns look it up again. A save
  // replaces the file, so the listing lists it anew under another entry id and drops the list's cursor with the old
  // one; the size and date it lists tell whether the file changed on disk. Entry ids count per listing, so another
  // listing, as a new column of the Columns view, can give the new file the id the old one had: the identity the save
  // reports, the device and inode of the file it wrote, tells it from the old one. Entries that list no device and
  // inode are told apart by their id alone.
  class EditedListing {
  public:
    struct Seen {
      bool relisted = false; // the listing holds the file under a new entry
      bool changed = false;  // the listed size or date is not the revision read or saved
    };

    // `entry` lists the file as editing starts.
    explicit EditedListing(const DirectoryEntry& entry);

    const Location& file() const noexcept { return m_file; }
    // The folder that lists the file.
    const std::optional<Location>& folder() const noexcept { return m_folder; }
    // The listing's entry of the file now, and `revision`, the one read or last saved. Facts not known yet, and facts
    // that change while `saving`, are compared once they settle.
    Seen see(const DirectoryEntry& entry, const UserFileIdentity& revision, bool saving);
    // A save started: the listing is to list the file anew. A refused save replaced nothing.
    void saveStarted() noexcept {
      m_awaited = true;
      m_saved.reset();
    }
    void saveRefused() noexcept { m_awaited = false; }
    // The save succeeded and wrote the file with this identity. Nothing is armed when the listing already listed the
    // file anew, since no relist is awaited.
    void saved(const UserFileIdentity& written) noexcept {
      if (m_awaited) {
        m_saved = written;
      }
    }
    // A save started and the listing has not listed the file anew since.
    bool awaited() const noexcept { return m_awaited; }

  private:
    struct Facts {
      std::optional<std::uint64_t> size;
      std::optional<std::uint64_t> modified; // seconds
      bool operator==(const Facts&) const = default;
    };

    Location m_file;
    std::optional<Location> m_folder;
    std::uint64_t m_id = 0;
    std::optional<UserFileIdentity> m_saved; // the file the save wrote, until the listing lists it
    Facts m_seen;                            // the facts last compared with the revision
    bool m_awaited = false;
  };
} // namespace luciole
