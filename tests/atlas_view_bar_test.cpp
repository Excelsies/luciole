#include "atlas_view_rig.h"
#include "instrument_rig.h"
#include "render/scene/text_node.h"
#include "test_check.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"

#include <algorithm>
#include <cstddef>
#include <linux/input-event-codes.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// The bar of a small Atlas pane and the controls over the map: they are not part of the map, so a click or a drag on
// them never reaches it.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::test::instrument;
  using namespace luciole::test::atlas_view;

  constexpr float kBar = 26;

  bool isControl(const Shape& shape) {
    const auto role = sceneRole(shape.id.role);
    return role == SceneRole::Chip || role == SceneRole::Button;
  }

  // The bar under the chips: a chip that stands for no folder.
  bool isBar(const Shape& shape) { return sceneRole(shape.id.role) == SceneRole::Chip && shape.id.node == kNoNode; }

  bool within(const ShapeRect& rect, float x, float y) {
    return x >= rect.x && x <= rect.x + rect.width && y >= rect.y && y <= rect.y + rect.height;
  }

  // The points of the bar's middle line that no chip or button covers.
  std::vector<ShapePoint> barGaps(const ShapeView& view) {
    std::vector<ShapePoint> gaps;
    for (float x = 1; x < view.width(); x += 2) {
      const bool covered = std::ranges::any_of(view.shapes(), [x](const Shape& shape) {
        return isControl(shape) && !isBar(shape) && within(shape.rect, x, kBar / 2);
      });
      if (!covered) {
        gaps.push_back({.x = x, .y = kBar / 2});
      }
    }
    return gaps;
  }

  // Every point of the bar takes its hit on a chip, a button or the bar itself, and the only text in it is a chip's.
  void barHoldsNoMap(const ShapeView& view) {
    std::size_t points = 0;
    std::size_t map = 0;
    for (float y = 1; y < kBar; y += 4) {
      for (float x = 1; x < view.width(); x += 3) {
        const auto index = hitShape(view.shapes(), {x, y}, 0);
        ++points;
        map += index && isControl(view.shapes()[*index]) ? 0 : 1;
      }
    }
    TEST_CHECK(points > 500 && map == 0);
    std::size_t chipText = 0;
    for (const auto& child : view.labelLayer().children()) {
      const auto* label = dynamic_cast<const TextNode*>(child.get());
      if (label == nullptr || !label->visible() || label->y() >= kBar) {
        continue;
      }
      const bool onChip = std::ranges::any_of(view.shapes(), [label](const Shape& shape) {
        return sceneRole(shape.id.role) == SceneRole::Chip
            && shape.id.node != kNoNode
            && within(shape.rect, label->x() + 1, label->y() + 1);
      });
      TEST_CHECK(onChip);
      chipText += onChip ? 1 : 0;
    }
    TEST_CHECK(chipText >= 1);
  }

  // A click in the bar's gaps asks for nothing and hovers no entry.
  void gapsTakeNoClick(Rig& rig) {
    const auto gaps = barGaps(rig.view->shapeView());
    TEST_CHECK(gaps.size() > 10);
    const auto requests = rig.requests.size();
    for (std::size_t i = 0; i < gaps.size(); i += 5) {
      rig.click(gaps[i], BTN_LEFT);
      TEST_CHECK(rig.hovers.empty() || rig.hovers.back() == kNoNode);
    }
    TEST_CHECK(rig.requests.size() == requests);
  }

  // In a small pane, the map stays under the bar at rest and zoomed in, and the bar's gaps take no click.
  void theBarKeepsTheMapOut() {
    const auto inputs = inputsFor(enteredAt(1), 1);
    Rig rig(WorkbenchInstrument::Atlas, 340, 380);
    rig.show(inputs);
    settle(rig, inputs);
    barHoldsNoMap(rig.view->shapeView());
    gapsTakeNoClick(rig);
    rig.key(XKB_KEY_plus, '+');
    rig.key(XKB_KEY_plus, '+');
    settle(rig, inputs);
    barHoldsNoMap(rig.view->shapeView());
    gapsTakeNoClick(rig);
  }

  // A drag from `from` over 40 px leaves the map where it was.
  void dragLeavesTheMap(Rig& rig, const PartInputs& inputs, ShapePoint from, NodeId node) {
    const auto& view = rig.view->shapeView();
    const auto before = centerOf(view, node);
    press(rig, from);
    for (int step = 1; step <= 4; ++step) {
      rig.dispatcher.pointerMotion(from.x + 10.0F * static_cast<float>(step), from.y + 5, 1);
      rig.frame(inputs);
    }
    release(rig, {.x = from.x + 40, .y = from.y + 5});
    settle(rig, inputs);
    TEST_NEAR(centerOf(view, node).x, before.x, 0.01F);
    TEST_NEAR(centerOf(view, node).y, before.y, 0.01F);
  }

  ShapePoint middle(const Shape& shape) {
    return {.x = shape.rect.x + shape.rect.width / 2, .y = shape.rect.y + shape.rect.height / 2};
  }

  // A drag that starts on a chip, a button or the bar's gap does not pan, and the wheel over them does not zoom.
  void controlsDoNotMoveTheMap() {
    const auto inputs = inputsFor(enteredAt(5), 1);
    Rig rig(WorkbenchInstrument::Atlas, 340, 380);
    rig.show(inputs);
    settle(rig, inputs);
    const auto& view = rig.view->shapeView();
    const auto* chip = hittable(view, SceneRole::Chip, 0);
    const auto* button = hittable(view, SceneRole::Button, kNoNode, 1);
    TEST_CHECK(chip != nullptr && button != nullptr);
    const auto onChip = middle(*chip);
    const auto onButton = middle(*button);
    const auto gaps = barGaps(view);
    TEST_CHECK(!gaps.empty());
    dragLeavesTheMap(rig, inputs, onChip, 5);
    dragLeavesTheMap(rig, inputs, onButton, 5);
    dragLeavesTheMap(rig, inputs, gaps.front(), 5);
    TEST_CHECK(rig.requests.empty());
    const float rest = radiusOf(view, 5);
    for (const auto at : {onChip, onButton, gaps.front()}) {
      TEST_CHECK(!rig.wheel(at, -100));
      settle(rig, inputs);
      TEST_NEAR(radiusOf(view, 5), rest, 0.01F);
    }
    // The map itself still pans and zooms.
    const auto center = centerOf(view, 5);
    TEST_CHECK(rig.wheel(center, -100));
    settle(rig, inputs);
    TEST_CHECK(radiusOf(view, 5) > rest * 1.05F);
  }

  // In a large pane the chips and buttons float over the map: a drag from them does not pan, one beside them does.
  void largePaneControls() {
    const auto inputs = inputsFor(enteredAt(1), 1);
    Rig rig(WorkbenchInstrument::Atlas, 900, 600);
    rig.show(inputs);
    settle(rig, inputs);
    const auto& view = rig.view->shapeView();
    const auto* chip = hittable(view, SceneRole::Chip, 0);
    const auto* button = hittable(view, SceneRole::Button, kNoNode, 2);
    TEST_CHECK(chip != nullptr && button != nullptr);
    dragLeavesTheMap(rig, inputs, middle(*chip), 1);
    dragLeavesTheMap(rig, inputs, middle(*button), 1);
    const auto before = centerOf(view, 1);
    const ShapePoint beside{.x = before.x, .y = before.y + 20};
    press(rig, beside);
    for (int step = 1; step <= 4; ++step) {
      rig.dispatcher.pointerMotion(beside.x + 10.0F * static_cast<float>(step), beside.y, 1);
      rig.frame(inputs);
    }
    release(rig, {.x = beside.x + 40, .y = beside.y});
    settle(rig, inputs);
    TEST_NEAR(centerOf(view, 1).x, before.x + 40, 0.01F);
  }
} // namespace

int main() {
  theBarKeepsTheMapOut();
  controlsDoNotMoveTheMap();
  largePaneControls();
  return 0;
}
