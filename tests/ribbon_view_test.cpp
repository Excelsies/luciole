#include "metrics_renderer.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/text_node.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "ui/workbench/ribbon_view.h"
#include "workbench/flame_bands.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <linux/input-event-codes.h>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using ::test::MetricsRenderer;
  namespace fixture = luciole::test;

  constexpr float kWidth = 700;
  constexpr float kHeight = 280;
  constexpr std::array<WorkbenchRibbon, 3> kKinds{
      WorkbenchRibbon::Flame, WorkbenchRibbon::Flow, WorkbenchRibbon::Ladder
  };

  PartInputs inputsFor(Tree tree, std::uint64_t serial) {
    return {
        .snapshot = {.tree = std::make_shared<const Tree>(std::move(tree)), .serial = serial},
        .settings = {},
        .palette = {}
    };
  }

  Tree enteredAt(NodeId folder) {
    auto tree = fixture::referenceTree();
    tree.root = folder;
    tree.ancestors = parentChain(tree, folder);
    return tree;
  }

  // The flame graph and the flow diagram show the folders ahead of the root, the ladder the entries of x's
  // ancestors: a (1) is a folder each one draws, y.png (6) a file the ladder draws.
  PartInputs inputsFor(WorkbenchRibbon kind, std::uint64_t serial = 1) {
    return inputsFor(kind == WorkbenchRibbon::Ladder ? enteredAt(5) : fixture::referenceTree(), serial);
  }

  // Folders f0 to f17, each inside the one before, entered at f17.
  Tree chainTree() {
    std::vector<fixture::WorkbenchRow> rows;
    rows.reserve(18);
    std::vector<std::string> names;
    names.reserve(18);
    for (int level = 0; level < 18; ++level) {
      names.push_back(std::format("f{}", level));
    }
    for (int level = 0; level < 18; ++level) {
      rows.push_back(
          {.parent = level - 1,
           .name = names[static_cast<std::size_t>(level)],
           .folder = true,
           .bytes = 100,
           .items = 1}
      );
    }
    auto tree = fixture::workbenchTree(rows, 1000);
    tree.root = 17;
    tree.ancestors = parentChain(tree, 17);
    return tree;
  }

  struct Rig {
    MetricsRenderer renderer;
    AnimationManager animations;
    ::Node root;
    RibbonView* view = nullptr;
    InputDispatcher dispatcher;
    std::vector<PartRequest> requests;
    std::vector<NodeId> hovers;
    LayoutRect rect;

    explicit Rig(WorkbenchRibbon kind, float width = kWidth, float height = kHeight) {
      root.setSize(2000, 2000);
      root.setAnimationManager(&animations);
      view = static_cast<RibbonView*>(root.addChild(std::make_unique<RibbonView>(kind, [] { return 0U; })));
      view->onRequest = [this](const PartRequest& request) { requests.push_back(request); };
      view->onHover = [this](NodeId node) { hovers.push_back(node); };
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

    void finishTransition() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
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
  };

  const Shape* shapeOf(const ShapeView& view, NodeId node, SceneRole role = SceneRole::Piece) {
    for (const auto& shape : view.shapes()) {
      if (shape.id.node == node && sceneRole(shape.id.role) == role && shape.hittable) {
        return &shape;
      }
    }
    return nullptr;
  }

  // Every shape drawn for a node, marks included.
  std::vector<Shape> shapesOf(const ShapeView& view, NodeId node) {
    std::vector<Shape> shapes;
    for (const auto& shape : view.shapes()) {
      if (shape.id.node == node) {
        shapes.push_back(shape);
      }
    }
    return shapes;
  }

  ShapePoint pointOf(const ShapeView& view, NodeId node) {
    const auto* shape = shapeOf(view, node);
    TEST_CHECK(shape != nullptr);
    return {.x = shape->rect.x + shape->rect.width / 2, .y = shape->rect.y + shape->rect.height / 2};
  }

  // Visible labels showing `text`.
  std::vector<const TextNode*> labelsOf(const ShapeView& view, const std::string& text) {
    std::vector<const TextNode*> found;
    for (const auto& child : view.labelLayer().children()) {
      const auto* label = dynamic_cast<const TextNode*>(child.get());
      if (label != nullptr && label->visible() && label->text() == text) {
        found.push_back(label);
      }
    }
    return found;
  }

  void sameShapes(std::span<const Shape> actual, std::span<const Shape> expected) {
    TEST_CHECK(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size() && i < expected.size(); ++i) {
      TEST_CHECK(actual[i] == expected[i]);
    }
  }

  float topOf(const Shape& shape) {
    switch (shape.kind) {
    case ShapeKind::Circle:
      return shape.circle.center.y - shape.circle.radius;
    case ShapeKind::Ribbon:
      return std::min(shape.ribbon.top[0].y, shape.ribbon.top[3].y);
    case ShapeKind::Line:
      return std::min(shape.line.from.y, shape.line.to.y);
    case ShapeKind::Sector:
      return shape.sector.center.y - shape.sector.r1;
    default:
      return shape.rect.y;
    }
  }

  void eachKindNamesThePieceUnderThePointer() {
    for (const auto kind : kKinds) {
      Rig rig(kind);
      rig.frame(inputsFor(kind));
      const NodeId node = kind == WorkbenchRibbon::Ladder ? 6 : 1;
      const auto at = pointOf(rig.view->shapeView(), node);
      rig.dispatcher.pointerEnter(at.x, at.y, 1);
      TEST_CHECK(!rig.hovers.empty() && rig.hovers.back() == node);
      rig.dispatcher.pointerLeave();
      TEST_CHECK(rig.hovers.back() == kNoNode);
    }
  }

  void clicksAndContextCarryTheNode() {
    for (const auto kind : kKinds) {
      Rig rig(kind);
      const auto inputs = inputsFor(kind);
      rig.frame(inputs);
      TEST_CHECK(rig.view->shownSnapshot().tree == inputs.snapshot.tree && rig.view->shownSnapshot().serial == 1);
      const auto at = pointOf(rig.view->shapeView(), 1);
      rig.click(at, BTN_LEFT);
      TEST_CHECK(rig.requests.size() == 1);
      TEST_CHECK(rig.requests[0].kind == PartRequestKind::Go && rig.requests[0].node == 1);
      TEST_NEAR(rig.requests[0].x, at.x, 0.01F);
      TEST_NEAR(rig.requests[0].y, at.y, 0.01F);
      rig.dispatcher.pointerButton(at.x, at.y, BTN_RIGHT, true, 4, 0, false);
      TEST_CHECK(rig.requests.size() == 2);
      TEST_CHECK(rig.requests[1].kind == PartRequestKind::Context && rig.requests[1].node == 1);
      // The ribbon leaves the wheel to the page.
      TEST_CHECK(!rig.dispatcher.pointerAxis(at.x, at.y, 0, 0, -10, -1, -120, 0));
      TEST_CHECK(rig.requests.size() == 2);
    }
    Rig rig(WorkbenchRibbon::Ladder);
    rig.frame(inputsFor(WorkbenchRibbon::Ladder));
    rig.click(pointOf(rig.view->shapeView(), 6), BTN_LEFT);
    TEST_CHECK(rig.requests.size() == 1);
    TEST_CHECK(rig.requests[0].kind == PartRequestKind::Select && rig.requests[0].node == 6);
  }

  void limitsFollowTheKind() {
    Rig flame(WorkbenchRibbon::Flame);
    TEST_NEAR(flame.view->limits().minWidth, 480.0, 0.0);
    TEST_NEAR(flame.view->limits().minHeight, 160.0, 0.0);
    TEST_NEAR(flame.view->limits().preferredHeight, 224.0, 0.0);
    TEST_NEAR(flame.view->limits().maxHeight, 281.0, 0.0);
    Rig flow(WorkbenchRibbon::Flow);
    TEST_NEAR(flow.view->limits().minWidth, 560.0, 0.0);
    TEST_NEAR(flow.view->limits().minHeight, 180.0, 0.0);
    TEST_NEAR(flow.view->limits().preferredHeight, 280.0, 0.0);
    TEST_NEAR(flow.view->limits().maxHeight, 420.0, 0.0);
    Rig ladder(WorkbenchRibbon::Ladder);
    ladder.frame(inputsFor(chainTree(), 1));
    TEST_NEAR(ladder.view->limits().minWidth, 360.0, 0.0);
    TEST_NEAR(ladder.view->limits().minHeight, 76.0, 0.0);
    TEST_NEAR(ladder.view->limits().preferredHeight, 127.0, 0.0);
    TEST_NEAR(ladder.view->limits().maxHeight, 181.0, 0.0);
  }

  void aTallHostKeepsTheSceneAgainstTheList() {
    const std::array<float, 3> tallest{281, 420, 181};
    for (std::size_t i = 0; i < kKinds.size(); ++i) {
      const float height = tallest[i] + 200;
      Rig rig(kKinds[i], kWidth, height);
      rig.frame(kKinds[i] == WorkbenchRibbon::Ladder ? inputsFor(chainTree(), 1) : inputsFor(kKinds[i]));
      const auto shapes = rig.view->shapeView().shapes();
      TEST_CHECK(!shapes.empty());
      for (const auto& shape : shapes) {
        TEST_CHECK(topOf(shape) >= height - tallest[i] - 0.5F);
      }
    }
  }

  void switchingKindTweensAndEndsOnTheNewScene() {
    const auto inputs = inputsFor(fixture::referenceTree(), 1);
    Rig expected(WorkbenchRibbon::Flow);
    expected.frame(inputs);
    Rig rig(WorkbenchRibbon::Flame);
    rig.frame(inputs);
    rig.view->setKind(WorkbenchRibbon::Flow);
    rig.frame(inputs);
    TEST_CHECK(rig.view->shapeView().transitioning());
    rig.finishTransition();
    rig.frame(inputs);
    sameShapes(rig.view->shapeView().shapes(), expected.view->shapeView().shapes());
  }

  void onlyChangedInputsBuildAndResizingDoesNotTween() {
    auto inputs = inputsFor(fixture::referenceTree(), 1);
    Rig rig(WorkbenchRibbon::Flame);
    rig.frame(inputs);
    rig.frame(inputs);
    TEST_CHECK(rig.view->sceneBuilds() == 1);
    rig.view->setCursor(1);
    rig.frame(inputs);
    TEST_CHECK(rig.view->sceneBuilds() == 1);
    inputs.settings.emphasis = 0.2;
    rig.frame(inputs);
    TEST_CHECK(rig.view->sceneBuilds() == 2);
    const auto next = inputsFor(enteredAt(1), 2);
    rig.resize(kWidth, 240);
    rig.frame(next, true);
    TEST_CHECK(rig.view->sceneBuilds() == 3);
    TEST_CHECK(!rig.view->shapeView().transitioning());
    rig.frame(inputsFor(fixture::referenceTree(), 3));
    TEST_CHECK(rig.view->shapeView().transitioning());
  }

  void pointedAndCursorMarksReachTheScene() {
    for (const auto kind : kKinds) {
      const auto inputs = inputsFor(kind);
      Rig rig(kind);
      rig.frame(inputs);
      const auto& view = rig.view->shapeView();
      const auto plain = shapesOf(view, 1);
      rig.view->setPointed(1);
      rig.frame(inputs);
      TEST_CHECK(view.marks().linked == 1);
      TEST_CHECK(shapesOf(view, 1) != plain);
      rig.view->setPointed(kNoNode);
      rig.frame(inputs);
      TEST_CHECK(shapesOf(view, 1) == plain);
      // The list's cursor is an entry of the current folder, which the ladder does not draw.
      if (kind != WorkbenchRibbon::Ladder) {
        rig.view->setCursor(1);
        rig.frame(inputs);
        TEST_CHECK(view.marks().cursor == 1);
        TEST_CHECK(shapesOf(view, 1) != plain);
      }
      TEST_CHECK(rig.view->sceneBuilds() == 1);
    }
  }

  // Each note shows once, the flow diagram's where its scene puts it; the flame graph's sits over the strata ahead
  // below every bar, and the ladder, which draws only the ancestors' entries, has none.
  void folderStatesShowTheirNote() {
    auto unreadable = fixture::referenceTree();
    unreadable.nodes[4].unreadable = true;
    unreadable.root = 4;
    unreadable.ancestors = {0};
    const std::vector<std::pair<PartInputs, std::string>> cases{
        {inputsFor(enteredAt(4), 1), "This folder is empty"},
        {inputsFor(std::move(unreadable), 2), "This folder could not be read"},
    };
    for (const auto kind : kKinds) {
      for (const auto& [inputs, note] : cases) {
        Rig rig(kind);
        rig.frame(inputs);
        rig.frame(inputs);
        const auto& view = rig.view->shapeView();
        const auto labels = labelsOf(view, note);
        TEST_CHECK(labels.size() == (kind == WorkbenchRibbon::Ladder ? 0U : 1U));
        TEST_CHECK(rig.view->sceneBuilds() == 1);
        if (kind == WorkbenchRibbon::Flame && !labels.empty()) {
          const auto bands = flameBands(flameBandHeight(kHeight), inputs.snapshot.tree->ancestors.size());
          double top = kHeight;
          double bottom = 0;
          for (const auto& row : bands.rows) {
            if (row.kind == FlameRowKind::Ahead) {
              top = std::min(top, row.y);
              bottom = std::max(bottom, row.y + row.h);
            }
          }
          TEST_CHECK(top < bottom);
          // A label's node sits on its baseline, under the middle of its box by half a cap height.
          TEST_NEAR(static_cast<double>(labels[0]->y()), (top + bottom) / 2, 8.0);
        }
      }
      Rig rig(kind);
      rig.frame(PartInputs{});
      rig.frame(PartInputs{});
      TEST_CHECK(labelsOf(rig.view->shapeView(), "Loading…").size() == 1);
    }
  }

  void destroyingMidTransitionIsSafe() {
    for (const auto kind : kKinds) {
      Rig rig(kind);
      rig.frame(inputsFor(kind, 1));
      rig.frame(inputsFor(enteredAt(kind == WorkbenchRibbon::Ladder ? 7 : 1), 2));
      TEST_CHECK(rig.view->shapeView().transitioning());
      rig.root.removeChild(rig.view);
      rig.view = nullptr;
      rig.animations.tick(16);
      TEST_CHECK(!rig.animations.hasActive());
    }
  }
} // namespace

int main() {
  eachKindNamesThePieceUnderThePointer();
  clicksAndContextCarryTheNode();
  limitsFollowTheKind();
  aTallHostKeepsTheSceneAgainstTheList();
  switchingKindTweensAndEndsOnTheNewScene();
  onlyChangedInputsBuildAndResizingDoesNotTween();
  pointedAndCursorMarksReachTheScene();
  folderStatesShowTheirNote();
  destroyingMidTransitionIsSafe();
  return 0;
}
