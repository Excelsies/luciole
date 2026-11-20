#pragma once

#include "render/core/image_decoder.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luciole {
  inline constexpr int kMaxFontSheetSide = 1024;

  // 0xRRGGBB colors of the sheet.
  struct FontSheetColors {
    std::uint32_t text = 0;
    std::uint32_t surface = 0;
    bool operator==(const FontSheetColors&) const = default;
  };

  // The theme's text color on its surface color for a font type, as "rrggbb:rrggbb"; empty for other types.
  // Preview images keep it in their identity, so a theme change renders a new sheet.
  std::string fontSheetVariant(std::string_view contentType);
  std::optional<FontSheetColors> parseFontSheetVariant(std::string_view variant);

  // Draws the sample sheet `side` pixels wide and at most as tall, using only the file's own faces through a private
  // fontconfig configuration. The file is untrusted: run it only in a resource-limited helper.
  std::expected<LoadedImageFile, std::string>
  renderFontSheet(const std::filesystem::path& file, int side, FontSheetColors colors);

  // Helper command line for renderThumbnailProvider; the source is descriptor 3.
  std::vector<std::string>
  fontPreviewArguments(int side, std::uint64_t maxBytes, std::string_view variant, const std::filesystem::path& output);
  // Private helper protocol: side, byte cap, variant, output path. Answers like the thumbnail worker.
  int runFontPreviewWorker(std::span<const std::string_view> arguments);
} // namespace luciole
