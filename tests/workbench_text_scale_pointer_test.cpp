#include "atlas_view_rig.h"
#include "instrument_rig.h"
#include "scene_capture.h"
#include "system_text_scale.h"
#include "test_check.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_fixture.h"

#include <chrono>
#include <cmath>
#include <format>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <string>
#include <thread>
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

  float pieceSize(const Rig& rig, NodeId node) {
    const auto* piece = shapeOf(rig.view->shapeView(), node);
    if (piece == nullptr) {
      return -1;
    }
    return piece->kind == ShapeKind::Circle ? piece->circle.radius : piece->rect.width;
  }

  // The size the piece of `node` settles at, in a pane made at text scale `scale`.
  float settledSize(WorkbenchInstrument kind, const PartInputs& inputs, NodeId node, float scale) {
    ::test::setSystemTextScale(scale);
    Rig rig(kind, 560, 520);
    rig.show(inputs);
    settle(rig, inputs);
    return pieceSize(rig, node);
  }

  // Until the worker's reply the Nest's scene stays as it was laid out, whichever frames show it.
  void fontChangeLeavesTheShownSceneUntilTheReply(WorkbenchInstrument kind) {
    ::test::setSystemTextScale(1.0F);
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(kind, 560, 520);
    rig.show(inputs);
    settle(rig, inputs);
    const auto& view = rig.view->shapeView();
    const float wasSize = pieceSize(rig, 1);
    TEST_CHECK(wasSize > 0);
    ::test::setSystemTextScale(kLarge);
    const auto over = pointOf(view, 2);
    rig.frame(inputs);                               // the view takes the new scale and asks for a layout
    rig.dispatcher.pointerMotion(over.x, over.y, 1); // the hover asks to show the scene again
    rig.frame(inputs);
    const float nowSize = pieceSize(rig, 1);
    std::println("kind {}: piece size {:.1f} before, {:.1f} during", static_cast<int>(kind), wasSize, nowSize);
    TEST_CHECK(std::abs(nowSize - wasSize) < 0.01F);
    // after the reply the scene is the one for the new scale
    rig.show(inputs);
    settle(rig, inputs);
    TEST_CHECK(std::abs(pieceSize(rig, 1) - wasSize) > 1);
  }

  // The Nest keeps its scene at the scale it was laid out for, and a reply to the layout made before the font changed
  // is shown at that scale while the layout for the new one runs. The first reply waits, undelivered, until the new
  // layout is asked for.
  void nestKeepsSceneScaleThroughTwoReplies() {
    const auto inputs = inputsFor(balancedTree(), 1);
    const float sizeA = settledSize(WorkbenchInstrument::Nest, inputs, 1, 1.0F);
    const float sizeB = settledSize(WorkbenchInstrument::Nest, inputs, 1, kLarge);
    TEST_CHECK(
        std::abs(sizeA * kLarge - sizeB) > 20 && std::abs(sizeA * kLarge - sizeA) > 20
    ); // a wrong scale stands out
    ::test::setSystemTextScale(1.0F);
    Rig rig(WorkbenchInstrument::Nest, 560, 520);
    rig.frame(inputs); // the view takes scale 1 and measures its labels
    rig.frame(inputs); // the layout for scale 1 starts on a worker
    ::test::setSystemTextScale(kLarge);
    rig.frame(inputs); // the view takes the new scale
    for (int wait = 0; wait < 5000 && g_main_context_pending(nullptr) == FALSE; ++wait) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(g_main_context_pending(nullptr) != FALSE); // the reply to the first layout is waiting
    rig.frame(inputs);                                    // the layout for the new scale is asked for, queued behind it
    g_main_context_iteration(nullptr, FALSE);             // the first reply, one at a time
    rig.frame(inputs);
    const float first = pieceSize(rig, 1);
    std::println(
        "after the first reply: piece {:.1f} (laid out at 1: {:.1f}, at {:.2f}: {:.1f})", first, sizeA, kLarge, sizeB
    );
    TEST_CHECK(std::abs(first - sizeA) < 0.5F);
    int steady = 0;
    for (int turn = 0; turn < 3000 && steady < 40; ++turn) {
      g_main_context_iteration(nullptr, FALSE);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      rig.frame(inputs);
      const float size = pieceSize(rig, 1);
      TEST_CHECK(std::abs(size - sizeA) < 0.5F || std::abs(size - sizeB) < 0.5F);
      steady = std::abs(size - sizeB) < 0.5F ? steady + 1 : 0;
    }
    TEST_CHECK(steady >= 40); // the second reply shows the scene for the new scale
  }

  // The Atlas draws its scene against the view as it is at once, so its size follows the font before the reply.
  void atlasFollowsAtOnce() {
    const auto inputs = inputsFor(balancedTree(), 1);
    const float sizeB = settledSize(WorkbenchInstrument::Atlas, inputs, 1, kLarge);
    const float sizeA = settledSize(WorkbenchInstrument::Atlas, inputs, 1, 1.0F);
    Rig rig(WorkbenchInstrument::Atlas, 560, 520);
    ::test::setSystemTextScale(1.0F);
    rig.show(inputs);
    settle(rig, inputs);
    ::test::setSystemTextScale(kLarge);
    rig.frame(inputs);
    const auto over = pointOf(rig.view->shapeView(), 2);
    rig.dispatcher.pointerMotion(over.x, over.y, 1);
    rig.frame(inputs);
    const float during = pieceSize(rig, 1);
    std::println("atlas piece {:.1f} at 1, {:.1f} during, {:.1f} at {:.2f}", sizeA, during, sizeB, kLarge);
    TEST_CHECK(std::abs(during - sizeB) < std::abs(during - sizeA));
  }

  // Another folder asked for while the font changes: whether the old scene is mapped to the new view or drawn as it
  // was laid out, it is never drawn larger than the font change makes it.
  void atlasFolderChangeThenFontChange(NodeId folder) {
    ::test::setSystemTextScale(1.0F);
    const auto first = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Atlas, 560, 520);
    rig.show(first);
    settle(rig, first);
    const float before = pieceSize(rig, 2);
    TEST_CHECK(before > 0);
    const auto next = inputsFor(enteredAt(folder), 2);
    rig.frame(next); // the layout for the other folder starts
    ::test::setSystemTextScale(kLarge);
    rig.frame(next);
    const auto over = pointOf(rig.view->shapeView(), 2);
    rig.dispatcher.pointerMotion(over.x, over.y, 1);
    rig.frame(next);
    const float during = pieceSize(rig, 2);
    std::println("atlas folder {}: piece {:.1f} before, {:.1f} during", folder, before, during);
    TEST_CHECK(during < 0 || during < before * 1.3F);
  }
} // namespace

int main() {
  ::test::setSystemTextScale(kLarge);
  wheelReachesTheScenePoint();
  anchoredRequestComesBackOnTheView();
  dragMovesTheMapByTheTravel();
  fontChangeLeavesTheShownSceneUntilTheReply(WorkbenchInstrument::Nest);
  nestKeepsSceneScaleThroughTwoReplies();
  atlasFollowsAtOnce();
  for (const NodeId folder : {1, 5, 4}) {
    atlasFolderChangeThenFontChange(folder);
  }
  std::println("workbench-text-scale-pointer: all checks passed");
  return 0;
}
