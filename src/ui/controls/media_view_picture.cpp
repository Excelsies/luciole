#include "render/core/image_decoder.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "ui/browser/preview_layout.h"
#include "ui/controls/media_view.h"
#include "ui/controls/preview_frame.h"

#include <algorithm>
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
  m_frame->setTexture({});
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
  m_frame->setTexture(m_frameTexture.id);
  m_picture->setTextureSize(frame.width, frame.height);
  m_picture->markPaintDirty();
}

void MediaView::showPoster() {
  m_frame->setTexture(m_posterTexture.id);
  m_picture->setTextureSize(m_posterTexture.width, m_posterTexture.height);
}

std::pair<float, float> MediaView::placePicture(Renderer& renderer, float width, float stage) {
  const float inset = std::min(luciole::kPreviewImageInset, std::max(0.0F, std::min(width, stage)) / 4);
  const float boxWidth = std::max(0.0F, width - (2 * inset));
  const float boxHeight = std::max(0.0F, stage - (2 * inset));
  float shownWidth = boxWidth;
  float shownHeight = boxHeight;
  const auto textureWidth = static_cast<float>(m_picture->textureWidth());
  const auto textureHeight = static_cast<float>(m_picture->textureHeight());
  if (textureWidth > 0 && textureHeight > 0) {
    const float fit = std::min(boxWidth / textureWidth, boxHeight / textureHeight);
    shownWidth = textureWidth * fit;
    shownHeight = textureHeight * fit;
  }
  m_frame->setPicture((boxWidth - shownWidth) / 2, (boxHeight - shownHeight) / 2, shownWidth, shownHeight);
  m_frame->arrange(renderer, {.x = inset, .y = inset, .width = boxWidth, .height = boxHeight});
  return {boxWidth, boxHeight};
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
