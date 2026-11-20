#include "ui/controls/pane_divider_focus.h"

#include "render/scene/input_dispatcher.h"
#include "ui/controls/pane_divider.h"

#include <utility>

namespace {
  const Node* topOf(const Node* node) {
    while (node != nullptr && node->parent() != nullptr) {
      node = node->parent();
    }
    return node;
  }
} // namespace

void PaneDividerFocus::changed(InputArea* old, InputArea* now) {
  // A press on a divider clears the focus before it gives it to the divider: two changes in a row, with the pointer
  // over the divider. A clear that stands alone is not the area before a divider reached later.
  InputArea* const cleared = std::exchange(m_cleared, nullptr);
  auto* gained = dynamic_cast<PaneDivider*>(now);
  auto* lost = dynamic_cast<PaneDivider*>(old);
  if (now == nullptr && old != nullptr && lost == nullptr) {
    watch(old);
    m_cleared = old;
  }
  if (gained != nullptr && lost == nullptr) {
    m_before = old != nullptr ? old : (m_dispatcher.hoveredArea() == gained ? cleared : nullptr);
    watch(m_before);
    return;
  }
  if (lost == nullptr || gained != nullptr) {
    return;
  }
  const bool released = lost->takeReleased();
  InputArea* const before = takeBefore(*lost);
  if (released && now == nullptr && before != nullptr) {
    m_dispatcher.setFocus(before);
  }
}

void PaneDividerFocus::watch(InputArea* area) {
  if (area != nullptr) {
    area->observeDestruction(m_alive, [this](InputArea* gone) {
      m_before = m_before == gone ? nullptr : m_before;
      m_cleared = m_cleared == gone ? nullptr : m_cleared;
    });
  }
}

InputArea* PaneDividerFocus::takeBefore(const InputArea& divider) {
  InputArea* const before = std::exchange(m_before, nullptr);
  if (before == nullptr || !before->focusable() || !before->enabled() || topOf(before) != topOf(&divider)) {
    return nullptr;
  }
  return before;
}
