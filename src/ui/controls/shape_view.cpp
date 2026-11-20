#include "ui/controls/shape_view.h"

#include "render/animation/motion_service.h"
#include "render/core/renderer.h"
#include "ui/controls/shape_batch_node.h"
#include "ui/style.h"

#include <cmath>
#include <memory>
#include <utility>
#include <wayland-client-protocol.h>

namespace {
  using luciole::workbench::NodeId;
  using luciole::workbench::ShapeId;
} // namespace

ShapeView::ShapeView(std::function<std::uint32_t()> modifiers)
    : m_modifiers(std::move(modifiers)),
      m_batch(static_cast<ShapeBatchNode*>(addChild(std::make_unique<ShapeBatchNode>()))),
      m_labelLayer(addChild(std::make_unique<Node>())),
      m_area(static_cast<InputArea*>(addChild(std::make_unique<InputArea>()))), m_labels(*m_labelLayer) {
  setClipChildren(true);
  m_batch->setHitTestVisible(false);
  m_labelLayer->setHitTestVisible(false);
  m_area->setFocusable(true);
  m_area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_MIDDLE, BTN_RIGHT}));
  m_area->setOnEnter([this](const InputArea::PointerData& pointer) {
    m_pointerInside = true;
    m_pointerX = pointer.localX;
    m_pointerY = pointer.localY;
    setHovered(shapeAt(m_pointerX, m_pointerY));
  });
  m_area->setOnMotion([this](const InputArea::PointerData& pointer) {
    m_pointerX = pointer.localX;
    m_pointerY = pointer.localY;
    if (!dragMotion(m_pointerX, m_pointerY)) {
      setHovered(shapeAt(m_pointerX, m_pointerY));
    }
  });
  m_area->setOnLeave([this] {
    m_pointerInside = false;
    setHovered(std::nullopt);
  });
  m_area->setOnPress([this](const InputArea::PointerData& pointer) {
    if (pointer.pressed) {
      pointerPressed(pointer);
    } else {
      pointerReleased(pointer);
    }
  });
  m_area->setOnCancel([this] {
    m_pressed.reset();
    endDrag();
  });
  m_area->setOnAxisHandler([this](const InputArea::PointerData& pointer) {
    return onWheel
        && onWheel(
               ShapeWheel{
                   .vertical = pointer.axis == WL_POINTER_AXIS_VERTICAL_SCROLL,
                   .delta = pointer.scrollDelta(Style::scrollWheelStep),
                   .x = pointer.localX,
                   .y = pointer.localY,
               }
        );
  });
  m_area->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (onKey) {
      onKey(key);
    }
  });
}

ShapeView::~ShapeView() { stopTransition(); }

void ShapeView::setScene(luciole::workbench::SceneShapes scene, float transitionMs) {
  stopTransition();
  m_scene = std::move(scene);
  const bool animate = std::isfinite(transitionMs)
      && transitionMs > 0
      && animationManager() != nullptr
      && MotionService::instance().enabled();
  if (!animate) {
    m_shown = m_scene.shapes;
    m_labels.show(m_scene.labels);
    rebuildMesh();
    if (m_pointerInside && !m_dragging) {
      setHovered(shapeAt(m_pointerX, m_pointerY));
    }
    return;
  }
  m_tween = luciole::workbench::ShapeTween(m_shown, m_scene.shapes);
  m_labels.beginTransition(m_scene.labels);
  // A completion already collected by the manager can still run after a newer transition began.
  const std::uint64_t transition = m_transition;
  m_animation = animationManager()->animate(
      0.0F, 1.0F, transitionMs, Easing::EaseInOutCubic,
      [this, transition](float progress) {
        if (transition == m_transition) {
          showFrame(progress);
        }
      },
      [this, transition] {
        if (transition == m_transition) {
          finishTransition();
        }
      },
      this
  );
}

void ShapeView::stopTransition() {
  if (m_animation != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_animation);
  }
  ++m_transition;
  m_animation = 0;
  m_tween = {};
}

void ShapeView::showFrame(float progress) {
  m_tween.frame(progress, m_shown);
  rebuildMesh();
  m_labels.setProgress(progress);
}

void ShapeView::finishTransition() {
  m_animation = 0;
  m_tween = {};
  m_shown = m_scene.shapes;
  rebuildMesh();
  m_labels.finish();
  if (m_pointerInside && !m_dragging) {
    setHovered(shapeAt(m_pointerX, m_pointerY));
  }
}

void ShapeView::rebuildMesh() { m_batch->setShapes(m_shown, m_pixelScale); }

void ShapeView::setHighlighted(NodeId node) {
  if (m_highlighted == node) {
    return;
  }
  m_highlighted = node;
  if (onMarksChanged) {
    onMarksChanged();
  }
}

