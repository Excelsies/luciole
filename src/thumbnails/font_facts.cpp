#include "thumbnails/font_facts.h"

#include "thumbnails/font_sample.h"

#include <algorithm>
#include <array>
#include <fontconfig/fcfreetype.h>
#include <format>
#include <ft2build.h>
#include <memory>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

namespace {
  using luciole::FontContainer;
  constexpr std::uint32_t kMaxTables = 1024;
  constexpr std::uint32_t kMaxCollectionFaces = 256;
  constexpr std::uint32_t kMaxAxes = 16;
  constexpr std::array<std::string_view, 12> kFontTypes{
      "font/ttf",
      "font/otf",
      "font/collection",
      "font/woff",
      "font/woff2",
      "font/sfnt",
      "application/x-font-ttf",
      "application/x-font-otf",
      "application/font-woff",
      "application/vnd.ms-opentype",
      // Their globs are .otf and .otc, which the file name alone cannot tell apart from fonts.
      "application/vnd.oasis.opendocument.formula-template",
      "application/vnd.oasis.opendocument.chart-template",
  };

  std::uint32_t bigEndian(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t width) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
      value = (value << 8) | bytes[offset + i];
    }
    return value;
  }
  bool inside(std::span<const std::uint8_t> bytes, std::uint64_t offset, std::uint64_t length) {
    return offset <= bytes.size() && length <= bytes.size() - offset;
  }
  bool sfntVersion(std::uint32_t version) {
    return version == 0x00010000U || version == 0x74727565U || version == 0x4F54544FU;
  }
  // The table directory at `start` and every table it lists fit in the bytes.
  bool sfntIntact(std::span<const std::uint8_t> bytes, std::uint64_t start) {
    if (!inside(bytes, start, 12) || !sfntVersion(bigEndian(bytes, start, 4))) {
      return false;
    }
    const auto count = bigEndian(bytes, start + 4, 2);
    if (count == 0 || count > kMaxTables || !inside(bytes, start + 12, 16ULL * count)) {
      return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto record = start + 12 + (16ULL * i);
      if (!inside(bytes, bigEndian(bytes, record + 8, 4), bigEndian(bytes, record + 12, 4))) {
        return false;
      }
    }
    return true;
  }
  bool collectionIntact(std::span<const std::uint8_t> bytes) {
    if (!inside(bytes, 0, 12)) {
      return false;
    }
    const auto count = bigEndian(bytes, 8, 4);
    if (count == 0 || count > kMaxCollectionFaces || !inside(bytes, 12, 4ULL * count)) {
      return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      if (!sfntIntact(bytes, bigEndian(bytes, 12 + (4ULL * i), 4))) {
        return false;
      }
    }
    return true;
  }
  // WOFF lists compressed tables; WOFF2 tables are one stream that only FreeType decodes, so its header is checked.
  bool woffIntact(std::span<const std::uint8_t> bytes, bool second) {
    const std::size_t header = second ? 48 : 44;
    if (!inside(bytes, 0, header) || bigEndian(bytes, 8, 4) != bytes.size()) {
      return false;
    }
    const auto count = bigEndian(bytes, 12, 2);
    if (count == 0 || count > kMaxTables) {
      return false;
    }
    if (second) {
      return true;
    }
    if (!inside(bytes, header, 20ULL * count)) {
      return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto record = header + (20ULL * i);
      if (!inside(bytes, bigEndian(bytes, record + 4, 4), bigEndian(bytes, record + 8, 4))) {
        return false;
      }
    }
    return true;
  }
  std::string text(FcPattern* pattern, const char* object) {
    FcChar8* value = nullptr;
    return FcPatternGetString(pattern, object, 0, &value) == FcResultMatch && value
        ? std::string(reinterpret_cast<const char*>(value))
        : std::string();
  }
  std::string formatName(const luciole::FontFacts& facts) {
    std::string outlines = facts.outlines.starts_with("CFF") ? "OpenType " + facts.outlines : facts.outlines;
    switch (facts.container) {
    case FontContainer::Collection:
      return outlines + " collection";
    case FontContainer::Woff:
      return "WOFF, " + outlines;
    case FontContainer::Woff2:
      return "WOFF2, " + outlines;
    case FontContainer::Sfnt:
      break;
    }
    return outlines;
  }
  struct LibraryDone {
    void operator()(FT_Library library) const { FT_Done_FreeType(library); }
  };
  struct FaceDone {
    void operator()(FT_Face face) const { FT_Done_Face(face); }
  };
} // namespace

bool luciole::isFontContentType(std::string_view contentType) { return std::ranges::contains(kFontTypes, contentType); }

std::uint64_t luciole::fontByteCap(int maxFileMiB) {
  return std::min(static_cast<std::uint64_t>(std::max(1, maxFileMiB)) * 1024U * 1024U, kMaxFontFileBytes);
}

std::expected<luciole::FontContainer, std::string> luciole::checkFontFile(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 4) {
    return std::unexpected("This is not a font file.");
  }
  const auto signature = bigEndian(bytes, 0, 4);
  std::optional<FontContainer> container;
  bool intact = false;
  if (sfntVersion(signature)) {
    container = FontContainer::Sfnt;
    intact = sfntIntact(bytes, 0);
  } else if (signature == 0x74746366U) {
    container = FontContainer::Collection;
    intact = collectionIntact(bytes);
  } else if (signature == 0x774F4646U || signature == 0x774F4632U) {
    container = signature == 0x774F4646U ? FontContainer::Woff : FontContainer::Woff2;
    intact = woffIntact(bytes, *container == FontContainer::Woff2);
  }
  if (!container) {
    return std::unexpected("This is not a font file.");
  }
  if (!intact) {
    return std::unexpected("The font file is truncated or damaged.");
  }
  return *container;
}

