#include "ui/controls/preview_frame.h"

#include "render/core/render_styles.h"
#include "render/scene/image_node.h"
#include "render/scene/rect_node.h"
#include "ui/browser/preview_picture_style.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

namespace luciole {
  namespace {
    // A picture within this of the box's edge is not cut by it.
    constexpr float kFits = 0.01F;

    RoundedRectStyle maskStyle(ColorRole surface) {
      RoundedRectStyle style;
      style.fill = resolveColorSpec(colorSpecFromRole(surface));
      style.radius = Radii(Style::radiusMd);
      style.invertFill = true;
      return style;
    }

    void place(RectNode& node, const PreviewFrame::Visible& at, float dx = 0, float dy = 0) {
      node.setPosition(at.x + dx, at.y + dy);
      node.setFrameSize(at.width, at.height);
    }
  } // namespace

  PreviewFrame::PreviewFrame(ColorRole surface) : m_surface(surface) {
    m_shadow = static_cast<RectNode*>(addChild(std::make_unique<RectNode>()));
    m_shadow->setVisible(false);
    m_clip = addChild(std::make_unique<Node>());
    m_clip->setClipChildren(true);
    m_image = static_cast<ImageNode*>(m_clip->addChild(std::make_unique<ImageNode>()));
    m_image->setFitMode(ImageFitMode::Contain);
    m_image->setHitTestVisible(false);
    m_mask = static_cast<RectNode*>(addChild(std::make_unique<RectNode>()));
    m_outline = static_cast<RectNode*>(addChild(std::make_unique<RectNode>()));
    for (auto* node : {m_mask, m_outline}) {
      node->setVisible(false);
      node->setHitTestVisible(false);
    }
    m_shadow->setHitTestVisible(false);
    m_paletteConn = paletteChanged().connect([this] { markLayoutDirty(); });
  }

  PreviewFrame::~PreviewFrame() = default;

  void PreviewFrame::setTexture(TextureId id) {
    const bool changes = m_image->textureId().valid() != id.valid();
    m_image->setTextureId(id);
    if (changes) {
      markLayoutDirty();
    }
  }

  void PreviewFrame::setPicture(float x, float y, float width, float height) {
    if (x == m_x && y == m_y && width == m_width && height == m_height) {
      return;
    }
    m_x = x;
    m_y = y;
    m_width = width;
    m_height = height;
    markLayoutDirty();
  }

  LayoutSize PreviewFrame::doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) {
    // Fills a bounded space; unbounded, it keeps the size it has.
    return constraints.constrain(
        {constraints.hasMaxWidth ? constraints.maxWidth : width(),
         constraints.hasMaxHeight ? constraints.maxHeight : height()}
    );
  }

  void PreviewFrame::doLayout(Renderer& /*renderer*/) {
    const float boxWidth = width();
    const float boxHeight = height();
    const float left = std::max(m_x, 0.0F);
    const float top = std::max(m_y, 0.0F);
    const float right = std::min(m_x + m_width, boxWidth);
    const float bottom = std::min(m_y + m_height, boxHeight);
    const bool seen = m_image->textureId().valid() && right > left && bottom > top;
    m_visible = seen ? Visible{left, top, right - left, bottom - top} : Visible{};
    m_cut = seen
        && (m_x < -kFits || m_y < -kFits || m_x + m_width > boxWidth + kFits || m_y + m_height > boxHeight + kFits);
    m_clip->setPosition(0, 0);
    m_clip->setSize(boxWidth, boxHeight);
    m_image->setPosition(m_x, m_y);
    m_image->setFrameSize(m_width, m_height);
    // A fitted picture is rounded and outlined itself; a cut one is square under the overlay.
    const bool rounded = seen && !m_cut;
    preview_picture::styleImage(*m_image, rounded);
    for (auto* node : {m_mask, m_outline}) {
      node->setVisible(m_cut);
    }
    if (m_cut) {
      m_mask->setStyle(maskStyle(m_surface));
      m_outline->setStyle(preview_picture::outline());
      place(*m_mask, m_visible);
      place(*m_outline, m_visible);
    }
    preview_picture::styleShadow(*m_shadow, seen, m_visible.x, m_visible.y, m_visible.width, m_visible.height);
  }
} // namespace luciole
