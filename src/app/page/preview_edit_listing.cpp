#include "app/page/preview_edit_listing.h"

namespace luciole {
  EditedListing::EditedListing(const DirectoryEntry& entry)
      : m_file(entry.location()), m_folder(m_file.parent()), m_id(entry.id),
        m_seen{.size = entry.size, .modified = entry.modifiedSeconds} {}

  EditedListing::Seen EditedListing::see(const DirectoryEntry& entry, const UserFileIdentity& revision, bool saving) {
    Seen seen;
    const bool savedFile =
        m_saved && entry.tags.identity && entry.tags.device == m_saved->device && entry.tags.inode == m_saved->inode;
    if (entry.id != m_id || savedFile) {
      m_id = entry.id;
      m_awaited = false;
      m_saved.reset();
      seen.relisted = true;
    }
    const Facts now{.size = entry.size, .modified = entry.modifiedSeconds};
    if (!now.size || !now.modified || now == m_seen || saving) {
      return seen;
    }
    m_seen = now;
    const auto modified = revision.modifiedSeconds;
    const Facts read{
        .size = revision.size,
        .modified = modified >= 0 ? std::optional(static_cast<std::uint64_t>(modified)) : std::nullopt,
    };
    seen.changed = now != read;
    return seen;
  }
} // namespace luciole
