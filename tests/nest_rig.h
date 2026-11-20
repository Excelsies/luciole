#pragma once

#include "core/timer_manager.h"
#include "instrument_rig.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// Steps and probes shared by the Nest part's view tests.
namespace luciole::test::nest {
  using namespace luciole::workbench;
  using namespace luciole::test::instrument;

  // Frames until `done`, running due timers, letting workers publish and tweens run, then ends the tweens; fails the
  // test after `limit`.
  template <typename Done>
  void tickUntil(Rig& rig, const PartInputs& inputs, Done done, std::chrono::milliseconds limit = kWaitLimit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    rig.frame(inputs);
    while (!done()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      TimerManager::instance().tick();
      pump(1);
      rig.animations.tick(16);
      rig.frame(inputs);
    }
    rig.finishTransition(limit);
    rig.frame(inputs);
  }

  // Runs due timers, lets workers publish and tweens run, over a few frames.
  inline void settle(Rig& rig, const PartInputs& inputs) {
    for (int turn = 0; turn < 20; ++turn) {
      TimerManager::instance().tick();
      pump(1);
      rig.animations.tick(16);
      rig.frame(inputs);
    }
    rig.finishTransition();
    rig.frame(inputs);
  }

  // The open peek card's body, or null.
  inline const Shape* cardOf(const ShapeView& view) {
    const auto found = std::ranges::find_if(view.shapes(), [](const Shape& shape) {
      return sceneRole(shape.id.role) == SceneRole::Head && shape.hittable;
    });
    return found == view.shapes().end() ? nullptr : &*found;
  }

  // Whether the keyboard's ring is drawn for the node.
  inline bool marked(const ShapeView& view, NodeId node) {
    return std::ranges::any_of(view.shapes(), [node](const Shape& shape) {
      return shape.id.node == node && sceneRole(shape.id.role) == SceneRole::Mark;
    });
  }

  inline bool within(const ShapeRect& box, float x, float y) {
    return x >= box.x && y >= box.y && x <= box.x + box.width && y <= box.y + box.height;
  }

  // Rows of `count` files in the folder at row `parent`, named with `prefix`; `names` keeps the names alive.
  inline void addFiles(
      std::vector<WorkbenchRow>& rows, std::deque<std::string>& names, int parent, std::size_t count,
      std::string_view prefix
  ) {
    for (std::size_t i = 0; i < count; ++i) {
      names.push_back(std::format("{}{}.txt", prefix, i));
      rows.push_back({.parent = parent, .name = names.back(), .bytes = 1000.0 + static_cast<double>(i % 17) * 50});
    }
  }

  // r holds `count` files.
  inline Tree manyFiles(std::size_t count) {
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "r", .folder = true, .items = static_cast<double>(count)}};
    std::deque<std::string> names;
    addFiles(rows, names, 0, count, "f");
    return workbenchTree(rows, 6000);
  }
} // namespace luciole::test::nest
