#pragma once

#include "fs/archive_container_types.h"
#include "fs/location.h"

#include <optional>
#include <string>
#include <string_view>

namespace luciole {
  // The archive container type of a native file: by its suffix, else by a MIME type only that type has. A type of a
  // format whose filter this build lacks is not recognized. A file named like a real archive or a split part is none,
  // whatever MIME type it reports.
  std::optional<std::size_t> archiveContainerType(const Location& location, std::string_view mime);
  bool isArchiveContainer(const Location& location, std::string_view mime);
  // Whether Browse Archive and Extract take the file: a real archive or an archive container, whatever the setting.
  bool isBrowsableArchive(const Location& location, std::string_view mime);
  // Whether Open shows the archive browser: a real archive, or an archive container its setting switches on. A
  // container that is off goes to its application even when the system reports it as an archive.
  bool opensAsArchive(const Location& location, std::string_view mime, const ArchiveContainerSet& openAsArchive);
  // The folder a file extracts into by default: archiveFolderName, and for an archive container its suffix removed
  // too, so the folder never has the file's own name. "Archive" when nothing usable remains.
  std::string archiveExtractFolderName(const Location& location);
} // namespace luciole
