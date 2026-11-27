#include "ui/controls/animated_image_view.h"

#include "imaging/image_limits.h"
#include "render/core/image_decoder.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/input_area.h"
#include "ui/browser/preview_layout.h"
#include "ui/controls/preview_frame.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <utility>
#include <wayland-client-protocol.h>

namespace {
  using luciole::AnimationState;

  // Waits this long after a resize before decoding at the new size.
  constexpr std::chrono::milliseconds kResizeSettle{150};
  // Frames skipped at most in one step to catch up with the clock; further behind, the timeline restarts.
  constexpr std::size_t kMaxCatchUp = 64;
  // Logical pixels a press travels before it pans instead of clicking.
  constexpr float kDragSlop = 3;

  bool validStill(const LoadedImageFile& pixels) {
    return pixels.width > 0
        && pixels.height > 0
        && pixels.width <= luciole::kMaxImageSide
        && pixels.height <= luciole::kMaxImageSide
        && pixels.rgba.size() == static_cast<std::size_t>(pixels.width) * static_cast<std::size_t>(pixels.height) * 4;
  }
} // namespace

AnimatedImageView::AnimatedImageView(std::function<Clock::time_point()> clock, luciole::AnimationLimits limits)
    : m_clock(std::move(clock)), m_limits(limits) {
  setClipChildren(true);
  m_frame = static_cast<luciole::PreviewFrame*>(addChild(std::make_unique<luciole::PreviewFrame>()));
  m_image = &m_frame->image();
  m_area = static_cast<InputArea*>(addChild(std::make_unique<InputArea>()));
  m_area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT}));
  m_area->setOnPress([this](const InputArea::PointerData& pointer) {
    press(pointer.localX, pointer.localY, pointer.pressed);
  });
  m_area->setOnMotion([this](const InputArea::PointerData& pointer) { drag(pointer.localX, pointer.localY); });
  m_area->setOnCancel([this] { m_dragging = false; });
  m_area->setOnClick([this](const InputArea::PointerData&) {
    if (!m_dragged) {
      togglePause();
    }
  });
  m_area->setOnAxisHandler([this](const InputArea::PointerData& pointer) {
    const float delta = pointer.scrollDelta(Style::scrollWheelStep);
    if (pointer.axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
      scrollTo(m_scrollX, m_scrollY + delta);
      return maxScrollY() > 0;
    }
    scrollTo(m_scrollX + delta, m_scrollY);
    return maxScrollX() > 0;
  });
}

AnimatedImageView::~AnimatedImageView() { release(); }

void AnimatedImageView::setFile(std::optional<luciole::Location> file) {
  if (file == m_file && m_still == nullptr) {
    return;
  }
  release();
  m_still.reset();
  m_file = std::move(file);
  m_reported = AnimationState::Opening;
  m_sourceWidth = 0;
  m_sourceHeight = 0;
  markLayoutDirty();
}

void AnimatedImageView::setStill(std::shared_ptr<const LoadedImageFile> pixels) {
  release();
  m_file.reset();
  m_still = pixels != nullptr && validStill(*pixels) ? std::move(pixels) : nullptr;
  m_sourceWidth = m_still == nullptr ? 0 : m_still->sourceWidth > 0 ? m_still->sourceWidth : m_still->width;
  m_sourceHeight = m_still == nullptr ? 0 : m_still->sourceHeight > 0 ? m_still->sourceHeight : m_still->height;
  markLayoutDirty();
}

void AnimatedImageView::setShown(bool shown) {
  if (shown == m_shown) {
    return;
  }
  m_shown = shown;
  if (!shown) {
    release();
  }
  markLayoutDirty();
}

void AnimatedImageView::setPreviewScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0) {
    return;
  }
  const float clamped = std::clamp(scale, kMinScale, kMaxScale);
  if (clamped == m_scale) {
    return;
  }
  const auto [shownWidth, shownHeight] = shownSize();
  if (shownWidth > 0 && shownHeight > 0 && !m_anchor) {
    // Where the center falls on the picture, as the scroll offsets place it now.
    const auto [boxWidth, boxHeight] = boxSize();
    const float left = shownWidth <= boxWidth ? (boxWidth - shownWidth) / 2 : -m_scrollX;
    const float top = shownHeight <= boxHeight ? (boxHeight - shownHeight) / 2 : -m_scrollY;
    m_anchor = std::pair((boxWidth / 2 - left) / shownWidth, (boxHeight / 2 - top) / shownHeight);
  }
  m_scale = clamped;
  markLayoutDirty();
}

void AnimatedImageView::scrollTo(float x, float y) {
  const float clampedX = std::isfinite(x) ? std::clamp(x, 0.0F, maxScrollX()) : m_scrollX;
  const float clampedY = std::isfinite(y) ? std::clamp(y, 0.0F, maxScrollY()) : m_scrollY;
  if (clampedX != m_scrollX || clampedY != m_scrollY) {
    m_scrollX = clampedX;
    m_scrollY = clampedY;
    markLayoutDirty();
  }
}

