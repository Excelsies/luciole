#pragma once

#include "render/core/render_styles.h"

class ImageNode;
class RectNode;

namespace luciole::preview_picture {
  // The look of a preview picture, shared by the side preview's FileIcon and the Workbench's pictures: corners
  // rounded by Style::radiusMd, an OutlineVariant outline and the soft shadow of kPreviewShadow, which shows only
  // while Style::popupShadowsEnabled().

  // Rounds and outlines `image`, or clears both when not `framed`.
  void styleImage(ImageNode& image, bool framed);
  // Shows `shadow` behind the rect left, top, width, height when `framed` and shadows are enabled, else hides it.
  void styleShadow(RectNode& shadow, bool framed, float left, float top, float width, float height);
  // A border-only style with the same radius and outline, for drawing the outline over a cut picture.
  RoundedRectStyle outline();
} // namespace luciole::preview_picture
