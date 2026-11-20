#pragma once

#include "render/scene/input_area.h"

#include <cstdint>
#include <functional>
#include <optional>

class Box;
class Renderer;

// Column dividers move along x and resize columns; row dividers move along y and resize rows.
enum class PaneDividerAxis : std::uint8_t { Column, Row };

// Values are positions the divider asks for; the owner clamps them and places the divider again.
struct PaneDividerCallbacks {
  std::function<void(float)> preview; // a drag moved
  std::function<void(float)> commit;  // a drag ended, once, or a key stepped
  std::function<void(float)> cancel;  // a drag was abandoned: the value it started from
  std::function<void()> reset;        // a double press or Home: back to the default
};

// A 6 px handle around a 1 px line that resizes whatever its owner says. A column divider mirrors its travel and
// its Left and Right keys in right-to-left layouts.
class PaneDivider final : public InputArea {
public:
  static constexpr float kHitSize = 6;
  static constexpr float kKeyStep = 16;
  static constexpr float kLargeKeyStep = 64; // with Shift
  static constexpr std::uint32_t kDoublePressMs = 400;
  static constexpr float kDoublePressDistance = 6;

  explicit PaneDivider(PaneDividerAxis axis);

  void setCallbacks(PaneDividerCallbacks callbacks);
  // Ignored while dragging, and for a value that is not finite.
  void setValue(float value);
  // A fixed divider is only a line: no cursor, no focus, no drag, and presses reach what lies under it. Ignored
  // while dragging.
  void setDraggable(bool draggable);

  PaneDividerAxis axis() const noexcept { return m_axis; }
  float value() const noexcept { return m_value; }
  bool draggable() const noexcept { return m_draggable; }
  bool dragging() const noexcept { return m_dragging; }
  // Whether the pointer pressed it and let go since it took the focus; clears the answer.
  bool takeReleased() noexcept;

private:
  struct Press {
    std::uint32_t time = 0;
    float x = 0;
    float y = 0;
  };

  void doLayout(Renderer& renderer) override;
  void press(const PointerData& pointer);
  void release();
  void motion(const PointerData& pointer);
  void key(const KeyData& data);
  void abandon();
  void applyLook();

  PaneDividerAxis m_axis;
  PaneDividerCallbacks m_callbacks;
  Box* m_line = nullptr;
  float m_value = 0;
  float m_startValue = 0;
  float m_pressX = 0;
  float m_pressY = 0;
  bool m_draggable = true;
  bool m_dragging = false;
  bool m_moved = false;
  bool m_releasedSinceFocus = false;
  std::optional<Press> m_lastClick; // a press released without a drag, the first half of a double press
};
