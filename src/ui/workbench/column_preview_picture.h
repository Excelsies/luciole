#pragma once

#include "ui/workbench/column_preview_loads.h"

#include <functional>
#include <memory>

namespace luciole {
  class ThumbnailService;

  // A picture in the column preview's picture view, for Animation and Picture: an image whose frames move plays; a
  // still image, and one whose frames turn out not to move, is decoded by `thumbnails`' preview decoder at the slot's
  // size times the scale, and asked for again when a larger scale needs more pixels. Without `thumbnails`, or when
  // the decoder refuses the file or cannot read it, its status turns Still so the page's own preview shows it.
  std::unique_ptr<ColumnPreviewLoad> startPictureLoad(
      ColumnPreviewView& view, const ColumnPreviewRequest& request, ThumbnailService* thumbnails,
      std::function<void()> frameNeeded
  );
} // namespace luciole