std::expected<luciole::FontFacts, std::string> luciole::readFontFacts(std::span<const std::uint8_t> bytes) {
  const auto container = checkFontFile(bytes);
  if (!container) {
    return std::unexpected(container.error());
  }
  FT_Library rawLibrary = nullptr;
  if (FT_Init_FreeType(&rawLibrary) != 0) {
    return std::unexpected("FreeType is unavailable.");
  }
  const std::unique_ptr<FT_LibraryRec_, LibraryDone> library(rawLibrary);
  FT_Face rawFace = nullptr;
  const auto opened = FT_New_Memory_Face(library.get(), bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &rawFace);
  if (opened != 0) {
    return std::unexpected(
        *container == FontContainer::Woff2 && opened == FT_Err_Unimplemented_Feature
            ? "This FreeType build cannot read WOFF2 fonts."
            : "The font file cannot be read."
    );
  }
  const std::unique_ptr<FT_FaceRec_, FaceDone> face(rawFace);
  const std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> pattern(
      FcFreeTypeQueryFace(face.get(), reinterpret_cast<const FcChar8*>(""), 0, nullptr), &FcPatternDestroy
  );
  if (!pattern) {
    return std::unexpected("The font file cannot be read.");
  }
  FontFacts facts{
      .container = *container,
      .family = text(pattern.get(), FC_FAMILY),
      .style = text(pattern.get(), FC_STYLE),
      .fullName = text(pattern.get(), FC_FULLNAME),
      .outlines = text(pattern.get(), FC_FONTFORMAT),
      .version = 0,
      .glyphs = face->num_glyphs,
      .faces = face->num_faces,
      .axes = {},
      .monospace = false,
      .color = false,
      .scripts = {},
  };
  int version = 0, spacing = FC_PROPORTIONAL;
  FcBool color = FcFalse;
  FcCharSet* coverage = nullptr;
  facts.version = FcPatternGetInteger(pattern.get(), FC_FONTVERSION, 0, &version) == FcResultMatch ? version : 0;
  (void)FcPatternGetInteger(pattern.get(), FC_SPACING, 0, &spacing);
  facts.monospace = spacing == FC_MONO || spacing == FC_CHARCELL;
  facts.color = FcPatternGetBool(pattern.get(), FC_COLOR, 0, &color) == FcResultMatch && color;
  if (FcPatternGetCharSet(pattern.get(), FC_CHARSET, 0, &coverage) == FcResultMatch) {
    facts.scripts = fontScripts(coverage);
  }
  FT_MM_Var* variations = nullptr;
  if (FT_HAS_MULTIPLE_MASTERS(face.get()) && FT_Get_MM_Var(face.get(), &variations) == 0) {
    for (FT_UInt i = 0; i < std::min<FT_UInt>(variations->num_axis, kMaxAxes); ++i) {
      const auto& axis = variations->axis[i];
      std::string tag(4, ' ');
      for (std::size_t c = 0; c < 4; ++c) {
        const auto letter = static_cast<char>((axis.tag >> (24 - (c * 8))) & 0xFFU);
        tag[c] = letter >= 0x20 && letter < 0x7F ? letter : '?';
      }
      facts.axes.push_back(
          {.tag = tag,
           .minimum = static_cast<double>(axis.minimum) / 65536.0,
           .maximum = static_cast<double>(axis.maximum) / 65536.0}
      );
    }
    FT_Done_MM_Var(library.get(), variations);
  }
  return facts;
}

luciole::MetadataFields luciole::fontFactFields(const FontFacts& facts) {
  MetadataFields fields;
  for (const auto& [label, value] :
       {std::pair{"Family", facts.family}, std::pair{"Style", facts.style}, std::pair{"Full name", facts.fullName}}) {
    if (!value.empty()) {
      fields.push_back({label, value});
    }
  }
  if (facts.version > 0) {
    fields.push_back({"Version", std::format("{:.3f}", static_cast<double>(facts.version) / 65536.0)});
  }
  if (!facts.outlines.empty()) {
    fields.push_back({"Format", formatName(facts)});
  }
  fields.push_back({"Glyphs", std::to_string(facts.glyphs)});
  fields.push_back({"Faces", std::to_string(facts.faces)});
  std::string axes;
  for (const auto& axis : facts.axes) {
    axes += std::format("{}{} {:g}–{:g}", axes.empty() ? "" : ", ", axis.tag, axis.minimum, axis.maximum);
  }
  if (!axes.empty()) {
    fields.push_back({"Axes", axes});
  }
  fields.push_back({"Monospaced", facts.monospace ? "Yes" : "No"});
  fields.push_back({"Color glyphs", facts.color ? "Yes" : "No"});
  std::string scripts;
  for (const auto name : facts.scripts) {
    scripts += std::format("{}{}", scripts.empty() ? "" : ", ", name);
  }
  if (!scripts.empty()) {
    fields.push_back({"Scripts", scripts});
  }
  return fields;
}
