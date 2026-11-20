#include "ui/controls/media_position_bar.h"

#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <utility>

namespace {
  RoundedRectStyle barStyle(ColorRole role) {
    RoundedRectStyle style;
    style.fill = resolveColorSpec(colorSpecFromRole(role));
    style.border = style.fill;
    style.radius = MediaPositionBar::kTrackHeight / 2;
    return style;
  }
} // namespace

MediaPositionBar::MediaPositionBar() {
  m_track = static_cast<RectNode*>(addChild(std::make_unique<RectNode>()));
  m_fill = static_cast<RectNode*>(addChild(std::make_unique<RectNode>()));
  m_area = static_cast<InputArea*>(addChild(std::make_unique<InputArea>()));
  m_area->setOnPress([this](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT) {
      return;
    }
    if (m_enabled) {
      seekTo(data.localX, luciole::SeekMode::Exact, !data.pressed);
    } else {
      m_held.reset();
    }
  });
  m_area->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_enabled && m_area->pressed()) {
      seekTo(data.localX, luciole::SeekMode::KeyFrame, false);
    }
  });
  // A cancelled drag lands exactly where it last was.
  m_area->setOnCancel([this] {
    if (const auto held = std::exchange(m_held, std::nullopt); held && onSeek) {
      onSeek(*held, luciole::SeekMode::Exact, true);
    }
  });
}

void MediaPositionBar::setFraction(double fraction) {
  m_fraction = std::isfinite(fraction) ? std::clamp(fraction, 0.0, 1.0) : 0.0;
  const float filled = static_cast<float>(m_fraction) * width();
  if (std::abs(filled - m_fill->width()) >= 0.5F) {
    m_fill->setFrameSize(filled, kTrackHeight);
  }
}

void MediaPositionBar::setRowHeight(float height) {
  m_rowHeight = std::isfinite(height) ? std::max(height, 0.0F) : 0.0F;
  markLayoutDirty();
}

LayoutSize MediaPositionBar::doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) {
  return constraints.constrain({.width = 0, .height = m_rowHeight});
}

void MediaPositionBar::doLayout(Renderer& /*renderer*/) {
  const float top = std::max(0.0F, (height() - kTrackHeight) / 2);
  m_track->setPosition(0, top);
  m_track->setFrameSize(width(), kTrackHeight);
  m_track->setStyle(barStyle(ColorRole::OutlineVariant));
  m_fill->setPosition(0, top);
  m_fill->setFrameSize(static_cast<float>(m_fraction) * width(), kTrackHeight);
  m_fill->setStyle(barStyle(ColorRole::Primary));
  m_area->setFrameSize(width(), height());
}

void MediaPositionBar::seekTo(float x, luciole::SeekMode mode, bool done) {
  if (width() <= 0 || !onSeek) {
    return;
  }
  const auto fraction = static_cast<double>(x / width());
  m_held = done ? std::nullopt : std::optional(fraction);
  onSeek(fraction, mode, done);
}
