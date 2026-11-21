#include "atlas_view_rig.h"
#include "instrument_rig.h"
#include "scene_capture.h"
#include "system_text_scale.h"
#include "test_check.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_fixture.h"

#include <cmath>
#include <format>
#include <linux/input-event-codes.h>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// The instruments through their views at the text scale of a 16 pt system font, with the real parts: what the
// pointer and the keys do reaches the part at the point of its own scene, and what the part asks for comes back at the
// place on the view where the shape is drawn. A font change while a worker lays the scene out at the old scale
// leaves the scene shown, and the pointer, at the old scale until the reply.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::test::instrument;
  using namespace luciole::test::atlas_view;

  constexpr float kLarge = 16.0F * 96.0F / 72.0F / 14.0F;

  // r holds a folder `a` and thirty small notes, so the dial needs "+N more".
  Tree longTree() {
    std::vector<std::string> names;
    names.reserve(40);
    std::vector<luciole::test::WorkbenchRow> rows{
        {.parent = -1, .name = "r", .folder = true, .bytes = 9e5, .items = 32},
        {.parent = 0, .name = "a", .folder = true, .bytes = 4e5, .items = 2},
        {.parent = 1, .name = "x", .bytes = 2e5},
    };
    for (int i = 0; i < 30; ++i) {
      names.push_back(std::format("note-{:02}.txt", i));
      rows.push_back({.parent = 0, .name = names.back(), .bytes = 3000.0 + i * 10});
    }
    return luciole::test::workbenchTree(rows, 1e6);
  }

  // A wheel over a piece reaches the part at the point of its scene: it enters the piece under the pointer.
  void wheelReachesTheScenePoint() {
    const auto inputs = inputsFor(longTree(), 1);
    Rig rig(WorkbenchInstrument::Iris, 560, 700);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto* piece = shapeOf(view, 1, SceneRole::Piece);
    TEST_CHECK(piece != nullptr);
    rig.requests.clear();
    TEST_CHECK(rig.wheel(inside(*piece), -1));
    TEST_CHECK(rig.requests.size() == 1 && rig.requests[0].kind == PartRequestKind::Go && rig.requests[0].node == 1);
    // a press hits the shape drawn under it
    rig.requests.clear();
    rig.click(inside(*piece), BTN_LEFT);
    TEST_CHECK(rig.requests.size() == 1 && rig.requests[0].node == 1);
  }

  // A key's request is anchored at the place the part drew its shape, on the view.
  void anchoredRequestComesBackOnTheView() {
    const auto inputs = inputsFor(longTree(), 1);
    Rig rig(WorkbenchInstrument::Iris, 560, 700);
    rig.show(inputs);
    rig.key(XKB_KEY_End);
    rig.frame(inputs);
    rig.requests.clear();
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.requests.size() == 1 && rig.requests[0].kind == PartRequestKind::ShowMore);
    const auto& ask = rig.requests[0];
    TEST_CHECK(ask.anchored);
    // the dial's "+N more" opens at the hub, which is drawn at the middle of the view
    bool onHub = false;
    for (const auto& shape : rig.view->shapeView().shapes()) {
      const bool hub = shape.id.node == 0 && sceneRole(shape.id.role) == SceneRole::Hub;
      onHub = onHub || (hub && shapeContains(shape, {.x = ask.x, .y = ask.y}, 1));
    }
    std::println("anchor at {:.1f},{:.1f}", ask.x, ask.y);
    TEST_CHECK(onHub);
  }

  // A drag over the Atlas moves the map by the pointer's travel on the view.
  void dragMovesTheMapByTheTravel() {
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Atlas, 560, 520);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto start = centerOf(view, 0);
    const auto from = pointOf(view, 2);
    press(rig, from);
    rig.dispatcher.pointerMotion(from.x + 10, from.y, 1);
    rig.frame(inputs);
    pump(50);
    rig.frame(inputs);
    for (int step = 2; step <= 4; ++step) {
      rig.dispatcher.pointerMotion(from.x + 10.0F * static_cast<float>(step), from.y, 1);
      rig.view->update(inputs, false);
    }
    TEST_CHECK(std::abs(centerOf(view, 0).x - (start.x + 40)) < 0.5F);
    TEST_CHECK(std::abs(centerOf(view, 0).y - start.y) < 0.5F);
    release(rig, {.x = from.x + 40, .y = from.y});
  }

  // Until the worker's reply the Nest's scene stays as it was laid out, whichever frames show it.
  void fontChangeLeavesTheShownSceneUntilTheReply(WorkbenchInstrument kind) {
    ::test::setSystemTextScale(1.0F);
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(kind, 560, 520);
    rig.show(inputs);
    settle(rig, inputs);
    const auto& view = rig.view->shapeView();
    const auto* const before = shapeOf(view, 1);
    TEST_CHECK(before != nullptr);
    const auto kept = *before;
    ::test::setSystemTextScale(kLarge);
    const auto over = pointOf(view, 2);
    rig.frame(inputs);                               // the view takes the new scale and asks for a layout
    rig.dispatcher.pointerMotion(over.x, over.y, 1); // the hover asks to show the scene again
    rig.frame(inputs);
    const auto* during = shapeOf(view, 1);
    TEST_CHECK(during != nullptr);
    // the size of the piece, not its place: the Atlas follows a new size with its camera at once
    const float wasSize = kept.kind == ShapeKind::Circle ? kept.circle.radius : kept.rect.width;
    const float nowSize = during->kind == ShapeKind::Circle ? during->circle.radius : during->rect.width;
    std::println("kind {}: piece size {:.1f} before, {:.1f} during", static_cast<int>(kind), wasSize, nowSize);
    TEST_CHECK(during->kind == kept.kind);
    TEST_CHECK(std::abs(nowSize - wasSize) < 0.01F);
    // after the reply the scene is the one for the new scale
    rig.show(inputs);
    settle(rig, inputs);
    const auto* after = shapeOf(view, 1);
    TEST_CHECK(after != nullptr);
    TEST_CHECK(std::abs((after->kind == ShapeKind::Circle ? after->circle.radius : after->rect.width) - wasSize) > 1);
  }
} // namespace

int main() {
  ::test::setSystemTextScale(kLarge);
  wheelReachesTheScenePoint();
  anchoredRequestComesBackOnTheView();
  dragMovesTheMapByTheTravel();
  // the Nest shows its scene as laid out until the worker's reply; the Atlas draws at the new size at once
  fontChangeLeavesTheShownSceneUntilTheReply(WorkbenchInstrument::Nest);
  std::println("workbench-text-scale-pointer: all checks passed");
  return 0;
}
