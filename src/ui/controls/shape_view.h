#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "render/scene/tooltip_content.h"
#include "ui/controls/shape_labels.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench/shape_tween.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

class ShapeBatchNode;

struct ShapeWheel {
  bool vertical = true;
  float delta = 0; // logical px, positive down or right
  float x = 0;     // pointer in the view
  float y = 0;
};

enum class ShapeDragPhase : std::uint8_t { Begin, Move, End };

// A left-button drag: Begin at the press point once the pointer moved past the drag threshold, Move at each motion,
// End at the release or a cancel.
struct ShapeDrag {
  ShapeDragPhase phase = ShapeDragPhase::Begin;
  float x = 0; // pointer in the view
  float y = 0;
};

// A scene of shapes and labels: the shapes drawn by one batch node, a bounded pool of text nodes for the labels
// and one input area that picks shapes with the pure hit test. Coordinates are the view's local logical pixels.
class ShapeView final : public Node {
public:
  static constexpr std::size_t kMaxLabels = ShapeLabels::kCap;

  // `modifiers` reports the keyboard modifiers held, read when a shape is activated.
  explicit ShapeView(std::function<std::uint32_t()> modifiers);
  ~ShapeView() override;

  // Shows `scene`. With a positive transition, an animation manager and motion enabled, tweens from what is shown
  // in one animation that changes no node size; otherwise shows it at once.
  void setScene(luciole::workbench::SceneShapes scene, float transitionMs = 0);
  // The shapes drawn now, mid-transition included.
  std::span<const luciole::workbench::Shape> shapes() const noexcept { return m_shown; }
  bool transitioning() const noexcept { return m_animation != 0; }

  // The node another pane points at, and the cursor's node; kNoNode for none.
  void setHighlighted(luciole::workbench::NodeId node);
  void setCursor(luciole::workbench::NodeId node);
  std::optional<luciole::workbench::ShapeId> hovered() const noexcept { return m_hovered; }
  // Hovered shape, highlighted node and cursor, as the scene builders take them.
  luciole::workbench::ShapeMarks marks() const;

  // Tooltip of the hovered shape; none clears it.
  void setTooltipProvider(std::function<TooltipContent(luciole::workbench::ShapeId)> provider);

  InputArea& inputArea() const noexcept { return *m_area; }
  const ShapeBatchNode& batch() const noexcept { return *m_batch; }
  const Node& labelLayer() const noexcept { return *m_labelLayer; }

  std::function<void(std::optional<luciole::workbench::ShapeId>)> onHoverChanged;
  std::function<void(luciole::workbench::ShapeId, const InputArea::PointerData&)> onPress;
  // A left or middle button pressed and released over the same shape.
  std::function<void(luciole::workbench::ShapeId, std::uint32_t button, std::uint32_t modifiers)> onActivate;
  // A right press, over a shape or the background.
  std::function<void(std::optional<luciole::workbench::ShapeId>, float x, float y)> onContextRequest;
  // Returns whether the wheel was used.
  std::function<bool(const ShapeWheel&)> onWheel;
  // Returns whether the drag is taken. A taken Begin takes the press, so no shape is activated, and the hover stays
  // cleared until the End.
  std::function<bool(const ShapeDrag&)> onDrag;
  std::function<void(const InputArea::KeyData&)> onKey;
  // The hovered shape, highlighted node or cursor changed.
  std::function<void()> onMarksChanged;
  // Called from layout when the view's size changes, so the owner can build its scene for the new size.
  std::function<void(float width, float height)> onResize;

protected:
  void doLayout(Renderer& renderer) override;

private:
  std::optional<luciole::workbench::ShapeId> shapeAt(float x, float y) const;
  void setHovered(std::optional<luciole::workbench::ShapeId> id);
  void pointerPressed(const InputArea::PointerData& pointer);
  void pointerReleased(const InputArea::PointerData& pointer);
  // True when the motion belongs to a drag.
  bool dragMotion(float x, float y);
  void endDrag();
  void stopTransition();
  void showFrame(float progress);
  void finishTransition();
  void rebuildMesh();

  std::function<std::uint32_t()> m_modifiers;
  ShapeBatchNode* m_batch = nullptr;
  Node* m_labelLayer = nullptr;
  InputArea* m_area = nullptr;
  ShapeLabels m_labels;
  luciole::workbench::SceneShapes m_scene;
  std::vector<luciole::workbench::Shape> m_shown;
  luciole::workbench::ShapeTween m_tween;
  AnimationManager::Id m_animation = 0;
  std::uint64_t m_transition = 0; // counts stopped transitions; an animation's callbacks run only for its own
  float m_pixelScale = 1;
  float m_laidWidth = -1;
  float m_laidHeight = -1;
  float m_pointerX = 0;
  float m_pointerY = 0;
  bool m_pointerInside = false;
  std::optional<luciole::workbench::ShapeId> m_hovered;
  std::optional<luciole::workbench::ShapeId> m_pressed;
  std::uint32_t m_pressedButton = 0;
  float m_pressX = 0; // where the left button went down
  float m_pressY = 0;
  bool m_leftDown = false; // a left press that may still become a drag
  bool m_dragging = false;
  luciole::workbench::NodeId m_highlighted = luciole::workbench::kNoNode;
  luciole::workbench::NodeId m_cursor = luciole::workbench::kNoNode;
  std::function<TooltipContent(luciole::workbench::ShapeId)> m_tooltip;
};
