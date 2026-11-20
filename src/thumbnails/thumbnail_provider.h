#pragma once

#include "model/thumbnail_options.h"
#include "render/core/image_decoder.h"

#include <gio/gio.h>
#include <string_view>

namespace luciole {
  inline constexpr std::string_view kVideoThumbnailVersion = "3";
  // Video browsing thumbnails optionally use the system filmstrip helper; previews retain clean frames.
  // `variant` carries the theme colors of a font sheet (fontSheetVariant).
  std::expected<LoadedImageFile, std::string> renderThumbnailProvider(
      int sourceFd, const std::string& mime, int side, const ThumbnailOptions& options, GCancellable* cancellable,
      ThumbnailPurpose purpose = ThumbnailPurpose::Preview, std::string_view variant = {}
  );
} // namespace luciole
