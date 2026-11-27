#pragma once

#include "render/core/texture_handle.h"
#include "render/scene/node.h"
#include "ui/signal.h"

class ImageNode;
class RectNode;

namespace luciole {
  // A picture drawn the way the side preview draws one: corners rounded by Style::radiusMd, an OutlineVariant
  // outline and the preview's soft shadow, which shows only while Style::popupShadowsEnabled(). The node's box is
  // where the picture may be seen; the shadow falls outside it, so the owner leaves kPreviewImageInset around the
  // box. A picture that fits the box is rounded itself, as FileIcon does. A picture cut by the box, zoomed or
  // panned, is clipped to the box and the visible part is rounded by a surface-colored mask over its corners with
  // the outline above it, so rounded corners never scroll with the picture.
  class PreviewFrame final : public Node {
  public:
    PreviewFrame();
    ~PreviewFrame() override;
    PreviewFrame(const PreviewFrame&) = delete;
    PreviewFrame& operator=(const PreviewFrame&) = delete;
    PreviewFrame(PreviewFrame&&) = delete;
    PreviewFrame& operator=(PreviewFrame&&) = delete;

    ImageNode& image() noexcept { return *m_image; }
    const ImageNode& image() const noexcept { return *m_image; }
    // Sets the picture's texture. Until it has one nothing is drawn: no shadow, outline or mask around an empty box.
    void setTexture(TextureId id);
    // Where the picture lies, relative to this node's box; it may reach past the box.
    void setPicture(float x, float y, float width, float height);
    // The picture is cut by the box, so the mask and the outline overlay round it.
    bool cut() const noexcept { return m_cut; }
    // The part of the picture inside the box, relative to the box.
    struct Visible {
      float x, y, width, height;
    };
    Visible visible() const noexcept { return m_visible; }

  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
    void doLayout(Renderer& renderer) override;

  private:
    RectNode* m_shadow = nullptr;
    Node* m_clip = nullptr;
    ImageNode* m_image = nullptr;
    RectNode* m_mask = nullptr;
    RectNode* m_outline = nullptr;
    float m_x = 0, m_y = 0, m_width = 0, m_height = 0;
    Visible m_visible{};
    bool m_cut = false;
    Signal<>::ScopedConnection m_paletteConn;
  };
} // namespace luciole
