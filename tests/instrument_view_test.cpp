#include "instrument_rig.h"
#include "metrics_renderer.h"
#include "render/scene/text_node.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_atlas_fixture.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <linux/input-event-codes.h>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::test::instrument;
  using ::test::MetricsRenderer;
  namespace fixture = luciole::test;

  constexpr std::array<WorkbenchInstrument, 3> kKinds{
      WorkbenchInstrument::Nest, WorkbenchInstrument::Iris, WorkbenchInstrument::Atlas
  };

  void eachKindShowsTheTreeAndNamesThePieceUnderThePointer() {
    const auto inputs = inputsFor(balancedTree(), 1);
    for (const auto kind : kKinds) {
      Rig rig(kind);
      rig.show(inputs);
      TEST_CHECK(rig.view->shownSnapshot().serial == 1);
      TEST_CHECK(shapeOf(rig.view->shapeView(), 3) == nullptr); // hidden entries are not drawn
      const auto file = pointOf(rig.view->shapeView(), 2);
      rig.dispatcher.pointerEnter(file.x, file.y, 1);
      TEST_CHECK(!rig.hovers.empty() && rig.hovers.back() == 2);
      rig.dispatcher.pointerLeave();
      TEST_CHECK(rig.hovers.back() == kNoNode);
    }
  }

  void clicksAndContextCarryTheNode() {
    const auto inputs = inputsFor(balancedTree(), 1);
    for (const auto kind : kKinds) {
      Rig rig(kind);
      rig.show(inputs);
      const auto file = pointOf(rig.view->shapeView(), 2);
      rig.click(file, BTN_LEFT);
      TEST_CHECK(rig.requests.size() == 1);
      TEST_CHECK(rig.requests[0].kind == PartRequestKind::Select && rig.requests[0].node == 2);
      TEST_CHECK(rig.requests[0].button == BTN_LEFT);
      TEST_NEAR(rig.requests[0].x, file.x, 0.01F);
      TEST_NEAR(rig.requests[0].y, file.y, 0.01F);
      rig.click(pointOf(rig.view->shapeView(), 1), BTN_MIDDLE);
      TEST_CHECK(rig.requests.size() == 2);
      TEST_CHECK(rig.requests[1].kind == PartRequestKind::Go && rig.requests[1].node == 1);
      TEST_CHECK(rig.requests[1].button == BTN_MIDDLE);
      const auto at = pointOf(rig.view->shapeView(), 2);
      rig.dispatcher.pointerButton(at.x, at.y, BTN_RIGHT, true, 4, 0, false);
      TEST_CHECK(rig.requests.size() == 3);
      TEST_CHECK(rig.requests[2].kind == PartRequestKind::Context && rig.requests[2].node == 2);
      TEST_NEAR(rig.requests[2].x, at.x, 0.01F);
    }
  }

  // While an Atlas layout of a new snapshot is on the worker, the old scene stays, and so do the ids it speaks in.
  void requestsAndHoverSpeakTheShownSnapshot() {
    const auto first = inputsFor(balancedTree(), 1);
    const auto second = inputsFor(reorderedTree(), 2);
    Rig rig(WorkbenchInstrument::Atlas);
    rig.show(first);
    const auto file = pointOf(rig.view->shapeView(), 2);
    rig.frame(second);
    rig.click(file, BTN_LEFT);
    const auto& shown = rig.view->shownSnapshot();
    TEST_CHECK(shown.tree == first.snapshot.tree && shown.serial == 1);
    TEST_CHECK(rig.requests.size() == 1);
    TEST_CHECK(rig.requests[0].kind == PartRequestKind::Select);
    TEST_CHECK(shown.tree->nodes[rig.requests[0].node].name == "b.txt");
    TEST_CHECK(!rig.hovers.empty() && shown.tree->nodes[rig.hovers.back()].name == "b.txt");
    rig.show(second);
    TEST_CHECK(rig.view->shownSnapshot().serial == 2);
    rig.click(pointOf(rig.view->shapeView(), 3), BTN_LEFT);
    TEST_CHECK(rig.requests.size() == 2);
    TEST_CHECK(rig.requests[1].kind == PartRequestKind::Select && rig.requests[1].node == 3);
  }

  void levelWheelStepsOneLevelPerInterval() {
    const auto tree = enteredAt(1);
    const auto start = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    const ShapeWheel up{.vertical = true, .delta = -10, .x = 0, .y = 0};
    const ShapeWheel down{.vertical = true, .delta = 10, .x = 0, .y = 0};
    LevelWheel wheel;
    const auto entered = wheel.turn(up, tree, 5, start);
    TEST_CHECK(entered.handled && entered.requests.size() == 1 && entered.requests[0].node == 5);
    const auto early = wheel.turn(down, tree, kNoNode, start + LevelWheel::kInterval - std::chrono::milliseconds(1));
    TEST_CHECK(early.handled && early.requests.empty());
    const auto later = wheel.turn(down, tree, kNoNode, start + LevelWheel::kInterval);
    TEST_CHECK(later.requests.size() == 1 && later.requests[0].kind == PartRequestKind::Go);
    TEST_CHECK(later.requests[0].node == 0);
    TEST_CHECK(!wheel.turn({.vertical = false, .delta = 10, .x = 0, .y = 0}, tree, 5, start).handled);
  }

  void nestAndIrisWheelStepALevel() {
    for (const auto kind : {WorkbenchInstrument::Nest, WorkbenchInstrument::Iris}) {
      const auto inputs = inputsFor(enteredAt(1), 1);
      Rig rig(kind);
      rig.show(inputs);
      // Up over a folder enters it; a file does nothing.
      TEST_CHECK(rig.wheel(pointOf(rig.view->shapeView(), 6), -10));
      TEST_CHECK(rig.requests.empty());
      TEST_CHECK(rig.wheel(pointOf(rig.view->shapeView(), 5), -10));
      TEST_CHECK(rig.requests.size() == 1);
      TEST_CHECK(rig.requests[0].kind == PartRequestKind::Go && rig.requests[0].node == 5);
      // Down goes to the parent.
      Rig fresh(kind);
      fresh.show(inputs);
      TEST_CHECK(fresh.wheel(pointOf(fresh.view->shapeView(), 5), 10));
      TEST_CHECK(fresh.requests.size() == 1);
      TEST_CHECK(fresh.requests[0].kind == PartRequestKind::Go && fresh.requests[0].node == 0);
    }
  }

  void atlasWheelZoomsAboutThePointerAndEntersAFolder() {
    auto tree = fixture::atlas::atlasTree();
    tree.root = fixture::atlas::kHome;
    tree.ancestors = parentChain(tree, tree.root);
    const auto inputs = inputsFor(std::move(tree), 1);
    Rig rig(WorkbenchInstrument::Atlas);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto* docs = shapeOf(view, fixture::atlas::kDocs);
    TEST_CHECK(docs != nullptr && docs->kind == ShapeKind::Circle);
    const auto at = docs->circle.center;
    const float radius = docs->circle.radius;
    TEST_CHECK(rig.wheel(at, -10));
    rig.until(inputs, [&] {
      const auto* now = shapeOf(view, fixture::atlas::kDocs);
      return now != nullptr && now->circle.radius > radius;
    });
    const auto* zoomed = shapeOf(view, fixture::atlas::kDocs);
    TEST_NEAR(zoomed->circle.center.x, at.x, 0.5F);
    TEST_NEAR(zoomed->circle.center.y, at.y, 0.5F);
    // Zooming on until docs fills more than half the view enters it; each turn lands over the frames after it.
    for (int step = 0; step < 40 && rig.requests.empty(); ++step) {
      rig.wheel(at, -40);
      rig.frame(inputs);
    }
    TEST_CHECK(rig.requests.size() == 1);
    TEST_CHECK(rig.requests[0].kind == PartRequestKind::Go && rig.requests[0].node == fixture::atlas::kDocs);
  }

  void keysFollowEachKindsTable() {
    const auto inputs = inputsFor(balancedTree(), 1);
    for (const auto kind : {WorkbenchInstrument::Nest, WorkbenchInstrument::Iris}) {
      Rig rig(kind);
      rig.show(inputs);
      rig.key(XKB_KEY_Home);
      TEST_CHECK(rig.requests.size() == 1);
      TEST_CHECK(rig.requests[0].kind == PartRequestKind::Select && rig.requests[0].node == 1);
      rig.view->setCursor(1);
      rig.frame(inputs);
      rig.key(kind == WorkbenchInstrument::Nest ? XKB_KEY_End : XKB_KEY_Down);
      TEST_CHECK(rig.requests.size() == 2);
      TEST_CHECK(rig.requests[1].kind == PartRequestKind::Select && rig.requests[1].node != 1);
      rig.key(XKB_KEY_Return);
      TEST_CHECK(rig.requests.size() == 3);
      TEST_CHECK(rig.requests[2].kind == PartRequestKind::Go && rig.requests[2].node == 1);
      rig.view->setCursor(2);
      rig.frame(inputs);
      rig.key(XKB_KEY_Return);
      TEST_CHECK(rig.requests.size() == 4);
      TEST_CHECK(rig.requests[3].kind == PartRequestKind::Open && rig.requests[3].node == 2);
    }
    Rig rig(WorkbenchInstrument::Atlas);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const float rest = shapeOf(view, 2)->circle.radius;
    rig.key(XKB_KEY_minus, '-');
    rig.until(inputs, [&] { return std::abs(shapeOf(view, 2)->circle.radius - rest / 1.6F) < 0.01F; });
    rig.key(XKB_KEY_0, '0');
    rig.until(inputs, [&] { return std::abs(shapeOf(view, 2)->circle.radius - rest) < 0.01F; });
    rig.view->setCursor(2);
    rig.frame(inputs);
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.requests.size() == 1);
    TEST_CHECK(rig.requests[0].kind == PartRequestKind::Open && rig.requests[0].node == 2);
  }

  void backspaceGoesUpALevel() {
    const auto top = inputsFor(balancedTree(), 1);
    const auto inside = inputsFor(enteredAt(1), 2);
    for (const auto kind : kKinds) {
      Rig rig(kind);
      rig.show(top);
      rig.key(XKB_KEY_BackSpace);
      TEST_CHECK(rig.requests.empty());
      rig.show(inside);
      rig.key(XKB_KEY_BackSpace);
      TEST_CHECK(rig.requests.size() == 1);
      TEST_CHECK(rig.requests[0].kind == PartRequestKind::Go && rig.requests[0].node == 0);
    }
  }

  void switchingKindTweensAndEndsOnTheNewScene() {
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig expected(WorkbenchInstrument::Iris);
    expected.show(inputs);
    Rig rig(WorkbenchInstrument::Nest);
    rig.show(inputs);
    rig.view->setKind(WorkbenchInstrument::Iris);
    rig.frame(inputs);
    TEST_CHECK(rig.view->shapeView().transitioning());
    rig.finishTransition();
    rig.frame(inputs);
    TEST_CHECK(!rig.view->shapeView().transitioning());
    sameShapes(rig.view->shapeView().shapes(), expected.view->shapeView().shapes());
  }

  void onlyChangedInputsBuild() {
    auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Nest);
    rig.frame(inputs);
    TEST_CHECK(rig.view->sceneBuilds() == 1);
    rig.frame(inputs);
    rig.frame(inputs);
    TEST_CHECK(rig.view->sceneBuilds() == 1);
    rig.view->setPointed(2);
    rig.frame(inputs);
    TEST_CHECK(rig.view->sceneBuilds() == 1); // marks only restyle the scene
    inputs.settings.sizeBy = WorkbenchMeasure::Files;
    rig.frame(inputs);
    TEST_CHECK(rig.view->sceneBuilds() == 2);
    rig.resize(400, 380);
    rig.frame(inputs);
    TEST_CHECK(rig.view->sceneBuilds() == 3);
    inputs.snapshot.serial = 2;
    rig.frame(inputs);
    TEST_CHECK(rig.view->sceneBuilds() == 4);
    rig.frame(inputs);
    TEST_CHECK(rig.view->sceneBuilds() == 4);
    inputs.palette.surface.r ^= 1U;
    rig.frame(inputs);
    TEST_CHECK(rig.view->sceneBuilds() == 5);
  }

  void resizingStartsNoTransition() {
    const auto first = inputsFor(balancedTree(), 1);
    const auto second = inputsFor(enteredAt(1), 2);
    const auto third = inputsFor(enteredAt(5), 3);
    Rig rig(WorkbenchInstrument::Nest);
    rig.show(first);
    rig.resize(400, 360);
    rig.frame(second, true);
    rig.until(second, [&] { return rig.view->shownSnapshot().serial == 2; });
    TEST_CHECK(!rig.view->shapeView().transitioning());
    TEST_CHECK(shapeOf(rig.view->shapeView(), 5) != nullptr);
    rig.frame(third);
    rig.until(third, [&] { return rig.view->shownSnapshot().serial == 3; });
    TEST_CHECK(rig.view->shapeView().transitioning());
  }

  // Shapes pair by node id, so a snapshot whose ids name other entries shows at once.
  void aSnapshotWithOtherIdsShowsAtOnce() {
    for (const auto kind : kKinds) {
      Rig rig(kind);
      rig.show(inputsFor(balancedTree(), 1));
      rig.show(inputsFor(reorderedTree(), 2));
      TEST_CHECK(!rig.view->shapeView().transitioning());
      rig.show(inputsFor(reorderedTree(), 3));
      // The Atlas moves its own camera instead of tweening shapes.
      TEST_CHECK(rig.view->shapeView().transitioning() == (kind != WorkbenchInstrument::Atlas));
    }
  }

  // A wheel turn while an Atlas layout of a new snapshot is on the worker: the entered folder still flies in.
  void aWheelTurnKeepsTheFlightToCome() {
    const auto first = inputsFor(balancedTree(), 1);
    const auto second = inputsFor(enteredAt(4), 2);
    Rig rig(WorkbenchInstrument::Atlas);
    rig.show(first);
    rig.frame(second);
    TEST_CHECK(rig.wheel({.x = kWidth / 2, .y = kHeight / 2}, -1));
    rig.frame(second);
    rig.until(second, [&] { return rig.view->shownSnapshot().serial == 2; });
    TEST_CHECK(!rig.view->shapeView().transitioning());
    const auto& view = rig.view->shapeView();
    const float start = shapeOf(view, 4)->circle.radius;
    rig.until(second, [&] { return shapeOf(view, 4)->circle.radius > start + 1; });
  }

  void pointedAndCursorMarksReachTheScene() {
    const auto inputs = inputsFor(balancedTree(), 1);
    for (const auto kind : kKinds) {
      Rig rig(kind);
      rig.show(inputs);
      const auto& view = rig.view->shapeView();
      const auto plain = shapesOf(view, 2);
      const auto requested = rig.frameRequests;
      rig.view->setPointed(2);
      TEST_CHECK(rig.frameRequests > requested);
      rig.until(inputs, [&] { return shapesOf(view, 2) != plain; });
      TEST_CHECK(view.marks().linked == 2);
      rig.view->setPointed(kNoNode);
      rig.until(inputs, [&] { return shapesOf(view, 2) == plain; });
      rig.view->setCursor(2);
      rig.until(inputs, [&] { return shapesOf(view, 2) != plain; });
      TEST_CHECK(view.marks().cursor == 2);
      TEST_CHECK(rig.view->sceneBuilds() == 1);
    }
  }

  void folderStatesShowTheirNote() {
    auto unreadable = balancedTree();
    unreadable.nodes[8].unreadable = true;
    unreadable.root = 8;
    unreadable.ancestors = {0};
    // A folder a walk cut short and left is not running: its note is the one for entries left out, never "Loading…".
    auto cutShort = enteredAt(8);
    cutShort.nodes[8].lowerBound = true;
    cutShort.nodes[8].truncated = true;
    cutShort.nodes[8].items = 5;
    const std::vector<std::pair<PartInputs, std::string>> cases{
        {inputsFor(enteredAt(8), 1), "This folder is empty"},
        {inputsFor(std::move(unreadable), 2), "This folder could not be read"},
        {inputsFor(std::move(cutShort), 3), "Not expanded in this snapshot: at least 5 items"},
        {PartInputs{}, "Loading…"},
    };
    for (const auto kind : kKinds) {
      for (const auto& [inputs, note] : cases) {
        Rig rig(kind);
        rig.until(inputs, [&] { return notesWritten(rig.view->shapeView(), note) > 0; });
        TEST_CHECK(notesWritten(rig.view->shapeView(), note) == 1);
        TEST_CHECK(note == "Loading…" || notesWritten(rig.view->shapeView(), "Loading…") == 0);
        TEST_CHECK(rig.view->sceneBuilds() == 1);
      }
    }
  }

  void labelsFitTheirRoom() {
    auto tree = balancedTree();
    tree.nodes[2].name = "a file name far too long for the tile it sits in, whatever the view size.txt";
    const auto inputs = inputsFor(std::move(tree), 1);
    Rig rig(WorkbenchInstrument::Nest);
    rig.show(inputs);
    MetricsRenderer renderer;
    Renderer& metrics = renderer;
    bool cut = false;
    for (const auto& child : rig.view->shapeView().labelLayer().children()) {
      const auto* label = dynamic_cast<const TextNode*>(child.get());
      if (label == nullptr || !label->visible()) {
        continue;
      }
      const auto natural = metrics.measureText(label->text(), label->fontSize()).width;
      TEST_CHECK(natural <= label->maxWidth() + 0.01F);
      cut = cut || label->text().contains("…");
    }
    TEST_CHECK(cut);
  }

  void destroyingMidTransitionIsSafe() {
    const auto first = inputsFor(balancedTree(), 1);
    const auto second = inputsFor(enteredAt(1), 2);
    for (const auto kind : kKinds) {
      Rig rig(kind);
      rig.show(first);
      rig.frame(second);
      // The Atlas flies its own camera to the new folder instead of tweening shapes.
      rig.until(second, [&] {
        return kind == WorkbenchInstrument::Atlas ? rig.view->shownSnapshot().serial == 2
                                                  : rig.view->shapeView().transitioning();
      });
      rig.root.removeChild(rig.view);
      rig.view = nullptr;
      rig.animations.tick(16);
      TEST_CHECK(!rig.animations.hasActive());
    }
  }

  void limitsAreTheContracts() {
    const auto limits = InstrumentView::limits();
    TEST_NEAR(limits.minWidth, 240.0, 0.0);
    TEST_NEAR(limits.minHeight, 220.0, 0.0);
  }
} // namespace

int main() {
  eachKindShowsTheTreeAndNamesThePieceUnderThePointer();
  clicksAndContextCarryTheNode();
  requestsAndHoverSpeakTheShownSnapshot();
  levelWheelStepsOneLevelPerInterval();
  nestAndIrisWheelStepALevel();
  atlasWheelZoomsAboutThePointerAndEntersAFolder();
  keysFollowEachKindsTable();
  backspaceGoesUpALevel();
  switchingKindTweensAndEndsOnTheNewScene();
  onlyChangedInputsBuild();
  resizingStartsNoTransition();
  aSnapshotWithOtherIdsShowsAtOnce();
  aWheelTurnKeepsTheFlightToCome();
  pointedAndCursorMarksReachTheScene();
  folderStatesShowTheirNote();
  labelsFitTheirRoom();
  destroyingMidTransitionIsSafe();
  limitsAreTheContracts();
  return 0;
}
