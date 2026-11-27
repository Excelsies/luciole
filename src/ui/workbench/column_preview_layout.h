#pragma once

#include "model/thumbnail_options.h"
#include "ui/style.h"
#include "ui/surface_shadow.h"

#include <algorithm>
#include <cmath>

namespace luciole {
  // Room above and below the picture for its drop shadow, which the scroll view would otherwise clip.
  inline constexpr float kColumnImageInset = static_cast<float>(surface_shadow::kBlurRadius);

  // A Workbench preview slot's picture: its box in the slot and the texture side to request for it.
  struct ColumnImageLayout {
    float width, height;
    int pixels;
  };

  // The box takes the slot's width and follows the picture's shape within what the slot's height allows; the
  // texture is the picture fitted into that box, at the output scale, up to the preview limit.
  inline ColumnImageLayout
  columnImageLayout(int sourceWidth, int sourceHeight, float width, float height, float scale) {
    const float budget = std::max(40.0F, width - Style::spaceMd * 2 - Style::scrollbarWidth - Style::scrollbarGap);
    const float maximumHeight = std::max(80.0F, height - 250.0F);
    const bool known = sourceWidth > 0 && sourceHeight > 0;
    const float boxHeight = std::clamp(
        known ? budget * static_cast<float>(sourceHeight) / static_cast<float>(sourceWidth) : 96.0F, 48.0F,
        maximumHeight
    );
    float extent = std::max(budget, maximumHeight);
    if (known) {
      const float fit =
          std::min(budget / static_cast<float>(sourceWidth), boxHeight / static_cast<float>(sourceHeight));
      extent = static_cast<float>(std::max(sourceWidth, sourceHeight)) * fit;
    }
    const int pixels = static_cast<int>(
        std::ceil(std::clamp(extent * std::max(0.01F, scale), 1.0F, static_cast<float>(kMaxPreviewImageSide)))
    );
    return {budget, boxHeight, pixels};
  }
} // namespace luciole