float AnimatedImageView::maxScrollX() const noexcept { return std::max(0.0F, shownSize().first - boxSize().first); }

float AnimatedImageView::maxScrollY() const noexcept { return std::max(0.0F, shownSize().second - boxSize().second); }

luciole::AnimationState AnimatedImageView::state() const noexcept {
  return m_frames != nullptr ? m_frames->state() : AnimationState::Opening;
}

std::optional<std::size_t> AnimatedImageView::frameIndex() const noexcept {
  return m_current != nullptr ? std::optional(m_current->index) : std::nullopt;
}

float AnimatedImageView::inset() const noexcept {
  return std::min(luciole::kPreviewImageInset, std::max(0.0F, std::min(width(), height())) / 4);
}

std::pair<float, float> AnimatedImageView::boxSize() const noexcept {
  return {std::max(0.0F, width() - (2 * inset())), std::max(0.0F, height() - (2 * inset()))};
}

std::pair<float, float> AnimatedImageView::shownSize() const {
  const auto [boxWidth, boxHeight] = boxSize();
  if (m_sourceWidth <= 0 || m_sourceHeight <= 0 || boxWidth <= 0 || boxHeight <= 0) {
    return {boxWidth, boxHeight};
  }
  const auto sourceWidth = static_cast<float>(m_sourceWidth);
  const auto sourceHeight = static_cast<float>(m_sourceHeight);
  const float fit = std::min({1.0F, boxWidth / sourceWidth, boxHeight / sourceHeight});
  return {sourceWidth * fit * m_scale, sourceHeight * fit * m_scale};
}

// Centered while it fits, else placed by the scroll offsets; a pending anchor sets those offsets first.
void AnimatedImageView::doLayout(Renderer& renderer) {
  m_renderer = &renderer;
  const auto [shownWidth, shownHeight] = shownSize();
  const auto [boxWidth, boxHeight] = boxSize();
  if (const auto anchor = std::exchange(m_anchor, std::nullopt)) {
    m_scrollX = (anchor->first * shownWidth) - (boxWidth / 2);
    m_scrollY = (anchor->second * shownHeight) - (boxHeight / 2);
  }
  m_scrollX = std::clamp(m_scrollX, 0.0F, maxScrollX());
  m_scrollY = std::clamp(m_scrollY, 0.0F, maxScrollY());
  // The frame's box leaves room around it for the picture's shadow; the picture lies in it, clipped at its edge.
  m_frame->setPicture(
      shownWidth <= boxWidth ? (boxWidth - shownWidth) / 2 : -m_scrollX,
      shownHeight <= boxHeight ? (boxHeight - shownHeight) / 2 : -m_scrollY, shownWidth, shownHeight
  );
  m_frame->arrange(renderer, {.x = inset(), .y = inset(), .width = boxWidth, .height = boxHeight});
  m_area->arrange(renderer, {.x = 0, .y = 0, .width = width(), .height = height()});
  if (m_still != nullptr) {
    if (onScreen()) {
      showStill();
    }
    return;
  }
  const float scale = renderer.renderScale() * m_scale;
  m_boxWidth = std::clamp(static_cast<int>(std::lround(boxWidth * scale)), 0, luciole::kMaxImageSide);
  m_boxHeight = std::clamp(static_cast<int>(std::lround(boxHeight * scale)), 0, luciole::kMaxImageSide);
  if (!m_file || m_boxWidth == 0 || m_boxHeight == 0 || !onScreen()) {
    return;
  }
  if (m_frames == nullptr) {
    m_startTimer.start(std::chrono::milliseconds{0}, [this] { start(); });
  } else if (needsRestart()) {
    m_startTimer.start(kResizeSettle, [this] { start(); });
  }
}

void AnimatedImageView::doInvalidateGpuResources(Renderer& renderer) {
  m_renderer = &renderer;
  m_texture = {};
  m_image->setTextureId({});
  if (m_current != nullptr) {
    show(std::exchange(m_current, nullptr));
  } else if (m_still != nullptr && onScreen()) {
    showStill();
  }
}

// Decodes for the laid-out box; frames on screen stay until the new first frame arrives.
void AnimatedImageView::start() {
  if (!m_file || !onScreen()) {
    return;
  }
  m_frameTimer.stop();
  m_stalled = false;
  m_frames = std::make_unique<luciole::AnimationFrames>(
      *m_file, m_boxWidth, m_boxHeight, [this] { framesChanged(); }, m_limits
  );
}

// Frames already decoded at a size well matched to what the box and the budget allow are kept.
bool AnimatedImageView::needsRestart() const {
  if (m_current == nullptr || m_sourceWidth <= 0 || m_sourceHeight <= 0) {
    return false;
  }
  const double wanted =
      luciole::animationFrameSize(m_sourceWidth, m_sourceHeight, m_boxWidth, m_boxHeight, m_limits.budgetBytes).first;
  const double decoded = m_current->width;
  return wanted > decoded * 1.25 || wanted * 2 < decoded;
}

void AnimatedImageView::release() {
  m_startTimer.stop();
  m_frames.reset();
  m_paused = false;
  m_dragging = false;
  clearFrame();
}

