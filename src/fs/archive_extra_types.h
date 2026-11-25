#pragma once

#include "fs/location.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace luciole {
  // What an archive type needs from this build before it can be read.
  enum class ArchiveExtraNeed : std::uint8_t {
    None,      // libarchive reads the format itself
    Lz4,       // the in-process lz4 library; libarchive would otherwise run an lz4 program
    LzmaAlone, // liblzma's lzma-alone decoder
    Lzip,      // liblzma's lzip decoder
    Compress   // libarchive's own LZW filter
  };
  // An archive type of its own file name suffix and content type, read and never written.
  struct ArchiveExtraType {
    std::string_view suffix; // without the dot, in lower case
    std::string_view mime;
    ArchiveExtraNeed need;
  };
  // The one list of these types: the name rules and the reader both follow it.
  inline constexpr auto kArchiveExtraTypes = std::to_array<ArchiveExtraType>({
      {"tar.lz4", "application/x-lz4-compressed-tar", ArchiveExtraNeed::Lz4},
      {"tlz4", "application/x-lz4-compressed-tar", ArchiveExtraNeed::Lz4},
      {"tar.lzma", "application/x-lzma-compressed-tar", ArchiveExtraNeed::LzmaAlone},
      {"tlz", "application/x-lzma-compressed-tar", ArchiveExtraNeed::LzmaAlone},
      {"tar.lz", "application/x-lzip-compressed-tar", ArchiveExtraNeed::Lzip},
      {"tar.z", "application/x-tarz", ArchiveExtraNeed::Compress},
      {"taz", "application/x-tarz", ArchiveExtraNeed::Compress},
      {"a", "application/x-archive", ArchiveExtraNeed::None},
      {"ar", "application/x-archive", ArchiveExtraNeed::None},
      {"cpio", "application/x-cpio", ArchiveExtraNeed::None},
  });

  // Whether this build can read what `need` names.
  bool archiveExtraNeedAvailable(ArchiveExtraNeed need);
  // The type of a native file: by its suffix, else by a content type only that type has. None when the build lacks
  // what the type needs.
  std::optional<std::size_t> archiveExtraType(const Location& location, std::string_view mime);
  // The folder a file of such a type extracts into: its name without the suffix. Empty when it is no such type.
  std::string archiveExtraFolderName(const Location& location);
} // namespace luciole
