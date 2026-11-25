#include "fs/archive_extra_types.h"

#include "fs/filename.h"
#include "fs/gio_ptr.h"

#include <algorithm>
#include <archive.h>
#include <lzma.h>

bool luciole::archiveExtraNeedAvailable(ArchiveExtraNeed need) {
  switch (need) {
  case ArchiveExtraNeed::None:
  case ArchiveExtraNeed::Compress:
    return true;
  case ArchiveExtraNeed::Lz4:
    return archive_liblz4_version() != nullptr;
  case ArchiveExtraNeed::LzmaAlone:
  case ArchiveExtraNeed::Lzip:
    return lzma_filter_decoder_is_supported(LZMA_FILTER_LZMA1) != 0;
  }
  return false;
}

namespace {
  std::optional<std::size_t> typeOfSuffix(std::string_view lowerName) {
    for (std::size_t index = 0; index < luciole::kArchiveExtraTypes.size(); ++index) {
      const auto suffix = luciole::kArchiveExtraTypes[index].suffix;
      // The suffix follows a dot and a stem: ".a" alone is a hidden file, not an archive.
      if (lowerName.size() > suffix.size() + 1
          && lowerName.ends_with(suffix)
          && lowerName[lowerName.size() - suffix.size() - 1] == '.') {
        return index;
      }
    }
    return std::nullopt;
  }
} // namespace

std::optional<std::size_t> luciole::archiveExtraType(const Location& location, std::string_view mime) {
  const auto name = location.basename();
  if (!location.nativePath() || !name) {
    return std::nullopt;
  }
  const GString lower(g_ascii_strdown(name->c_str(), -1));
  auto found = typeOfSuffix(lower.get());
  if (!found && !mime.empty()) {
    const auto* type = std::ranges::find(kArchiveExtraTypes, mime, &ArchiveExtraType::mime);
    if (type != kArchiveExtraTypes.end()) {
      found = static_cast<std::size_t>(type - kArchiveExtraTypes.begin());
    }
  }
  if (!found || !archiveExtraNeedAvailable(kArchiveExtraTypes[*found].need)) {
    return std::nullopt;
  }
  return found;
}

std::string luciole::archiveExtraFolderName(const Location& location) {
  const auto name = location.basename();
  if (!name) {
    return {};
  }
  const GString lower(g_ascii_strdown(name->c_str(), -1));
  const auto type = typeOfSuffix(lower.get());
  if (!type) {
    return {};
  }
  // The suffix matched case-insensitively, so it is the last suffix.size() + 1 bytes of the file name.
  const auto stem = name->substr(0, name->size() - kArchiveExtraTypes[*type].suffix.size() - 1);
  return filenameProblem(stem) ? "Archive" : stem;
}
