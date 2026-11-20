#pragma once

#include "instrument_rig.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <linux/input-event-codes.h>
#include <vector>

// What the Atlas view tests share: the circle of a node, the controls over the map, a press and a release, and frames
// until the view rests.
namespace luciole::test::atlas_view {
  using namespace luciole::workbench;
  using luciole::test::instrument::Rig;

  // The current folder's own circle, or the circle that stands for `node`.
  inline float radiusOf(const ShapeView& view, NodeId node) {
    const auto* shape = instrument::shapeOf(view, node);
    TEST_CHECK(shape != nullptr && shape->kind == ShapeKind::Circle);
    return shape == nullptr ? 0.0F : shape->circle.radius;
  }

  inline ShapePoint centerOf(const ShapeView& view, NodeId node) {
    const auto* shape = instrument::shapeOf(view, node);
    TEST_CHECK(shape != nullptr && shape->kind == ShapeKind::Circle);
    return shape == nullptr ? ShapePoint{} : shape->circle.center;
  }

  inline const Shape* hittable(const ShapeView& view, SceneRole role, NodeId node, std::size_t part = 0) {
    for (const auto& shape : view.shapes()) {
      if (shape.hittable && shape.id.node == node && shape.id.role == shapeRole(role, part)) {
        return &shape;
      }
    }
    return nullptr;
  }

  // Frames until nothing moves: a hundred frames in a row, over at least 400 ms, that ask for no frame and change no
  // shape. Worker layouts may land late on a busy machine, so a short quiet spell is not enough.
  inline void settle(Rig& rig, const PartInputs& inputs) {
    std::vector<Shape> last;
    std::size_t asked = rig.frameRequests;
    int quiet = 0;
    auto since = std::chrono::steady_clock::now();
    rig.until(inputs, [&] {
      const auto shapes = rig.view->shapeView().shapes();
      const bool same = rig.frameRequests == asked && std::ranges::equal(shapes, last);
      quiet = same ? quiet + 1 : 0;
      if (!same) {
        since = std::chrono::steady_clock::now();
      }
      asked = rig.frameRequests;
      last.assign(shapes.begin(), shapes.end());
      return quiet >= 100 && std::chrono::steady_clock::now() - since >= std::chrono::milliseconds(400);
    });
  }

  inline void press(Rig& rig, ShapePoint at) {
    rig.dispatcher.pointerMotion(at.x, at.y, 1);
    rig.dispatcher.pointerButton(at.x, at.y, BTN_LEFT, true, 2, 0, false);
  }

  inline void release(Rig& rig, ShapePoint at) {
    rig.dispatcher.pointerButton(at.x, at.y, BTN_LEFT, false, 3, 0, false);
  }
} // namespace luciole::test::atlas_view
