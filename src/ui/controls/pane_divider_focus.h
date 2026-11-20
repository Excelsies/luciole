#pragma once

#include "render/scene/input_area.h"

#include <memory>

class InputDispatcher;

// Gives the focus back to the area that had it when a pointer press on a PaneDivider ends. The divider takes the focus
// while the pointer is down, so it keeps its Escape and its keys, and the dispatcher clears it on release. Only that
// clear after the pointer's release restores; a clear while the pointer is down forgets the area. Tab from an area to
// a divider, and on to another divider, remembers the first area; a click on the divider then ends there. A divider
// reached with Tab keeps the focus until a click. Feed it every focus change of the dispatcher, after the host has seen
// it.
class PaneDividerFocus {
public:
  explicit PaneDividerFocus(InputDispatcher& dispatcher) : m_dispatcher(dispatcher) {}

  void changed(InputArea* old, InputArea* now);

private:
  // Clears the remembered areas it was holding when the area is destroyed.
  void watch(InputArea* area);
  // The remembered area, if it is alive, in the divider's scene and able to take the focus.
  InputArea* takeBefore(const InputArea& divider);

  InputDispatcher& m_dispatcher;
  InputArea* m_before = nullptr;  // the area that had the focus when a divider took it
  InputArea* m_cleared = nullptr; // the area whose focus was cleared by the change just before this one
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};
