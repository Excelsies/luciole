#pragma once

#include "metrics_renderer.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/text_node.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_fixture.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// An instrument view over a real input dispatcher and the shared text measurer, with the trees its tests use.
namespace luciole::test::instrument {
  using namespace luciole::workbench;

  inline constexpr float kWidth = 420;
  inline constexpr float kHeight = 380;
  // How long a waiting helper lets worker results arrive before it fails the test; it guards against a hang, so it is
  // long enough for a busy machine.
  inline constexpr std::chrono::milliseconds kWaitLimit{10000};

  inline PartInputs inputsFor(Tree tree, std::uint64_t serial) {
    return {
        .snapshot = {.tree = std::make_shared<const Tree>(std::move(tree)), .serial = serial},
        .settings = {},
        .palette = {}
    };
  }

  // Ids: 0 r, 1 a, 2 b.txt, 3 .hidden, 4 c, 5 x (in a), 6 y.png (in a), 7 z.txt (in c), 8 e (an empty folder).
  // Entries of a folder weigh about the same, so each one gets its own piece.
  inline Tree balancedTree() {
    return workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true, .bytes = 1700, .items = 5, .files = 4},
            {.parent = 0, .name = "a", .folder = true, .bytes = 400, .items = 2, .files = 1},
            {.parent = 0, .name = "b.txt", .bytes = 300},
            {.parent = 0, .name = ".hidden", .bytes = 100},
            {.parent = 0, .name = "c", .folder = true, .bytes = 300, .items = 1, .files = 1},
            {.parent = 1, .name = "x", .folder = true, .bytes = 200},
            {.parent = 1, .name = "y.png", .bytes = 200},
            {.parent = 4, .name = "z.txt", .bytes = 300},
            {.parent = 0, .name = "e", .folder = true, .bytes = 250},
        },
        6000
    );
  }

  // The folder of balancedTree() listed in another order. Ids: 0 r, 1 c, 2 a, 3 b.txt, 4 .hidden, 5 z.txt (in c),
  // 6 x (in a), 7 y.png (in a), 8 e.
  inline Tree reorderedTree() {
    return workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true, .bytes = 1700, .items = 5, .files = 4},
            {.parent = 0, .name = "c", .folder = true, .bytes = 300, .items = 1, .files = 1},
            {.parent = 0, .name = "a", .folder = true, .bytes = 400, .items = 2, .files = 1},
            {.parent = 0, .name = "b.txt", .bytes = 300},
            {.parent = 0, .name = ".hidden", .bytes = 100},
            {.parent = 1, .name = "z.txt", .bytes = 300},
            {.parent = 2, .name = "x", .folder = true, .bytes = 200},
            {.parent = 2, .name = "y.png", .bytes = 200},
            {.parent = 0, .name = "e", .folder = true, .bytes = 250},
        },
        6000
    );
  }

  inline Tree enteredAt(NodeId folder) {
    auto tree = balancedTree();
    tree.root = folder;
    tree.ancestors = parentChain(tree, folder);
    return tree;
  }

  // Lets worker results reach the main context, `turns` times about a millisecond apart.
  inline void pump(int turns) {
    for (int turn = 0; turn < turns; ++turn) {
      while (g_main_context_iteration(nullptr, FALSE) != 0) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  struct Rig {
    ::test::MetricsRenderer renderer;
    AnimationManager animations;
    ::Node root;
    InstrumentView* view = nullptr;
    InputDispatcher dispatcher;
    std::vector<PartRequest> requests;
    std::vector<NodeId> hovers;
    std::size_t frameRequests = 0;
    LayoutRect rect;

    explicit Rig(WorkbenchInstrument kind, float width = kWidth, float height = kHeight) {
      root.setSize(2000, 2000);
      root.setAnimationManager(&animations);
      view = static_cast<InstrumentView*>(root.addChild(std::make_unique<InstrumentView>(kind, [] { return 0U; })));
      view->onRequest = [this](const PartRequest& request) { requests.push_back(request); };
      view->onHover = [this](NodeId node) { hovers.push_back(node); };
      view->onFrameNeeded = [this] { ++frameRequests; };
      dispatcher.setSceneRoot(&root);
      resize(width, height);
    }

    void resize(float width, float height) {
      rect = {.x = 0, .y = 0, .width = width, .height = height};
      view->arrange(renderer, rect);
    }

    void frame(const PartInputs& inputs, bool resizing = false) {
      view->update(inputs, resizing);
      view->arrange(renderer, rect);
    }

    // Frames until `done`, letting worker results reach the main context; fails the test after `limit`.
    template <typename Done>
    void until(const PartInputs& inputs, Done done, std::chrono::milliseconds limit = kWaitLimit) {
      const auto deadline = std::chrono::steady_clock::now() + limit;
      frame(inputs);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        pump(1);
        frame(inputs);
      }
    }

    void show(const PartInputs& inputs, std::chrono::milliseconds limit = kWaitLimit) {
      until(
          inputs,
          [this, &inputs] {
            return !view->shapeView().shapes().empty() && view->shownSnapshot().tree == inputs.snapshot.tree;
          },
          limit
      );
    }

    // Ticks until the transition ends; fails the test after `limit`.
    void finishTransition(std::chrono::milliseconds limit = kWaitLimit) {
      const auto deadline = std::chrono::steady_clock::now() + limit;
      while (animations.hasActive()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        animations.tick(16);
      }
    }

    void click(ShapePoint at, std::uint32_t button) {
      dispatcher.pointerMotion(at.x, at.y, 1);
      dispatcher.pointerButton(at.x, at.y, button, true, 2, 0, false);
      dispatcher.pointerButton(at.x, at.y, button, false, 3, 0, false);
    }

    bool wheel(ShapePoint at, double value) {
      dispatcher.pointerMotion(at.x, at.y, 1);
      return dispatcher.pointerAxis(at.x, at.y, 0, 0, value, value < 0 ? -1 : 1, value < 0 ? -120 : 120, 0);
    }

    void key(std::uint32_t sym, std::uint32_t utf32 = 0) {
      dispatcher.setFocus(&view->shapeView().inputArea());
      dispatcher.keyEvent(sym, utf32, 0, true);
      dispatcher.keyEvent(sym, utf32, 0, false);
    }
  };

  inline const Shape* shapeOf(const ShapeView& view, NodeId node, SceneRole role = SceneRole::Piece) {
    for (const auto& shape : view.shapes()) {
      if (shape.id.node == node && sceneRole(shape.id.role) == role && shape.hittable) {
        return &shape;
      }
    }
    return nullptr;
  }

  // A point well inside a rectangle, a ring sector or a circle.
  inline ShapePoint inside(const Shape& shape) {
    switch (shape.kind) {
    case ShapeKind::Sector: {
      const auto& s = shape.sector;
      const float angle = (s.a0 + s.a1) / 2 * std::numbers::pi_v<float> / 180;
      const float radius = (s.r0 + s.r1) / 2;
      return {.x = s.center.x + radius * std::sin(angle), .y = s.center.y - radius * std::cos(angle)};
    }
    case ShapeKind::Circle:
      return shape.circle.center;
    default:
      return {.x = shape.rect.x + shape.rect.width / 2, .y = shape.rect.y + shape.rect.height / 2};
    }
  }

  // Every shape drawn for a node, marks included.
  inline std::vector<Shape> shapesOf(const ShapeView& view, NodeId node) {
    std::vector<Shape> shapes;
    for (const auto& shape : view.shapes()) {
      if (shape.id.node == node) {
        shapes.push_back(shape);
      }
    }
    return shapes;
  }

  inline ShapePoint pointOf(const ShapeView& view, NodeId node) {
    const auto* shape = shapeOf(view, node);
    TEST_CHECK(shape != nullptr);
    return inside(*shape);
  }

  // Visible labels showing `text`.
  inline std::vector<const TextNode*> visibleLabels(const ShapeView& view, const std::string& text) {
    std::vector<const TextNode*> found;
    for (const auto& child : view.labelLayer().children()) {
      const auto* label = dynamic_cast<const TextNode*>(child.get());
      if (label != nullptr && label->visible() && label->text() == text) {
        found.push_back(label);
      }
    }
    return found;
  }

  // How many times `note` is written: as one visible label, or wrapped over consecutive visible labels.
  inline std::size_t notesWritten(const ShapeView& view, const std::string& note) {
    std::vector<std::string> lines;
    for (const auto& child : view.labelLayer().children()) {
      const auto* label = dynamic_cast<const TextNode*>(child.get());
      if (label != nullptr && label->visible()) {
        lines.push_back(label->text());
      }
    }
    std::size_t written = 0;
    for (std::size_t first = 0; first < lines.size(); ++first) {
      std::string joined;
      for (std::size_t last = first; last < lines.size() && joined.size() < note.size(); ++last) {
        if (!joined.empty()) {
          joined += ' ';
        }
        joined += lines[last];
        if (joined == note) {
          ++written;
        }
      }
    }
    return written;
  }

  inline void sameShapes(std::span<const Shape> actual, std::span<const Shape> expected) {
    TEST_CHECK(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size() && i < expected.size(); ++i) {
      TEST_CHECK(actual[i] == expected[i]);
    }
  }
} // namespace luciole::test::instrument
