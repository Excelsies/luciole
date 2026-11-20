#include "ui/controls/pane_divider.h"

#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "cursor-shape-v1-client-protocol.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
#include <memory>
#include <utility>

namespace {
  constexpr float kLineWidth = 1;
  constexpr float kActiveLineWidth = 3;
} // namespace

PaneDivider::PaneDivider(PaneDividerAxis axis) : m_axis(axis) {
  setAcceptedButtons(buttonMask(BTN_LEFT));
  setCursorShape(
      axis == PaneDividerAxis::Column ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE
                                      : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE
  );
  setFocusable(true);
  setTabStop(true);
  auto line = std::make_unique<Box>();
  line->setHitTestVisible(false);
  m_line = static_cast<Box*>(addChild(std::move(line)));
  setOnPress([this](const PointerData& pointer) {
    if (pointer.button != BTN_LEFT) {
      return;
    }
    if (pointer.pressed) {
      press(pointer);
    } else {
      release();
    }
  });
  setOnMotion([this](const PointerData& pointer) { motion(pointer); });
  setOnCancel([this] { abandon(); });
  setOnKeyDown([this](const KeyData& data) { key(data); });
  setOnEnter([this](const PointerData&) { applyLook(); });
  setOnLeave([this] { applyLook(); });
  setOnFocusGain([this] {
    m_releasedSinceFocus = false;
    applyLook();
  });
  setOnFocusLoss([this] { applyLook(); });
  applyLook();
}

bool PaneDivider::takeReleased() noexcept { return std::exchange(m_releasedSinceFocus, false); }

void PaneDivider::setCallbacks(PaneDividerCallbacks callbacks) { m_callbacks = std::move(callbacks); }

void PaneDivider::setValue(float value) {
  if (!m_dragging && std::isfinite(value)) {
    m_value = value;
  }
}

void PaneDivider::setDraggable(bool draggable) {
  if (m_dragging || draggable == m_draggable) {
    return;
  }
  m_draggable = draggable;
  setEnabled(draggable);
  setHitTestVisible(draggable); // a fixed divider leaves the pointer to what lies under it
  m_lastClick.reset();
  applyLook();
}

void PaneDivider::doLayout(Renderer&) { applyLook(); }

void PaneDivider::press(const PointerData& pointer) {
  if (!m_draggable || m_dragging) {
    return;
  }
  // unsigned time: a clock that went backwards gives a large gap, never a double press
  if (m_lastClick
      && pointer.time - m_lastClick->time <= kDoublePressMs
      && std::hypot(pointer.sceneX - m_lastClick->x, pointer.sceneY - m_lastClick->y) <= kDoublePressDistance) {
    m_lastClick.reset();
    if (m_callbacks.reset) {
      m_callbacks.reset();
    }
    return;
  }
  m_lastClick = Press{.time = pointer.time, .x = pointer.sceneX, .y = pointer.sceneY};
  m_dragging = true;
  m_moved = false;
  m_startValue = m_value;
  m_pressX = pointer.sceneX;
  m_pressY = pointer.sceneY;
  applyLook();
}

void PaneDivider::release() {
  m_releasedSinceFocus = true;
  if (!std::exchange(m_dragging, false)) {
    return;
  }
  applyLook();
  if (!m_moved) {
    return; // a click: m_lastClick stays for a second press
  }
  m_lastClick.reset();
  if (m_callbacks.commit) {
    m_callbacks.commit(m_value);
  }
}

void PaneDivider::motion(const PointerData& pointer) {
  if (!m_dragging) {
    return;
  }
  // motion carries only local coordinates, and the divider moves while it is dragged
  float x = 0;
  float y = 0;
  absolutePosition(this, x, y);
  const float travel = m_axis == PaneDividerAxis::Column
      ? (x + pointer.localX - m_pressX) * (Style::rtl() ? -1.0F : 1.0F)
      : y + pointer.localY - m_pressY;
  if (!std::isfinite(travel) || (!m_moved && std::abs(travel) < Style::dragStartThreshold)) {
    return;
  }
  m_moved = true;
  m_value = m_startValue + travel;
  if (m_callbacks.preview) {
    m_callbacks.preview(m_value);
  }
}

void PaneDivider::key(const KeyData& data) {
  if (!data.pressed || data.preedit || !m_draggable) {
    return;
  }
  if (KeySymbol::isEscape(data.sym)) {
    abandon();
    return;
  }
  if (m_dragging || (data.modifiers != 0 && data.modifiers != KeyMod::Shift)) {
    return;
  }
  if (KeySymbol::isHome(data.sym)) {
    if (data.modifiers == 0 && m_callbacks.reset) {
      m_callbacks.reset();
    }
    return;
  }
  float direction = 0;
  if (m_axis == PaneDividerAxis::Column && (KeySymbol::isLeft(data.sym) || KeySymbol::isRight(data.sym))) {
    direction = KeySymbol::isRight(data.sym) != Style::rtl() ? 1.0F : -1.0F;
  } else if (m_axis == PaneDividerAxis::Row && (KeySymbol::isUp(data.sym) || KeySymbol::isDown(data.sym))) {
    direction = KeySymbol::isDown(data.sym) ? 1.0F : -1.0F;
  }
  if (direction == 0) {
    return;
  }
  m_lastClick.reset();
  m_value += direction * (data.modifiers == KeyMod::Shift ? kLargeKeyStep : kKeyStep);
  if (m_callbacks.commit) {
    m_callbacks.commit(m_value);
  }
}

void PaneDivider::abandon() {
  if (!std::exchange(m_dragging, false)) {
    return;
  }
  m_moved = false;
  m_value = m_startValue;
  m_lastClick.reset();
  applyLook();
  if (m_callbacks.cancel) {
    m_callbacks.cancel(m_startValue);
  }
}

void PaneDivider::applyLook() {
  const bool strong = m_draggable && (m_dragging || focused());
  const bool active = strong || (m_draggable && hovered());
  const ColorRole role = strong ? ColorRole::Primary : active ? ColorRole::OnSurfaceVariant : ColorRole::OutlineVariant;
  const float thickness = active ? kActiveLineWidth : kLineWidth;
  m_line->setFill(colorSpecFromRole(role));
  if (m_axis == PaneDividerAxis::Column) {
    m_line->setPosition((width() - thickness) * 0.5F, 0);
    m_line->setFrameSize(thickness, height());
  } else {
    m_line->setPosition(0, (height() - thickness) * 0.5F);
    m_line->setFrameSize(width(), thickness);
  }
}
