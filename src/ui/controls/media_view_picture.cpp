#include "render/core/image_decoder.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "ui/controls/media_view.h"

#include <cstddef>

namespace {
  // The largest poster side uploaded.
  constexpr int kMaxPosterSide = 8192;
} // namespace

void MediaView::setPoster(std::shared_ptr<const LoadedImageFile> poster) {
  if (poster == m_poster) {
    return;
  }
  releasePoster();
  m_poster = std::move(poster);
  m_posterPending = m_poster != nullptr && m_shown;
  scheduleCatchUp();
}

void MediaView::doInvalidateGpuResources(Renderer& /*renderer*/) {
  m_posterTexture = {};
  m_frameTexture = {};
  m_showingFrame = false;
  m_picture->setTextureId({});
  m_posterPending = m_poster != nullptr && m_shown;
  scheduleCatchUp();
}

void MediaView::uploadPoster() {
  m_posterPending = false;
  const auto& poster = *m_poster;
  const bool sized = poster.width > 0
      && poster.height > 0
      && poster.width <= kMaxPosterSide
      && poster.height <= kMaxPosterSide
      && poster.rgba.size() == static_cast<std::size_t>(poster.width) * static_cast<std::size_t>(poster.height) * 4;
  if (!sized) {
    return;
  }
  m_posterTexture = m_renderer->textureManager().loadFromRgba(poster.rgba.data(), poster.width, poster.height);
  if (!m_showingFrame) {
    showPoster();
  }
  refreshControls();
}

// Replaces the texture's pixels in place when the size is unchanged: the frame's one copy.
void MediaView::showFrame(const luciole::MediaFrame& frame) {
  auto& textures = m_renderer->textureManager();
  const bool reuse = m_frameTexture.valid()
      && m_frameTexture.width == frame.width
      && m_frameTexture.height == frame.height
      && textures.updateSubImage(
          m_frameTexture, frame.rgba.data(), 0, 0, frame.width, frame.height, TextureDataFormat::Rgba
      );
  if (!reuse) {
    if (m_frameTexture.valid()) {
      textures.unload(m_frameTexture);
    }
    m_frameTexture = textures.loadFromRgba(frame.rgba.data(), frame.width, frame.height);
    if (!m_frameTexture.valid()) {
      m_showingFrame = false;
      return;
    }
  }
  m_showingFrame = true;
  m_picture->setTextureId(m_frameTexture.id);
  m_picture->setTextureSize(frame.width, frame.height);
  m_picture->markPaintDirty();
}

void MediaView::showPoster() {
  m_picture->setTextureId(m_posterTexture.id);
  m_picture->setTextureSize(m_posterTexture.width, m_posterTexture.height);
}

void MediaView::releaseFrame() {
  if (m_frameTexture.valid() && m_renderer != nullptr) {
    m_renderer->textureManager().unload(m_frameTexture);
  }
  m_frameTexture = {};
  m_showingFrame = false;
  m_askedPixels = {-1, -1};
}

void MediaView::releasePoster() {
  if (m_posterTexture.valid() && m_renderer != nullptr) {
    m_renderer->textureManager().unload(m_posterTexture);
  }
  m_posterTexture = {};
  m_posterPending = false;
  if (!m_showingFrame) {
    showPoster();
  }
}