void AnimatedImageView::clearFrame() {
  m_frameTimer.stop();
  m_current.reset();
  m_stalled = false;
  if (m_texture.valid() && m_renderer != nullptr) {
    m_renderer->textureManager().unload(m_texture);
  }
  m_texture = {};
  m_image->setTextureId({});
}

// A still or failed file keeps its finished frames object, so state() reports it and layout does not reopen it.
void AnimatedImageView::framesChanged() {
  const auto state = m_frames->state();
  if (m_frames->sourceWidth() != m_sourceWidth || m_frames->sourceHeight() != m_sourceHeight) {
    m_sourceWidth = m_frames->sourceWidth();
    m_sourceHeight = m_frames->sourceHeight();
    markLayoutDirty();
  }
  if (state == AnimationState::Animated && !m_paused && (m_stalled || !m_frameTimer.active())) {
    m_due = m_clock();
    advance();
  } else if (state != AnimationState::Animated) {
    clearFrame();
  }
  if (state != m_reported) {
    m_reported = state;
    // Calls a copy and touches nothing after it, so the owner may destroy this view from the callback.
    if (auto changed = onStateChanged) {
      changed(state);
    }
  }
}

// Shows the frame the clock says, then waits for the next one. An ancestor that hides the view releases it.
void AnimatedImageView::advance() {
  if (!onScreen()) {
    release();
    return;
  }
  if (m_frames == nullptr || m_paused) {
    return;
  }
  const auto now = m_clock();
  std::shared_ptr<const luciole::AnimationFrame> latest;
  m_stalled = false;
  for (std::size_t step = 0; now >= m_due && step < kMaxCatchUp; ++step) {
    auto frame = m_frames->next();
    if (frame == nullptr) {
      m_stalled = !m_frames->finished();
      break;
    }
    m_due += frame->delay;
    latest = std::move(frame);
  }
  if (now >= m_due && latest != nullptr && !m_stalled) {
    m_due = now + latest->delay;
  }
  if (latest != nullptr) {
    show(std::move(latest));
  }
  if (!m_stalled && !m_frames->finished()) {
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(m_due - now);
    m_frameTimer.start(std::max(wait, std::chrono::milliseconds{1}), [this] { advance(); });
  }
}

// Replaces the texture's pixels, in place when the size is unchanged.
void AnimatedImageView::show(std::shared_ptr<const luciole::AnimationFrame> frame) {
  if (m_renderer == nullptr) {
    return;
  }
  auto& textures = m_renderer->textureManager();
  const auto* pixels = frame->rgba.data();
  const bool same = m_texture.valid() && m_texture.width == frame->width && m_texture.height == frame->height;
  if (!same
      || !textures.updateSubImage(m_texture, pixels, 0, 0, frame->width, frame->height, TextureDataFormat::Rgba)) {
    textures.unload(m_texture);
    m_texture = textures.loadFromPixels(pixels, frame->width, frame->height, TextureDataFormat::Rgba);
  }
  m_current = std::move(frame);
  m_image->setTextureId(m_texture.id);
  m_image->setTextureSize(m_current->width, m_current->height);
  m_image->markPaintDirty();
}

// Uploads the still once; it stays until the view is hidden, its picture changes or the GPU resources go.
void AnimatedImageView::showStill() {
  if (m_renderer == nullptr || m_still == nullptr || m_texture.valid()) {
    return;
  }
  m_texture = m_renderer->textureManager().loadFromPixels(
      m_still->rgba.data(), m_still->width, m_still->height, TextureDataFormat::Rgba
  );
  m_image->setTextureId(m_texture.id);
  m_image->setTextureSize(m_still->width, m_still->height);
  m_image->markPaintDirty();
}

void AnimatedImageView::togglePause() {
  if (m_current == nullptr) {
    return;
  }
  const auto now = m_clock();
  m_paused = !m_paused;
  if (m_paused) {
    m_pausedAt = now;
    m_frameTimer.stop();
    return;
  }
  m_due += now - m_pausedAt;
  advance();
}

void AnimatedImageView::press(float x, float y, bool pressed) {
  m_dragging = pressed;
  if (pressed) {
    m_dragged = false;
    m_dragX = x;
    m_dragY = y;
  }
}

// Pans once the press has travelled past the slop; a picture that fits does not move.
void AnimatedImageView::drag(float x, float y) {
  if (!m_dragging) {
    return;
  }
  const float dx = x - m_dragX;
  const float dy = y - m_dragY;
  if (!m_dragged && std::hypot(dx, dy) < kDragSlop) {
    return;
  }
  m_dragged = true;
  m_dragX = x;
  m_dragY = y;
  scrollTo(m_scrollX - dx, m_scrollY - dy);
}

bool AnimatedImageView::onScreen() const {
  if (!m_shown) {
    return false;
  }
  // Ends at the scene root.
  for (const Node* node = this; node != nullptr; node = node->parent()) {
    if (!node->visible()) {
      return false;
    }
  }
  return true;
}
