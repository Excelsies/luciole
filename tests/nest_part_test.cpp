#include "core/timer_manager.h"
#include "instrument_rig.h"
#include "nest_rig.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/nest_scene.h"
#include "workbench/nest_texture.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <variant>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// The Nest in its instrument view: the peek card by pointer and keyboard, the "+N more" stop, tooltips, the texture
// built on a worker, and the note of an empty folder, driven through a real input dispatcher.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::test::instrument;
  using namespace luciole::test::nest;

  constexpr NodeId kA = 1;    // folder a in balancedTree(), holding x and y.png
  constexpr NodeId kX = 5;    // folder x in a
  constexpr NodeId kC = 4;    // folder c, holding z.txt
  constexpr NodeId kFile = 2; // b.txt
  constexpr auto kDwell = std::chrono::milliseconds(kNestPeekDwellMs);

  struct ShownLabel {
    std::string text;
    float x = 0;
    float y = 0;
  };

  std::vector<ShownLabel> labelsShown(const ShapeView& view) {
    std::vector<ShownLabel> shown;
    for (const auto& child : view.labelLayer().children()) {
      const auto* label = dynamic_cast<const TextNode*>(child.get());
      if (label != nullptr && label->visible() && label->opacity() > 0.5F) {
        shown.push_back({.text = label->text(), .x = label->x(), .y = label->y()});
      }
    }
    return shown;
  }

  // Whether the label's first character lies on the box.
  bool meets(const ShownLabel& label, const ShapeRect& box) { return within(box, label.x + 2, label.y + 2); }

  // A tile whose middle lies off the card.
  ShapePoint offTheCard(const ShapeView& view, const Shape& card) {
    for (const auto& shape : view.shapes()) {
      const auto at = inside(shape);
      if (shape.hittable && sceneRole(shape.id.role) == SceneRole::Piece && !within(card.rect, at.x, at.y)) {
        return at;
      }
    }
    TEST_CHECK(false);
    return {};
  }

  // r holds seven files and a small folder, so the small folder's card covers some of its neighbors. Ids: 0 r, 1 to
  // 7 the files, 8 small, 9 s1.txt and 10 s2.txt (in small).
  Tree smallFolderTree() {
    std::vector<luciole::test::WorkbenchRow> rows{{.parent = -1, .name = "r", .folder = true, .items = 8, .files = 9}};
    static const std::array<std::string, 7> kNames{"f1.txt", "f2.txt", "f3.txt", "f4.txt",
                                                   "f5.txt", "f6.txt", "f7.txt"};
    for (const auto& name : kNames) {
      rows.push_back({.parent = 0, .name = name, .bytes = 1500});
    }
    rows.push_back({.parent = 0, .name = "small", .folder = true, .bytes = 200, .items = 2, .files = 2});
    rows.push_back({.parent = 8, .name = "s1.txt", .bytes = 150});
    rows.push_back({.parent = 8, .name = "s2.txt", .bytes = 50});
    return luciole::test::workbenchTree(rows, 6000);
  }

  void aRestingPointerOpensThePeek() {
    constexpr NodeId kSmall = 8;
    constexpr NodeId kInside = 9;
    const auto inputs = inputsFor(smallFolderTree(), 1);
    Rig rig(WorkbenchInstrument::Nest);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto before = labelsShown(view);
    const auto at = pointOf(view, kSmall);
    rig.dispatcher.pointerEnter(at.x, at.y, 1);
    settle(rig, inputs);
    TEST_CHECK(cardOf(view) == nullptr); // not before the dwell
    std::this_thread::sleep_for(kDwell);
    tickUntil(rig, inputs, [&] { return cardOf(view) != nullptr; });
    rig.finishTransition();
    rig.frame(inputs);
    const auto* card = cardOf(view);
    TEST_CHECK(card->id.node == kSmall);
    const auto* entry = shapeOf(view, kInside);
    TEST_CHECK(entry != nullptr && within(card->rect, inside(*entry).x, inside(*entry).y));
    // The card hides the labels it covers, and it covers some.
    const auto after = labelsShown(view);
    std::size_t covered = 0;
    for (const auto& label : before) {
      if (!meets(label, card->rect)) {
        continue;
      }
      ++covered;
      TEST_CHECK(std::ranges::none_of(after, [&label](const ShownLabel& now) {
        return now.text == label.text && std::abs(now.x - label.x) < 0.5F && std::abs(now.y - label.y) < 0.5F;
      }));
    }
    TEST_CHECK(covered >= 2);
    // Moving over the card keeps it; moving off it closes it.
    const auto onCard = inside(*entry);
    rig.dispatcher.pointerMotion(onCard.x, onCard.y, 1);
    rig.frame(inputs);
    TEST_CHECK(cardOf(view) != nullptr && rig.hovers.back() == kInside);
    const auto off = offTheCard(view, *cardOf(view));
    rig.dispatcher.pointerMotion(off.x, off.y, 1);
    settle(rig, inputs);
    TEST_CHECK(cardOf(view) == nullptr);
  }

  void movingOnRestartsTheDwell() {
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Nest);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto a = pointOf(view, kA);
    const auto c = pointOf(view, kC);
    rig.dispatcher.pointerEnter(a.x, a.y, 1);
    std::this_thread::sleep_for(kDwell / 2);
    rig.dispatcher.pointerMotion(c.x, c.y, 1);
    std::this_thread::sleep_for(kDwell / 2 + std::chrono::milliseconds(50));
    settle(rig, inputs);
    TEST_CHECK(cardOf(view) == nullptr);
    std::this_thread::sleep_for(kDwell / 2);
    tickUntil(rig, inputs, [&] { return cardOf(view) != nullptr; });
    TEST_CHECK(cardOf(view)->id.node == kC);
    // Leaving the view closes it.
    rig.dispatcher.pointerLeave();
    settle(rig, inputs);
    TEST_CHECK(cardOf(view) == nullptr);
    // A file tile starts no dwell.
    const auto file = pointOf(view, kFile);
    rig.dispatcher.pointerEnter(file.x, file.y, 1);
    std::this_thread::sleep_for(kDwell);
    settle(rig, inputs);
    TEST_CHECK(cardOf(view) == nullptr);
  }

  void spaceOpensThePeekAndArrowsWalkIt() {
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Nest);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    rig.view->setCursor(kA);
    rig.frame(inputs);
    rig.key(XKB_KEY_space, ' ');
    tickUntil(rig, inputs, [&] { return cardOf(view) != nullptr; });
    TEST_CHECK(rig.requests.empty());
    // The arrows move inside the card, not the list's cursor.
    rig.key(XKB_KEY_Right);
    rig.frame(inputs);
    TEST_CHECK(rig.requests.empty() && marked(view, kX));
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.requests.size() == 1);
    TEST_CHECK(rig.requests[0].kind == PartRequestKind::Go && rig.requests[0].node == kX);
    rig.frame(inputs);
    TEST_CHECK(cardOf(view) == nullptr);
    // Space opens and closes it; another key closes it and does its own work.
    rig.key(XKB_KEY_space, ' ');
    tickUntil(rig, inputs, [&] { return cardOf(view) != nullptr; });
    rig.key(XKB_KEY_space, ' ');
    rig.frame(inputs);
    TEST_CHECK(cardOf(view) == nullptr);
    rig.key(XKB_KEY_space, ' ');
    tickUntil(rig, inputs, [&] { return cardOf(view) != nullptr; });
    rig.key(XKB_KEY_Home);
    rig.frame(inputs);
    TEST_CHECK(cardOf(view) == nullptr);
    TEST_CHECK(rig.requests.size() == 2 && rig.requests[1].kind == PartRequestKind::Select);
    // Space on a file opens nothing.
    rig.view->setCursor(kFile);
    rig.frame(inputs);
    rig.key(XKB_KEY_space, ' ');
    settle(rig, inputs);
    TEST_CHECK(cardOf(view) == nullptr);
  }

  void anOpenPeekFollowsItsFolder() {
    const auto first = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Nest);
    rig.show(first);
    const auto& view = rig.view->shapeView();
    rig.view->setCursor(kA);
    rig.frame(first);
    rig.key(XKB_KEY_space, ' ');
    tickUntil(rig, first, [&] { return cardOf(view) != nullptr; });
    // A new snapshot of the same folder keeps the card; entering another folder closes it.
    const auto second = inputsFor(balancedTree(), 2);
    rig.show(second);
    settle(rig, second);
    TEST_CHECK(cardOf(view) != nullptr && cardOf(view)->id.node == kA);
    const auto inside = inputsFor(enteredAt(kC), 3);
    rig.show(inside);
    settle(rig, inside);
    TEST_CHECK(cardOf(view) == nullptr);
  }

  void theWheelAndAClickCloseThePeek() {
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Nest);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    rig.view->setCursor(kA);
    rig.frame(inputs);
    rig.key(XKB_KEY_space, ' ');
    tickUntil(rig, inputs, [&] { return cardOf(view) != nullptr; });
    rig.wheel(pointOf(view, kFile), 1);
    rig.frame(inputs);
    TEST_CHECK(cardOf(view) == nullptr);
    rig.key(XKB_KEY_space, ' ');
    tickUntil(rig, inputs, [&] { return cardOf(view) != nullptr; });
    rig.finishTransition();
    rig.frame(inputs);
    // The card's head enters the folder it shows.
    const auto* card = cardOf(view);
    rig.click({.x = card->rect.x + 4, .y = card->rect.y + 4}, BTN_LEFT);
    TEST_CHECK(!rig.requests.empty() && rig.requests.back().kind == PartRequestKind::Go);
    TEST_CHECK(rig.requests.back().node == kA);
    rig.frame(inputs);
    TEST_CHECK(cardOf(view) == nullptr);
  }

  void theMoreTileIsTheLastStop() {
    const auto inputs = inputsFor(manyFiles(300), 1);
    Rig rig(WorkbenchInstrument::Nest);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    TEST_CHECK(shapeOf(view, 0, SceneRole::More) != nullptr);
    TEST_CHECK(!marked(view, 0));
    rig.key(XKB_KEY_End);
    rig.frame(inputs);
    TEST_CHECK(rig.requests.empty() && marked(view, 0));
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.requests.size() == 1);
    TEST_CHECK(rig.requests[0].kind == PartRequestKind::ShowMore && rig.requests[0].node == 0);
    // An arrow leaves the stop for a tile, which the list's cursor then takes.
    rig.key(XKB_KEY_Up);
    TEST_CHECK(rig.requests.size() == 2 && rig.requests[1].kind == PartRequestKind::Select);
    rig.view->setCursor(rig.requests[1].node);
    rig.frame(inputs);
    TEST_CHECK(!marked(view, 0) && marked(view, rig.requests[1].node));
    // The list's cursor moving elsewhere also ends the stop.
    rig.key(XKB_KEY_End);
    rig.frame(inputs);
    TEST_CHECK(marked(view, 0));
    const auto other = std::ranges::find_if(view.shapes(), [&](const Shape& shape) {
      return shape.hittable && sceneRole(shape.id.role) == SceneRole::Piece && shape.id.node != rig.requests[1].node;
    });
    TEST_CHECK(other != view.shapes().end());
    const auto moved = other->id.node;
    rig.view->setCursor(moved);
    rig.frame(inputs);
    TEST_CHECK(!marked(view, 0));
    // End from there rings the stop again.
    rig.key(XKB_KEY_End);
    rig.frame(inputs);
    TEST_CHECK(marked(view, 0) && !marked(view, moved));
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.requests.size() == 3 && rig.requests[2].kind == PartRequestKind::ShowMore);
  }

  void tooltipsDescribeTheEntryUnderThePointer() {
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Nest);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto at = pointOf(view, kFile);
    rig.dispatcher.pointerEnter(at.x, at.y, 1);
    const auto content = view.inputArea().tooltipContent();
    const auto* text = std::get_if<std::string>(&content);
    TEST_CHECK(text != nullptr && text->starts_with("b.txt\n300 bytes") && text->contains("TXT"));
    rig.dispatcher.pointerLeave();
    TEST_CHECK(std::holds_alternative<std::monostate>(view.inputArea().tooltipContent()));
  }

  // The tiles show first and their texture follows from a worker, built for the snapshot shown.
  void theTextureFollowsTheTiles() {
    auto changed = balancedTree();
    changed.nodes[6].bytes = 20; // y.png in a
    const auto first = inputsFor(balancedTree(), 1);
    const auto second = inputsFor(changed, 2);
    Rig rig(WorkbenchInstrument::Nest);
    rig.frame(first);
    rig.frame(second);
    const auto& view = rig.view->shapeView();
    const auto patches = [&view] {
      std::vector<Shape> found;
      for (const auto& shape : view.shapes()) {
        if (shape.id.node == kA && sceneRole(shape.id.role) == SceneRole::Detail) {
          found.push_back(shape);
        }
      }
      return found;
    };
    rig.until(second, [&] { return rig.view->shownSnapshot().serial == 2 && patches().size() == 2; });
    rig.finishTransition();
    settle(rig, second);
    const auto scene = buildNestScene(*second.snapshot.tree, {}, kWidth, kHeight);
    NestTextureSources sources;
    sources.emplace(kA, nestTextureSource(*second.snapshot.tree, {}, kA, {}));
    std::optional<std::size_t> tile;
    for (std::size_t i = 0; i < scene.pieces.size(); ++i) {
      tile = scene.pieces[i].node == kA && scene.pieces[i].kind == NestPieceKind::Tile ? std::optional(i) : tile;
    }
    TEST_CHECK(tile.has_value());
    const auto expected = nestTexturePatches(scene, sources)[*tile];
    const auto drawnPatches = patches();
    TEST_CHECK(drawnPatches.size() == expected.size());
    for (std::size_t i = 0; i < std::min(expected.size(), drawnPatches.size()); ++i) {
      TEST_NEAR(drawnPatches[i].rect.width, static_cast<float>(expected[i].rect.w - 2), 1e-3F);
      TEST_NEAR(drawnPatches[i].rect.height, static_cast<float>(expected[i].rect.h - 2), 1e-3F);
    }
  }

  // The sentence of an empty folder sits in the empty core, inside the rings.
  void theNoteSitsInTheCore() {
    const auto inputs = inputsFor(enteredAt(8), 1);
    Rig rig(WorkbenchInstrument::Nest);
    rig.until(inputs, [&] { return !visibleLabels(rig.view->shapeView(), "This folder is empty").empty(); });
    const auto scene = buildNestScene(*inputs.snapshot.tree, {}, kWidth, kHeight);
    TEST_CHECK(!scene.rings.empty());
    const auto* note = visibleLabels(rig.view->shapeView(), "This folder is empty").front();
    TEST_NEAR(note->maxWidth(), static_cast<float>(scene.core.w - 24), 0.5F);
  }

  // A part that goes while its layout job and its dwell are pending leaves no timer behind, and the job's late reply
  // touches nothing of it.
  void goingWithWorkPendingIsSafe() {
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Nest);
    rig.show(inputs);
    const auto at = pointOf(rig.view->shapeView(), kA);
    rig.dispatcher.pointerEnter(at.x, at.y, 1);
    rig.frame(inputsFor(balancedTree(), 2));
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() >= 0); // the dwell runs
    rig.dispatcher.setSceneRoot(nullptr);
    rig.root.removeChild(rig.view);
    rig.view = nullptr;
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() < 0);
    std::this_thread::sleep_for(kDwell);
    for (int turn = 0; turn < 50; ++turn) {
      TimerManager::instance().tick();
      pump(1);
    }
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() < 0);
  }
} // namespace

int main() {
  aRestingPointerOpensThePeek();
  movingOnRestartsTheDwell();
  spaceOpensThePeekAndArrowsWalkIt();
  anOpenPeekFollowsItsFolder();
  theWheelAndAClickCloseThePeek();
  theMoreTileIsTheLastStop();
  tooltipsDescribeTheEntryUnderThePointer();
  theTextureFollowsTheTiles();
  theNoteSitsInTheCore();
  goingWithWorkPendingIsSafe();
  std::println("nest part checks passed");
  return 0;
}
