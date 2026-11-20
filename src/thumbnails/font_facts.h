#pragma once

#include "thumbnails/preview_metadata.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luciole {
  enum class FontContainer : std::uint8_t { Sfnt, Collection, Woff, Woff2 };

  // Largest font file the sheet and facts helpers read.
  inline constexpr std::uint64_t kMaxFontFileBytes = 256ULL * 1024U * 1024U;

  // Font file types, including the ODF template types that share the .otf and .otc globs; content decides.
  bool isFontContentType(std::string_view contentType);
  // Byte cap for both font helpers: the preview size limit in MiB (at least 1), at most kMaxFontFileBytes.
  std::uint64_t fontByteCap(int maxFileMiB);
  // Identifies the container by its signature and checks that every table it lists lies inside the bytes.
  std::expected<FontContainer, std::string> checkFontFile(std::span<const std::uint8_t> bytes);

  struct FontAxis {
    std::string tag;
    double minimum = 0;
    double maximum = 0;
  };

  // Facts of the first face, plus the face count of the whole file.
  struct FontFacts {
    FontContainer container = FontContainer::Sfnt;
    std::string family;
    std::string style;
    std::string fullName;
    // Outline format as fontconfig names it: TrueType, CFF or CFF2.
    std::string outlines;
    // head.fontRevision, 16.16 fixed point.
    std::int32_t version = 0;
    std::int64_t glyphs = 0;
    std::int64_t faces = 0;
    std::vector<FontAxis> axes;
    bool monospace = false;
    bool color = false;
    std::vector<std::string_view> scripts;
  };

  // Parses an untrusted font held in memory; run it only in a resource-limited helper.
  std::expected<FontFacts, std::string> readFontFacts(std::span<const std::uint8_t> bytes);
  // Details rows for the preview and Properties.
  MetadataFields fontFactFields(const FontFacts& facts);
} // namespace luciole
