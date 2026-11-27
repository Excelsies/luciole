#include "ui/browser/preview_picture_style.h"

#include "render/scene/image_node.h"
#include "render/scene/rect_node.h"
#include "ui/browser/preview_layout.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/surface_shadow.h"

namespace luciole::preview_picture {
  void styleImage(ImageNode& image, bool framed) {
    image.setRadius(framed ? Style::radiusMd : 0.0F);
    image.setBorder(
        framed ? resolveColorSpec(colorSpecFromRole(ColorRole::OutlineVariant)) : Color{},
        framed ? Style::borderWidth : 0.0F
    );
  }

  void styleShadow(RectNode& shadow, bool framed, float left, float top, float width, float height) {
    const bool visible = framed && Style::popupShadowsEnabled();
    shadow.setVisible(visible);
    if (visible) {
      const auto offset = shadowDirectionOffset(kPreviewShadow.direction);
      shadow.setStyle(surface_shadow::style(kPreviewShadow, 1.0F, {.radius = Radii(Style::radiusMd)}));
      shadow.setPosition(left + static_cast<float>(offset.x), top + static_cast<float>(offset.y));
      shadow.setFrameSize(width, height);
    }
  }

  RoundedRectStyle outline() {
    RoundedRectStyle style;
    style.fill = Color{0.0F, 0.0F, 0.0F, 0.0F};
    style.border = resolveColorSpec(colorSpecFromRole(ColorRole::OutlineVariant));
    style.radius = Radii(Style::radiusMd);
    style.borderWidth = Style::borderWidth;
    return style;
  }
} // namespace luciole::preview_picture
