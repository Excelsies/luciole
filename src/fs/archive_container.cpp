#include "fs/archive_container.h"

#include "fs/archive_type.h"
#include "fs/filename.h"
#include "fs/gio_ptr.h"

#include <algorithm>

namespace {
  // A name that is a real archive or a split or volume part: isSupportedArchive says yes to its name alone, or no even
  // to a ZIP MIME type.
  bool archiveByName(const luciole::Location& location) {
    return luciole::isSupportedArchive(location, "") || !luciole::isSupportedArchive(location, "application/zip");
  }
  bool readable(luciole::ArchiveContainerFormat format) {
    return format != luciole::ArchiveContainerFormat::TarGzip
        || luciole::archiveCompressionAvailable(luciole::ArchiveCompression::Gzip);
  }
} // namespace

std::optional<std::size_t> luciole::archiveContainerType(const Location& location, std::string_view mime) {
  const auto name = location.basename();
  if (!location.nativePath() || !name) {
    return std::nullopt;
  }
  const GString lower(g_ascii_strdown(name->c_str(), -1));
  const std::string_view text(lower.get());
  const auto dot = text.rfind('.');
  std::optional<std::size_t> found;
  if (dot != std::string_view::npos) {
    found = archiveContainerByName(text.substr(dot + 1));
  }
  if (!found && !mime.empty() && !archiveByName(location)) {
    const auto* type = std::ranges::find(kArchiveContainerTypes, mime, &ArchiveContainerType::mime);
    if (type != kArchiveContainerTypes.end()) {
      found = static_cast<std::size_t>(type - kArchiveContainerTypes.begin());
    }
  }
  if (!found || !readable(kArchiveContainerTypes[*found].format)) {
    return std::nullopt;
  }
  return found;
}

bool luciole::isArchiveContainer(const Location& location, std::string_view mime) {
  return archiveContainerType(location, mime).has_value();
}

bool luciole::isBrowsableArchive(const Location& location, std::string_view mime) {
  return isSupportedArchive(location, mime) || isArchiveContainer(location, mime);
}

bool luciole::opensAsArchive(
    const Location& location, std::string_view mime, const ArchiveContainerSet& openAsArchive
) {
  if (const auto type = archiveContainerType(location, mime)) {
    return openAsArchive.test(*type);
  }
  return isSupportedArchive(location, mime);
}

std::string luciole::archiveExtractFolderName(const Location& location) {
  const auto type = archiveContainerType(location, "");
  const auto name = location.basename();
  if (!type || !name) {
    return archiveFolderName(location);
  }
  // The suffix matched case-insensitively, so it is the last name.size() + 1 bytes of the file name.
  std::string stem = name->substr(0, name->size() - kArchiveContainerTypes[*type].name.size() - 1);
  return stem.empty() || filenameProblem(stem) ? "Archive" : stem;
}