void ShapeView::setCursor(NodeId node) {
  if (m_cursor == node) {
    return;
  }
  m_cursor = node;
  if (onMarksChanged) {
    onMarksChanged();
  }
}

luciole::workbench::ShapeMarks ShapeView::marks() const {
  return {.hovered = m_hovered, .linked = m_highlighted, .cursor = m_cursor};
}

void ShapeView::setTooltipProvider(std::function<TooltipContent(ShapeId)> provider) {
  m_tooltip = std::move(provider);
  if (!m_tooltip) {
    m_area->clearTooltip();
    return;
  }
  m_area->setTooltipProvider([this] -> TooltipContent {
    if (!m_hovered || !m_tooltip) {
      return {};
    }
    return m_tooltip(*m_hovered);
  });
}

std::optional<ShapeId> ShapeView::shapeAt(float x, float y) const {
  const bool inside = x >= 0 && y >= 0 && x <= width() && y <= height();
  if (!inside) {
    return std::nullopt;
  }
  const auto index = luciole::workbench::hitShape(m_shown, {x, y}, 0);
  if (!index) {
    return std::nullopt;
  }
  return m_shown[*index].id;
}

void ShapeView::setHovered(std::optional<ShapeId> id) {
  if (m_hovered == id) {
    return;
  }
  m_hovered = id;
  if (m_tooltip) {
    m_area->requestTooltipRefresh();
  }
  if (onHoverChanged) {
    onHoverChanged(id);
  }
  if (onMarksChanged) {
    onMarksChanged();
  }
}

void ShapeView::pointerPressed(const InputArea::PointerData& pointer) {
  const auto id = shapeAt(pointer.localX, pointer.localY);
  if (pointer.button == BTN_RIGHT) {
    m_pressed.reset();
    if (onContextRequest) {
      onContextRequest(id, pointer.localX, pointer.localY);
    }
    return;
  }
  m_pressed = id;
  m_pressedButton = pointer.button;
  if (pointer.button == BTN_LEFT) {
    m_leftDown = true;
    m_pressX = pointer.localX;
    m_pressY = pointer.localY;
  }
  if (id && onPress) {
    onPress(*id, pointer);
  }
}

bool ShapeView::dragMotion(float x, float y) {
  if (m_dragging) {
    if (onDrag) {
      onDrag({.phase = ShapeDragPhase::Move, .x = x, .y = y});
    }
    return true;
  }
  if (!m_leftDown || !onDrag || std::hypot(x - m_pressX, y - m_pressY) < Style::dragStartThreshold) {
    return false;
  }
  // A press whose drag is not taken stays a press; it is not offered again.
  m_leftDown = false;
  if (!onDrag({.phase = ShapeDragPhase::Begin, .x = m_pressX, .y = m_pressY})) {
    return false;
  }
  m_dragging = true;
  m_pressed.reset();
  setHovered(std::nullopt);
  onDrag({.phase = ShapeDragPhase::Move, .x = x, .y = y});
  return true;
}

void ShapeView::endDrag() {
  m_leftDown = false;
  if (!std::exchange(m_dragging, false)) {
    return;
  }
  if (onDrag) {
    onDrag({.phase = ShapeDragPhase::End, .x = m_pointerX, .y = m_pointerY});
  }
  if (m_pointerInside) {
    setHovered(shapeAt(m_pointerX, m_pointerY));
  }
}

void ShapeView::pointerReleased(const InputArea::PointerData& pointer) {
  if (pointer.button == BTN_LEFT && (m_dragging || m_leftDown)) {
    const bool dragged = m_dragging;
    m_pointerX = pointer.localX;
    m_pointerY = pointer.localY;
    endDrag();
    if (dragged) {
      return;
    }
  }
  const auto pressed = std::exchange(m_pressed, std::nullopt);
  if (!pressed || pointer.button != m_pressedButton) {
    return;
  }
  if (shapeAt(pointer.localX, pointer.localY) == pressed && onActivate) {
    onActivate(*pressed, pointer.button, m_modifiers ? m_modifiers() : 0);
  }
}

void ShapeView::doLayout(Renderer& renderer) {
  const float w = width();
  const float h = height();
  m_batch->setFrameSize(w, h);
  m_labelLayer->setFrameSize(w, h);
  m_area->setFrameSize(w, h);
  const float scale = renderer.renderScale();
  if (std::isfinite(scale) && scale > 0 && scale != m_pixelScale) {
    m_pixelScale = scale;
    rebuildMesh();
  }
  if (w != m_laidWidth || h != m_laidHeight) {
    m_laidWidth = w;
    m_laidHeight = h;
    if (onResize) {
      onResize(w, h);
    }
  }
  m_labels.place(renderer);
}
