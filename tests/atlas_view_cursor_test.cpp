#include "atlas_view_rig.h"
#include "instrument_rig.h"
#include "test_check.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"

#include <array>

// The Atlas follows the cursor: a cursor on a bubble the view does not show well brings the map back to rest.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::test::instrument;
  using namespace luciole::test::atlas_view;

  // Drags the map `dx` px across in four steps.
  void pan(Rig& rig, const PartInputs& inputs, ShapePoint from, float dx) {
    press(rig, from);
    for (int step = 1; step <= 4; ++step) {
      rig.dispatcher.pointerMotion(from.x + dx * static_cast<float>(step) / 4, from.y, 1);
      rig.frame(inputs);
    }
    release(rig, {.x = from.x + dx, .y = from.y});
    settle(rig, inputs);
  }

  // A cursor on a bubble in plain view moves nothing; one on a bubble near the view's edge flies the map to rest.
  void aCursorNearTheEdgeIsRevealed() {
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Atlas, 900, 600);
    rig.show(inputs);
    settle(rig, inputs);
    const auto& view = rig.view->shapeView();
    const float rest = radiusOf(view, 0);
    const auto center = centerOf(view, 0);
    rig.view->setCursor(2);
    settle(rig, inputs);
    TEST_NEAR(centerOf(view, 0).x, center.x, 0.01F);
    TEST_NEAR(radiusOf(view, 0), rest, 0.01F);
    rig.view->setCursor(kNoNode);
    pan(rig, inputs, {.x = center.x, .y = center.y + 40}, 400);
    TEST_CHECK(centerOf(view, 0).x > center.x + 200);
    // The bubble farthest right is now within 30 px of the view's right edge, or past it.
    NodeId edge = kNoNode;
    float right = 0;
    for (const NodeId node : std::array<NodeId, 4>{1, 2, 4, 8}) {
      if (const auto* shape = shapeOf(view, node); shape != nullptr && shape->circle.center.x > right) {
        right = shape->circle.center.x;
        edge = node;
      }
    }
    TEST_CHECK(edge != kNoNode && right > 900 - 30);
    const auto panned = centerOf(view, 0);
    rig.view->setCursor(edge);
    rig.until(inputs, [&] { return centerOf(view, 0).x < panned.x - 1; });
    settle(rig, inputs);
    TEST_NEAR(centerOf(view, 0).x, center.x, 0.01F);
    TEST_NEAR(centerOf(view, 0).y, center.y, 0.01F);
    TEST_NEAR(radiusOf(view, 0), rest, 0.01F);
  }
} // namespace

int main() {
  aCursorNearTheEdgeIsRevealed();
  return 0;
}
