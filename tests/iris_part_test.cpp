#include "browser/workbench_snapshot.h"
#include "fs/location.h"
#include "instrument_rig.h"
#include "ui/workbench/kept_snapshots.h"
#include "ui/workbench/snapshot_places.h"
#include "workbench/iris_hit.h"
#include "workbench_iris_fixture.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  using namespace luciole;
  using namespace luciole::test::instrument;

  bool hasShape(const ShapeView& view, NodeId node, SceneRole role) {
    return std::ranges::any_of(view.shapes(), [&](const Shape& shape) {
      return shape.id.node == node && sceneRole(shape.id.role) == role;
    });
  }

  // Visible labels whose text starts with `prefix`.
  std::vector<std::string> labelsStarting(const ShapeView& view, const std::string& prefix) {
    std::vector<std::string> found;
    for (const auto& child : view.labelLayer().children()) {
      const auto* label = dynamic_cast<const TextNode*>(child.get());
      if (label != nullptr && label->visible() && label->text().starts_with(prefix)) {
        found.push_back(label->text());
      }
    }
    return found;
  }

  void hover(Rig& rig, ShapePoint at) { rig.dispatcher.pointerMotion(at.x, at.y, 1); }

  const Shape* chipOf(const ShapeView& view, std::size_t index) {
    const auto shapes = view.shapes();
    const auto found = std::ranges::find_if(shapes, [index](const Shape& shape) {
      return shape.id == workbench::iris::chipId(index) && shape.hittable;
    });
    return found == shapes.end() ? nullptr : &*found;
  }

  // 60 files: the dial shows a few and "+N more", the last stop in angular order.
  void endRestsOnMoreAndEnterListsIt() {
    const auto files = luciole::test::iris::fileFolder("f");
    const auto inputs = inputsFor(files.tree, 1);
    Rig rig(WorkbenchInstrument::Iris);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    TEST_CHECK(!hasShape(view, 0, SceneRole::Mark));
    rig.key(XKB_KEY_End);
    TEST_CHECK(rig.requests.empty());
    rig.frame(inputs);
    TEST_CHECK(hasShape(view, 0, SceneRole::Mark));
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.requests.size() == 1);
    TEST_CHECK(rig.requests[0].kind == PartRequestKind::ShowMore && rig.requests[0].node == 0);
    // Up leaves "+N more" for the entry before it.
    rig.key(XKB_KEY_Up);
    TEST_CHECK(rig.requests.size() == 2 && rig.requests[1].kind == PartRequestKind::Select);
    const auto entry = rig.requests[1].node;
    TEST_CHECK(workbench::hasNode(files.tree, entry) && entry != 0);
    rig.frame(inputs);
    TEST_CHECK(!hasShape(view, 0, SceneRole::Mark));
    rig.view->setCursor(entry);
    rig.frame(inputs);
    TEST_CHECK(hasShape(view, entry, SceneRole::Mark));
    // Back on "+N more", a cursor moved from the list ends the stop.
    rig.key(XKB_KEY_End);
    rig.frame(inputs);
    TEST_CHECK(hasShape(view, 0, SceneRole::Mark));
    const NodeId other = files.tree.children[0];
    rig.view->setCursor(other);
    rig.frame(inputs);
    TEST_CHECK(!hasShape(view, 0, SceneRole::Mark) && hasShape(view, other, SceneRole::Mark));
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.requests.back().kind == PartRequestKind::Open && rig.requests.back().node == other);
  }

  // Ids of balancedTree: 0 r, 1 a, 5 x (in a). An ancestor row goes up to it; a trail chip goes back down.
  void stackRowsAndChipsGoWhereTheyName() {
    Rig rig(WorkbenchInstrument::Iris);
    const auto deep = inputsFor(enteredAt(5), 1);
    rig.show(deep);
    rig.finishTransition();
    rig.frame(deep);
    const auto& view = rig.view->shapeView();
    const auto* row = shapeOf(view, 0, SceneRole::Stack);
    TEST_CHECK(row != nullptr && row->rect.width < 60);
    // The stack rows and the hub are buttons: hovering them points at no entry in the other panes.
    const auto* hub = shapeOf(view, 5, SceneRole::Hub);
    TEST_CHECK(hub != nullptr);
    hover(rig, inside(*hub));
    TEST_CHECK(!rig.hovers.empty() && rig.hovers.back() == workbench::kNoNode);
    hover(rig, inside(*row));
    TEST_CHECK(!rig.hovers.empty() && rig.hovers.back() == workbench::kNoNode);
    rig.click(inside(*row), BTN_LEFT);
    TEST_CHECK(rig.requests.size() == 1);
    TEST_CHECK(rig.requests[0].kind == PartRequestKind::Go && rig.requests[0].node == 0);
    auto top = inputsFor(balancedTree(), 2);
    top.trail.folders = {1, 5};
    rig.show(top);
    rig.finishTransition();
    rig.frame(top);
    // The trail is a, then x: one chip each.
    TEST_CHECK(chipOf(view, 0) != nullptr && chipOf(view, 2) == nullptr);
    const auto* chip = chipOf(view, 1);
    TEST_CHECK(chip != nullptr);
    hover(rig, inside(*chip));
    const auto tip = view.inputArea().tooltipContent();
    TEST_CHECK(std::holds_alternative<std::string>(tip) && std::get<std::string>(tip) == "r/a/x");
    // A chip is a button: hovering it points at no entry in the other panes.
    TEST_CHECK(!rig.hovers.empty() && rig.hovers.back() == workbench::kNoNode);
    rig.click(inside(*chip), BTN_LEFT);
    TEST_CHECK(rig.requests.size() == 2);
    TEST_CHECK(rig.requests[1].kind == PartRequestKind::Go && rig.requests[1].node == 5);
  }

  // The hub offers the trail it is given, and a new trail for the same snapshot changes its chips.
  void chipsFollowTheTrailGiven() {
    Rig rig(WorkbenchInstrument::Iris);
    auto inputs = inputsFor(balancedTree(), 1);
    inputs.trail.folders = {1, 5};
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    TEST_CHECK(chipOf(view, 0) != nullptr && chipOf(view, 1) != nullptr && chipOf(view, 2) == nullptr);
    TEST_CHECK(visibleLabels(view, "x").size() == 1);
    inputs.trail.folders = {4};
    rig.frame(inputs);
    TEST_CHECK(chipOf(view, 0) != nullptr && chipOf(view, 1) == nullptr);
    TEST_CHECK(visibleLabels(view, "x").empty());
    inputs.trail.folders = {};
    rig.frame(inputs);
    TEST_CHECK(chipOf(view, 0) == nullptr);
  }

  // The row of "+N more" stands for no single entry: hovering it points at none in the other panes, while an entry's
  // row points at its entry.
  void theMoreRowPointsAtNoEntry() {
    const auto files = luciole::test::iris::fileFolder("f");
    const auto inputs = inputsFor(files.tree, 1);
    Rig rig(WorkbenchInstrument::Iris, 1400, 737);
    rig.show(inputs);
    rig.finishTransition();
    rig.frame(inputs);
    const auto& view = rig.view->shapeView();
    const NodeId first = files.tree.children[0];
    const auto* entry = shapeOf(view, first, SceneRole::Row);
    const auto* more = shapeOf(view, 0, SceneRole::Row);
    TEST_CHECK(entry != nullptr && more != nullptr);
    hover(rig, inside(*entry));
    TEST_CHECK(!rig.hovers.empty() && rig.hovers.back() == first);
    hover(rig, inside(*more));
    TEST_CHECK(rig.hovers.back() == workbench::kNoNode);
  }

  // The list of "+N more" opens where its piece is: from its row on a full dial, from the hub when no row shows.
  void enterOnMoreOpensAtThePiece() {
    const auto files = luciole::test::iris::fileFolder("f");
    const auto inputs = inputsFor(files.tree, 1);
    for (const auto& [width, height] : {std::pair{1400.0F, 737.0F}, {340.0F, 534.0F}}) {
      Rig rig(WorkbenchInstrument::Iris, width, height);
      rig.show(inputs);
      rig.finishTransition();
      rig.frame(inputs);
      rig.key(XKB_KEY_End);
      rig.frame(inputs);
      rig.key(XKB_KEY_Return);
      TEST_CHECK(rig.requests.size() == 1 && rig.requests[0].kind == PartRequestKind::ShowMore);
      const auto& view = rig.view->shapeView();
      const auto* row = shapeOf(view, 0, SceneRole::Row);
      const auto* piece = row != nullptr ? row : shapeOf(view, 0, SceneRole::Hub);
      TEST_CHECK(piece != nullptr && (row != nullptr) == (width > 1000));
      const auto at = inside(*piece);
      TEST_NEAR(rig.requests[0].x, at.x, 1e-3);
      TEST_NEAR(rig.requests[0].y, at.y, 1e-3);
    }
  }

  // Opened at r after a visit to "small", which falls into "+N more" among 60 files: the list of that piece names
  // "small" and holds as many entries as the piece says, though the hub shows a chip for it.
  void aTrailFolderInMoreStaysInItsList() {
    const auto files = luciole::test::iris::fileFolder("f");
    std::vector<luciole::test::WorkbenchRow> rows{
        {.parent = -1, .name = "r", .folder = true, .bytes = 1.9e6, .items = 61}
    };
    for (std::size_t i = 0; i < files.names.size(); ++i) {
      rows.push_back({.parent = 0, .name = files.names[i], .bytes = (60.0 - static_cast<double>(i)) * 1000});
    }
    rows.push_back({.parent = 0, .name = "small", .folder = true, .bytes = 10, .items = 0});
    constexpr NodeId kSmall = 61;
    const auto snapshotAt = [&rows](NodeId folder, std::uint64_t serial, const char* path) {
      auto tree = luciole::test::workbenchTree(rows, 0);
      tree.root = folder;
      tree.ancestors = parentChain(tree, folder);
      return std::make_shared<const WorkbenchSnapshot>(WorkbenchSnapshot{
          .serial = serial,
          .location = Location::fromPath(path),
          .settings = {},
          .tree = std::move(tree),
          .jump = {},
          .jumpLocations = {},
          .shares = {},
          .total = 0,
          .error = std::nullopt,
      });
    };
    const auto inputsOf = [](const std::shared_ptr<const WorkbenchSnapshot>& placed) {
      return PartInputs{
          .snapshot = {.tree = {placed, &placed->tree}, .serial = placed->serial}, .settings = {}, .palette = {}
      };
    };
    Rig rig(WorkbenchInstrument::Iris, 1400, 737);
    const auto entered = snapshotAt(kSmall, 1, "/r/small");
    rig.show(inputsOf(entered));
    const auto top = snapshotAt(0, 2, "/r");
    auto inputs = inputsOf(top);
    inputs.trail.folders = {kSmall};
    rig.show(inputs);
    rig.finishTransition();
    rig.frame(inputs);
    const auto& view = rig.view->shapeView();
    TEST_CHECK(visibleLabels(view, "small").size() == 1);
    const auto more = labelsStarting(view, "+");
    TEST_CHECK(more.size() == 1 && more[0].ends_with(" more"));
    KeptSnapshots kept;
    kept.add(placeSnapshot(top));
    const auto merged = kept.mergedIn(inputs.snapshot, 0, view.shapes(), {}, false, 100);
    TEST_CHECK(std::format("+{} more", merged.count) == more[0]);
    TEST_CHECK(std::ranges::any_of(merged.listed, [](const MergedEntry& entry) { return entry.name == "small"; }));
  }

  // Ids of balancedTree opened at a: 5 x, 6 y.png.
  void tooltipsNameWhatIsUnderThePointer() {
    Rig rig(WorkbenchInstrument::Iris);
    const auto inputs = inputsFor(enteredAt(1), 1);
    rig.show(inputs);
    rig.finishTransition();
    rig.frame(inputs);
    const auto& view = rig.view->shapeView();
    hover(rig, pointOf(view, 5));
    const auto piece = view.inputArea().tooltipContent();
    TEST_CHECK(std::holds_alternative<std::vector<TooltipRow>>(piece));
    const auto& rows = std::get<std::vector<TooltipRow>>(piece);
    TEST_CHECK(rows.size() == 3 && rows[0].key == "Name" && rows[0].value == "x");
    TEST_CHECK(rows[1].key == "Size" && rows[2].key == "Share" && rows[2].value.ends_with('%'));
    hover(rig, inside(*shapeOf(view, 1, SceneRole::Hub)));
    const auto hub = view.inputArea().tooltipContent();
    TEST_CHECK(std::holds_alternative<std::string>(hub) && std::get<std::string>(hub) == "Up to r");
    hover(rig, inside(*shapeOf(view, 0, SceneRole::Stack)));
    const auto stack = view.inputArea().tooltipContent();
    TEST_CHECK(std::holds_alternative<std::string>(stack) && std::get<std::string>(stack) == "r");
  }

  // A compact dial's readout names the folder, then what the pointer is over.
  void theReadoutFollowsThePointer() {
    Rig rig(WorkbenchInstrument::Iris, 340, 534);
    const auto inputs = inputsFor(enteredAt(1), 1);
    rig.show(inputs);
    rig.finishTransition();
    rig.frame(inputs);
    const auto& view = rig.view->shapeView();
    // The readout is the name with its size and count whole at the right end of the line: the folder's, then the
    // entry under the pointer's.
    TEST_CHECK(visibleLabels(view, "400 bytes  ·  2 items").size() == 1);
    TEST_CHECK(visibleLabels(view, "200 bytes  ·  0 items").empty());
    hover(rig, pointOf(view, 5));
    rig.frame(inputs);
    TEST_CHECK(visibleLabels(view, "200 bytes  ·  0 items").size() == 1);
    TEST_CHECK(visibleLabels(view, "400 bytes  ·  2 items").empty());
  }

  // The hub writes an empty folder's note once, wrapped onto its lines, and the view adds none of its own; a hub too
  // small for text writes none.
  void emptyFoldersAreNotedOnce() {
    const auto inputs = inputsFor(enteredAt(8), 1);
    const auto noted = [](const ShapeView& view) { return labelsStarting(view, "This folder").size(); };
    Rig wide(WorkbenchInstrument::Iris, 1400, 737);
    wide.show(inputs);
    TEST_CHECK(visibleLabels(wide.view->shapeView(), "This folder is empty").size() == 1);
    Rig rig(WorkbenchInstrument::Iris);
    rig.show(inputs);
    const auto lines = labelsStarting(rig.view->shapeView(), "This folder");
    TEST_CHECK(lines.size() == 1 && lines[0] != "This folder is empty" && !lines[0].contains("…"));
    // 240 by 180 gives a hub of 23 px.
    Rig tiny(WorkbenchInstrument::Iris, 240, 180);
    tiny.show(inputs);
    TEST_CHECK(noted(tiny.view->shapeView()) == 0);
  }
} // namespace

int main() {
  endRestsOnMoreAndEnterListsIt();
  stackRowsAndChipsGoWhereTheyName();
  chipsFollowTheTrailGiven();
  theMoreRowPointsAtNoEntry();
  enterOnMoreOpensAtThePiece();
  aTrailFolderInMoreStaysInItsList();
  tooltipsNameWhatIsUnderThePointer();
  theReadoutFollowsThePointer();
  emptyFoldersAreNotedOnce();
  return 0;
}
