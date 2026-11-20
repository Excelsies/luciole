#include "instrument_rig.h"
#include "nest_rig.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/nest_scene.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// The Nest's peek card over other folder tiles, and the "+N more" stop across folders, driven through a real input
// dispatcher.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::test::instrument;
  using namespace luciole::test::nest;

  constexpr float kWide = 900;
  constexpr float kTall = 600;
  constexpr auto kDwell = std::chrono::milliseconds(kNestPeekDwellMs);
  constexpr NodeId kFirstFolder = 1;  // f0 in foldersAndSmall()
  constexpr NodeId kSecondFolder = 7; // f1
  constexpr NodeId kSmall = 25;       // s

  // r holds folders f0 to f3 of five files each, then s, a small folder whose card covers some of them. Ids: 0 r,
  // 6k + 1 folder fk with its files after it, 25 s, 26 s1.txt and 27 s2.txt.
  Tree foldersAndSmall() {
    std::vector<luciole::test::WorkbenchRow> rows{{.parent = -1, .name = "r", .folder = true, .items = 5, .files = 22}};
    std::deque<std::string> names;
    for (int folder = 0; folder < 4; ++folder) {
      names.push_back(std::format("f{}", folder));
      rows.push_back({.parent = 0, .name = names.back(), .folder = true, .bytes = 5000, .items = 5, .files = 5});
      addFiles(rows, names, static_cast<int>(rows.size()) - 1, 5, std::format("f{}_", folder));
    }
    rows.push_back({.parent = 0, .name = "s", .folder = true, .bytes = 300, .items = 2, .files = 2});
    rows.push_back({.parent = static_cast<int>(kSmall), .name = "s1.txt", .bytes = 200});
    rows.push_back({.parent = static_cast<int>(kSmall), .name = "s2.txt", .bytes = 100});
    return luciole::test::workbenchTree(rows, 6000);
  }

  // top holds a and b, `count` files each; the current folder is `folder`. Ids: 0 top, 1 a, count + 2 b.
  Tree twoFolders(NodeId folder, std::size_t count) {
    std::vector<luciole::test::WorkbenchRow> rows{{.parent = -1, .name = "top", .folder = true, .items = 2}};
    std::deque<std::string> names;
    for (const auto* name : {"a", "b"}) {
      const auto items = static_cast<double>(count);
      rows.push_back({.parent = 0, .name = name, .folder = true, .bytes = 1e6, .items = items, .files = items});
      addFiles(rows, names, static_cast<int>(rows.size()) - 1, count, name);
    }
    auto tree = luciole::test::workbenchTree(rows, 6000);
    tree.root = folder;
    tree.ancestors = parentChain(tree, folder);
    return tree;
  }

  // r holds `count` folders of one file each.
  Tree manyFolders(std::size_t count) {
    const auto items = static_cast<double>(count);
    std::vector<luciole::test::WorkbenchRow> rows{{.parent = -1, .name = "r", .folder = true, .items = items}};
    std::deque<std::string> names;
    for (std::size_t i = 0; i < count; ++i) {
      names.push_back(std::format("d{}", i));
      const double bytes = 1000.0 + static_cast<double>(i % 17) * 50;
      rows.push_back({.parent = 0, .name = names.back(), .folder = true, .bytes = bytes, .items = 1, .files = 1});
      addFiles(rows, names, static_cast<int>(rows.size()) - 1, 1, names.back());
    }
    return luciole::test::workbenchTree(rows, 6000);
  }

  struct Covered {
    ShapePoint at;
    NodeId beneath = kNoNode;
  };

  // A point well inside the card that lies over a folder tile other than s, from the tiles shown before it opened.
  std::optional<Covered> overAnotherFolder(const Tree& tree, const std::vector<Shape>& tiles, const Shape& card) {
    const auto& box = card.rect;
    for (const auto& tile : tiles) {
      const auto node = tile.id.node;
      if (node == kSmall || !hasNode(tree, node) || !tree.nodes[node].folder || tree.nodes[node].parent != tree.root) {
        continue;
      }
      const float left = std::max(tile.rect.x, box.x + 30) + 2;
      const float top = std::max(tile.rect.y, box.y + 40) + 2;
      const float right = std::min(tile.rect.x + tile.rect.width, box.x + box.width - 30) - 2;
      const float bottom = std::min(tile.rect.y + tile.rect.height, box.y + box.height - 30) - 2;
      if (left < right && top < bottom) {
        return Covered{.at = {.x = (left + right) / 2, .y = (top + bottom) / 2}, .beneath = node};
      }
    }
    return std::nullopt;
  }

  std::vector<Shape> pieces(const ShapeView& view) {
    std::vector<Shape> found;
    for (const auto& shape : view.shapes()) {
      if (shape.hittable && sceneRole(shape.id.role) == SceneRole::Piece) {
        found.push_back(shape);
      }
    }
    return found;
  }

  // foldersAndSmall() with s holding a folder: 25 s, 26 sd, 27 sd1.txt, 28 sd2.txt, 29 s1.txt.
  Tree foldersAndNested() {
    std::vector<luciole::test::WorkbenchRow> rows{{.parent = -1, .name = "r", .folder = true, .items = 5, .files = 23}};
    std::deque<std::string> names;
    for (int folder = 0; folder < 4; ++folder) {
      names.push_back(std::format("f{}", folder));
      rows.push_back({.parent = 0, .name = names.back(), .folder = true, .bytes = 5000, .items = 5, .files = 5});
      addFiles(rows, names, static_cast<int>(rows.size()) - 1, 5, std::format("f{}_", folder));
    }
    rows.push_back({.parent = 0, .name = "s", .folder = true, .bytes = 400, .items = 2, .files = 3});
    rows.push_back({.parent = 25, .name = "sd", .folder = true, .bytes = 300, .items = 2, .files = 2});
    rows.push_back({.parent = 26, .name = "sd1.txt", .bytes = 200});
    rows.push_back({.parent = 26, .name = "sd2.txt", .bytes = 100});
    rows.push_back({.parent = 25, .name = "s1.txt", .bytes = 100});
    return luciole::test::workbenchTree(rows, 6000);
  }

  // The tile the card draws for `node`.
  const Shape* cardTile(const ShapeView& view, const Shape& card, NodeId node) {
    const auto found = std::ranges::find_if(view.shapes(), [&](const Shape& shape) {
      const auto& box = shape.rect;
      return shape.id.node == node
          && shape.hittable
          && sceneRole(shape.id.role) == SceneRole::Piece
          && box.x >= card.rect.x
          && box.y >= card.rect.y
          && box.x + box.width <= card.rect.x + card.rect.width
          && box.y + box.height <= card.rect.y + card.rect.height;
    });
    return found == view.shapes().end() ? nullptr : &*found;
  }

  // Turning the wheel up over a folder tile of the card enters that folder. Over the card's head or a file it does
  // nothing and the card stays; the tiles beneath the card are out of reach.
  void theWheelOverTheCardEntersOnlyItsFolders() {
    constexpr NodeId kNested = 26;
    constexpr NodeId kNestedFile = 29;
    const auto inputs = inputsFor(foldersAndNested(), 1);
    Rig rig(WorkbenchInstrument::Nest, kWide, kTall);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    rig.view->setCursor(kSmall);
    rig.frame(inputs);
    rig.key(XKB_KEY_space, ' ');
    tickUntil(rig, inputs, [&] { return cardOf(view) != nullptr; });
    const auto* card = cardOf(view);
    TEST_CHECK(card != nullptr && card->id.node == kSmall);
    if (card == nullptr) {
      return;
    }
    rig.requests.clear();
    rig.wheel({.x = card->rect.x + card->rect.width / 2, .y = card->rect.y + 4}, -1);
    rig.frame(inputs);
    TEST_CHECK(rig.requests.empty() && cardOf(view) != nullptr);
    const auto* file = cardTile(view, *cardOf(view), kNestedFile);
    TEST_CHECK(file != nullptr);
    if (file != nullptr) {
      rig.wheel(inside(*file), -1);
      rig.frame(inputs);
      TEST_CHECK(rig.requests.empty() && cardOf(view) != nullptr);
    }
    const auto* folder = cardTile(view, *cardOf(view), kNested);
    TEST_CHECK(folder != nullptr);
    if (folder == nullptr) {
      return;
    }
    rig.wheel(inside(*folder), -1);
    TEST_CHECK(rig.requests.size() == 1);
    TEST_CHECK(!rig.requests.empty() && rig.requests[0].kind == PartRequestKind::Go);
    TEST_CHECK(!rig.requests.empty() && rig.requests[0].node == kNested);
    rig.frame(inputs);
    TEST_CHECK(cardOf(view) == nullptr);
  }

  // A key that closes a card the pointer rests on leaves the tile beneath without a card until the pointer moves.
  void aKeyClosingTheCardOpensNoOther() {
    const auto inputs = inputsFor(foldersAndSmall(), 1);
    const auto& tree = *inputs.snapshot.tree;
    Rig rig(WorkbenchInstrument::Nest, kWide, kTall);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto tiles = pieces(view);
    const auto at = pointOf(view, kSmall);
    rig.dispatcher.pointerEnter(at.x, at.y, 1);
    std::this_thread::sleep_for(kDwell);
    tickUntil(rig, inputs, [&] { return cardOf(view) != nullptr; });
    const auto over = overAnotherFolder(tree, tiles, *cardOf(view));
    TEST_CHECK(over.has_value());
    if (!over) {
      return;
    }
    rig.dispatcher.pointerMotion(over->at.x, over->at.y, 1);
    settle(rig, inputs);
    TEST_CHECK(cardOf(view) != nullptr && cardOf(view)->id.node == kSmall);
    rig.key(XKB_KEY_Home);
    settle(rig, inputs);
    TEST_CHECK(cardOf(view) == nullptr);
    std::this_thread::sleep_for(kDwell + std::chrono::milliseconds(20));
    settle(rig, inputs);
    TEST_CHECK(cardOf(view) == nullptr);
    // The pointer moving to another folder tile rests there as usual.
    const NodeId next = over->beneath == kFirstFolder ? kSecondFolder : kFirstFolder;
    const auto there = pointOf(view, next);
    rig.dispatcher.pointerMotion(there.x, there.y, 1);
    std::this_thread::sleep_for(kDwell);
    tickUntil(rig, inputs, [&] { return cardOf(view) != nullptr; });
    TEST_CHECK(cardOf(view)->id.node == next);
  }

  // The stop rests on "+N more" of the folder it was put in: a new snapshot of that folder keeps it, another folder
  // starts without it.
  void theMoreStopStaysInItsFolder() {
    constexpr std::size_t kCount = 300;
    constexpr NodeId kFolderA = 1;
    constexpr NodeId kFolderB = kCount + 2;
    const auto inA = inputsFor(twoFolders(kFolderA, kCount), 1);
    Rig rig(WorkbenchInstrument::Nest);
    rig.show(inA);
    const auto& view = rig.view->shapeView();
    TEST_CHECK(shapeOf(view, kFolderA, SceneRole::More) != nullptr);
    rig.key(XKB_KEY_End);
    rig.frame(inA);
    TEST_CHECK(marked(view, kFolderA));
    const auto again = inputsFor(twoFolders(kFolderA, kCount), 2);
    rig.show(again);
    settle(rig, again);
    TEST_CHECK(marked(view, kFolderA));
    const auto inB = inputsFor(twoFolders(kFolderB, kCount), 3);
    rig.show(inB);
    settle(rig, inB);
    TEST_CHECK(shapeOf(view, kFolderB, SceneRole::More) != nullptr);
    TEST_CHECK(!marked(view, kFolderB));
    rig.requests.clear();
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.requests.empty());
  }

  // On "+N more" no tile has the ring, so Space opens no card.
  void spaceOnTheMoreStopOpensNothing() {
    const auto inputs = inputsFor(manyFolders(300), 1);
    Rig rig(WorkbenchInstrument::Nest, kWide, kTall);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto shown = pieces(view);
    TEST_CHECK(!shown.empty() && shapeOf(view, 0, SceneRole::More) != nullptr);
    if (shown.empty()) {
      return;
    }
    const auto folder = shown.front().id.node;
    rig.view->setCursor(folder);
    rig.frame(inputs);
    rig.key(XKB_KEY_End);
    settle(rig, inputs);
    TEST_CHECK(marked(view, 0) && !marked(view, folder));
    rig.key(XKB_KEY_space, ' ');
    settle(rig, inputs);
    settle(rig, inputs);
    TEST_CHECK(cardOf(view) == nullptr);
    TEST_CHECK(marked(view, 0));
  }
} // namespace

int main() {
  theWheelOverTheCardEntersOnlyItsFolders();
  aKeyClosingTheCardOpensNoOther();
  theMoreStopStaysInItsFolder();
  spaceOnTheMoreStopOpensNothing();
  std::println("nest part card checks passed");
  return 0;
}
