#include "core/input/key_modifiers.h"
#include "cursor-shape-v1-client-protocol.h"
#include "editing/text_motion.h"
#include "formats/text_lines.h"
#include "imaging/text_line_strip.h"
#include "ui/controls/code_editor_view.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <wayland-client-protocol.h>

namespace {
  namespace editing = luciole::editing;

  constexpr auto kMultiClickTime = std::chrono::milliseconds(400);
  constexpr float kMultiClickDistance = 6.0F;
} // namespace

void CodeEditorView::wirePointer() {
  m_area->setFocusable(true);
  m_area->setTextInputClient(this);
  m_area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
  m_area->setOnPress([this](const InputArea::PointerData& pointer) {
    if (pointer.pressed) {
      press(pointer);
    }
  });
  m_area->setOnMotion([this](const InputArea::PointerData& pointer) {
    if (m_area->pressed()) {
      drag(pointer);
    }
  });
  m_area->setOnAxisHandler([this](const InputArea::PointerData& pointer) { return wheel(pointer); });
  m_area->setOnKeyDown([this](const InputArea::KeyData& data) { (void)key(data); });
  m_area->setOnFocusGain([this] {
    restartBlink();
    markLayoutDirty();
  });
  m_area->setOnFocusLoss([this] {
    ++m_focusLosses;
    stopBlink();
    setComposing({}, 0);
    markLayoutDirty();
  });
}

editing::TextPosition CodeEditorView::positionAt(float x, float y) {
  auto& drawer = strip();
  const double s = deviceScale();
  const double textX = (x * s) + std::round(m_scrollX * s) - margin();
  const double textY = (y * s) + std::round(m_scrollY * s) - margin();
  const auto last = static_cast<double>(m_buffer.lineCount() - 1);
  const auto line = static_cast<std::size_t>(std::clamp(std::floor(textY / m_lineHeight), 0.0, last));
  const auto column = drawer.columnAt(styledLine(line), textX);
  return {.line = line, .byte = luciole::formats::tokenize::prefixBytes(m_buffer.line(line), column)};
}

void CodeEditorView::press(const InputArea::PointerData& pointer) {
  if (pointer.button != BTN_LEFT || !visible()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const bool again = m_clicks > 0
      && now - m_lastPress <= kMultiClickTime
      && std::abs(pointer.localX - m_lastPressX) <= kMultiClickDistance
      && std::abs(pointer.localY - m_lastPressY) <= kMultiClickDistance;
  m_clicks = again ? (m_clicks % 3) + 1 : 1;
  m_lastPress = now;
  m_lastPressX = pointer.localX;
  m_lastPressY = pointer.localY;
  setComposing({}, 0);
  m_goalX.reset();
  const auto position = positionAt(pointer.localX, pointer.localY);
  if (m_clicks == 2) {
    m_granularity = Granularity::Word;
    m_pivot = editing::wordRange(m_buffer, position);
    m_buffer.setSelection(m_pivot);
  } else if (m_clicks == 3) {
    m_granularity = Granularity::Line;
    m_pivot = editing::lineRange(m_buffer, position.line);
    m_buffer.setSelection(m_pivot);
  } else {
    m_granularity = Granularity::Character;
    const bool shift = modifiers && (modifiers() & KeyMod::Shift) != 0;
    const auto anchor = shift ? m_buffer.selection().anchor : position;
    m_pivot = {.anchor = anchor, .cursor = anchor};
    m_buffer.setSelection({.anchor = anchor, .cursor = position});
  }
  caretMoved();
}

void CodeEditorView::drag(const InputArea::PointerData& pointer) {
  if (!visible()) {
    return;
  }
  // A point past an edge lies on a line or column out of view, and revealing the caret there scrolls.
  extendTo(positionAt(pointer.localX, pointer.localY));
}

void CodeEditorView::extendTo(editing::TextPosition position) {
  editing::TextSelection range{.anchor = position, .cursor = position};
  if (m_granularity == Granularity::Word) {
    range = editing::wordRange(m_buffer, position);
  } else if (m_granularity == Granularity::Line) {
    range = editing::lineRange(m_buffer, position.line);
  }
  const bool before = position < m_pivot.start();
  const auto selection = before ? editing::TextSelection{.anchor = m_pivot.end(), .cursor = range.start()}
                                : editing::TextSelection{.anchor = m_pivot.start(), .cursor = range.end()};
  const auto wanted = m_granularity == Granularity::Character
      ? editing::TextSelection{.anchor = m_pivot.anchor, .cursor = position}
      : selection;
  if (wanted == m_buffer.selection()) {
    return;
  }
  m_buffer.setSelection(wanted);
  caretMoved();
}

bool CodeEditorView::wheel(const InputArea::PointerData& pointer) {
  if (!visible()) {
    return false;
  }
  const float delta = pointer.scrollDelta(Style::scrollWheelStep);
  const bool shift = modifiers && (modifiers() & KeyMod::Shift) != 0;
  const bool sideways =
      pointer.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL || (shift && pointer.axis == WL_POINTER_AXIS_VERTICAL_SCROLL);
  if (sideways) {
    scrollTo(m_scrollX + delta, m_scrollY);
    return maxScrollX() > 0;
  }
  scrollTo(m_scrollX, m_scrollY + delta);
  return maxScrollY() > 0;
}
