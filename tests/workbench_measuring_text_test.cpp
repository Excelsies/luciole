#include "test_check.h"
#include "workbench/entry_text.h"
#include "workbench/flow_topology.h"
#include "workbench/iris_facts.h"
#include "workbench/jump_rank.h"
#include "workbench/measure.h"
#include "workbench/nest_labels.h"
#include "workbench/nest_scene.h"
#include "workbench_fixture.h"
#include "workbench_iris_fixture.h"

#include <format>
#include <string>
#include <variant>
#include <vector>

// A folder whose measure has not finished never states a zero it does not know: its size and count read "measuring…",
// in the hub, the readout, the tooltips and the labels every instrument and ribbon takes from measureLabel. A folder a
// walk cut short and left reads "not measured" at zero and "at least <value>" above it.
namespace {
  using namespace luciole::test::iris;
  using luciole::WorkbenchMeasure;
  using luciole::test::WorkbenchRow;

  constexpr const char* kMeasuring = "measuring…";
  constexpr const char* kNotMeasured = "not measured";

  // fx (listed, 1 entry) holding iris, a folder known only by its name; both unmeasured. Ids: 0 fx, 1 iris, 2 done.
  Tree unmeasuredTree() {
    auto tree = luciole::test::workbenchTree(
        {{.parent = -1, .name = "fx", .folder = true, .items = 3},
         {.parent = 0, .name = "iris", .folder = true},
         {.parent = 0, .name = "done", .folder = true, .bytes = 700, .items = 2, .files = 2}},
        0
    );
    tree.nodes[0].partial = true;
    tree.nodes[1].partial = true;
    return tree;
  }

  void labelsSayMeasuring() {
    const auto tree = unmeasuredTree();
    for (const NodeId id : {NodeId{0}, NodeId{1}}) {
      TEST_CHECK(measureLabel(tree, id, WorkbenchMeasure::Bytes) == kMeasuring);
      TEST_CHECK(measureLabel(tree, id, WorkbenchMeasure::Equal) == kMeasuring);
      TEST_CHECK(measureLabel(tree, id, WorkbenchMeasure::Files) == kMeasuring);
    }
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Items) == kMeasuring);
    // Known values stay: a count read from the listing, and a measured folder's size.
    TEST_CHECK(measureLabel(tree, 0, WorkbenchMeasure::Items) == "3 items");
    TEST_CHECK(measureLabel(tree, 2, WorkbenchMeasure::Bytes) == "700 bytes");
    TEST_CHECK(measureLabel(tree, 2, WorkbenchMeasure::Files) == "2 files");
  }

  // A finished empty folder is empty, and a partial total already above zero is shown as it stands.
  void zeroOfAFinishedFolderStays() {
    auto tree = unmeasuredTree();
    tree.nodes[1].partial = false;
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Bytes) == "0 bytes");
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Items) == "0 items");
    tree.nodes[1].partial = true;
    tree.nodes[1].bytes = 512;
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Bytes) == "512 bytes");
  }

  void theHubSaysMeasuring() {
    auto tree = unmeasuredTree();
    tree.root = 0;
    const auto scene = buildScene(tree, fullInput()).value();
    TEST_CHECK(scene.hub.total == kMeasuring);
    TEST_CHECK(scene.hub.items == "3 items");
    tree.nodes[0].items = 0;
    const auto empty = buildScene(tree, fullInput()).value();
    TEST_CHECK(empty.hub.total == kMeasuring && empty.hub.items == kMeasuring);
  }

  ShapeId pieceOf(NodeId node) { return {.node = node, .role = shapeRole(SceneRole::Piece)}; }

  void readoutAndTooltipSayMeasuring() {
    auto tree = unmeasuredTree();
    tree.root = 0;
    const auto scene = buildScene(tree, hostInput(340, 534, true)).value();
    ShapeMarks onIris;
    onIris.hovered = pieceOf(1);
    const auto line = readoutLine(tree, scene, onIris, CursorStop::Marks);
    TEST_CHECK(line.text == "iris" && line.facts == "measuring…");
    const auto tip = shapeTip(tree, scene, pieceOf(1));
    TEST_CHECK(!tip.rows.empty() && tip.rows[1].key == "Size" && tip.rows[1].value == kMeasuring);
    // A share beside a size that is still being measured is approximate, for the node and for its siblings.
    TEST_CHECK(tip.rows[2].key == "Share" && tip.rows[2].value.starts_with("about "));
    TEST_CHECK(shapeTip(tree, scene, pieceOf(2)).rows[2].value.starts_with("about "));
    ShapeMarks onDone;
    onDone.hovered = pieceOf(2);
    TEST_CHECK(readoutLine(tree, scene, onDone, CursorStop::Marks).facts == "700 bytes  ·  2 items");
  }

  // Ids: 0 fx (cut short, 3 entries), 1 iris (cut short, nothing known), 2 big (cut short, 2 KiB, 4 items, 3 files),
  // 3 done (finished), 4 live (still measuring, 2 KiB, 4 items, 3 files).
  Tree cutShortTree() {
    auto tree = luciole::test::workbenchTree(
        {{.parent = -1, .name = "fx", .folder = true, .items = 3},
         {.parent = 0, .name = "iris", .folder = true},
         {.parent = 0, .name = "big", .folder = true, .bytes = 2048, .items = 4, .files = 3},
         {.parent = 0, .name = "done", .folder = true, .bytes = 700, .items = 2, .files = 2},
         {.parent = 0, .name = "live", .folder = true, .bytes = 2048, .items = 4, .files = 3}},
        0
    );
    for (const NodeId id : {NodeId{0}, NodeId{1}, NodeId{2}}) {
      tree.nodes[id].lowerBound = true;
    }
    tree.nodes[4].partial = true;
    return tree;
  }

  void fiveWordings() {
    const auto tree = cutShortTree();
    for (const auto by : {WorkbenchMeasure::Bytes, WorkbenchMeasure::Equal, WorkbenchMeasure::Files}) {
      TEST_CHECK(measureLabel(tree, 1, by) == kNotMeasured);
    }
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Items) == kNotMeasured);
    TEST_CHECK(measureLabel(tree, 2, WorkbenchMeasure::Bytes) == "at least 2.0\u00A0KiB");
    TEST_CHECK(measureLabel(tree, 2, WorkbenchMeasure::Items) == "at least 4 items");
    TEST_CHECK(measureLabel(tree, 2, WorkbenchMeasure::Files) == "at least 3 files");
    TEST_CHECK(itemsLabel(tree, 2) == "at least 4 items" && itemsLabel(tree, 1) == kNotMeasured);
    TEST_CHECK(measureLabel(tree, 4, WorkbenchMeasure::Bytes) == "2.0\u00A0KiB");
    TEST_CHECK(measureLabel(tree, 4, WorkbenchMeasure::Items) == "4 items");
    TEST_CHECK(measureLabel(tree, 3, WorkbenchMeasure::Bytes) == "700 bytes");
    // Still running wins over cut short; a finished empty folder is empty.
    auto both = cutShortTree();
    both.nodes[1].partial = true;
    TEST_CHECK(measureLabel(both, 1, WorkbenchMeasure::Bytes) == kMeasuring);
    both.nodes[1].partial = false;
    both.nodes[1].lowerBound = false;
    TEST_CHECK(measureLabel(both, 1, WorkbenchMeasure::Bytes) == "0 bytes");
  }

  // The short prefix "≥ " stands for "at least " before a lower bound above zero and changes nothing else.
  void theShortPrefix() {
    const auto tree = cutShortTree();
    using enum PrefixForm;
    TEST_CHECK(measureLabel(tree, 2, WorkbenchMeasure::Bytes, Short) == "\u2265 2.0\u00A0KiB");
    TEST_CHECK(measureLabel(tree, 2, WorkbenchMeasure::Items, Short) == "\u2265 4 items");
    TEST_CHECK(measureLabel(tree, 1, WorkbenchMeasure::Bytes, Short) == kNotMeasured);
    TEST_CHECK(measureLabel(tree, 3, WorkbenchMeasure::Bytes, Short) == "700 bytes");
    TEST_CHECK(measureLabel(tree, 4, WorkbenchMeasure::Bytes, Short) == "2.0\u00A0KiB");
    TEST_CHECK(measureLabel(tree, 2, WorkbenchMeasure::Bytes) == measureLabel(tree, 2, WorkbenchMeasure::Bytes, Long));
    TEST_CHECK(stateText(MeasureState::LowerBound, 5, "5 KiB", Short) == "\u2265 5 KiB");
    TEST_CHECK(stateText(MeasureState::LowerBound, 5, "5 KiB", Long) == "at least 5 KiB");
    TEST_CHECK(stateText(MeasureState::LowerBound, 0, "0 bytes", Short) == kNotMeasured);
    TEST_CHECK(stateText(MeasureState::Measuring, 0, "0 bytes", Short) == "measuring\u2026");
    TEST_CHECK(stateText(MeasureState::Done, 5, "5 KiB", Short) == "5 KiB");
  }

  void hubReadoutAndTooltipsForACutShortFolder() {
    auto tree = cutShortTree();
    tree.root = 0;
    auto scene = buildScene(tree, fullInput()).value();
    TEST_CHECK(scene.hub.total == kNotMeasured && scene.hub.items == "at least 3 items");
    tree.nodes[0].items = 0;
    scene = buildScene(tree, fullInput()).value();
    TEST_CHECK(scene.hub.total == kNotMeasured && scene.hub.items == kNotMeasured);
    tree.root = 2;
    scene = buildScene(tree, fullInput()).value();
    TEST_CHECK(scene.hub.total == "at least 2.0\u00A0KiB" && scene.hub.items == "at least 4 items");

    tree.root = 0;
    scene = buildScene(tree, hostInput(340, 534, true)).value();
    ShapeMarks onIris;
    onIris.hovered = pieceOf(1);
    TEST_CHECK(readoutLine(tree, scene, onIris, CursorStop::Marks).facts == kNotMeasured);
    ShapeMarks onBig;
    onBig.hovered = pieceOf(2);
    TEST_CHECK(
        readoutLine(tree, scene, onBig, CursorStop::Marks).facts == "at least 2.0\u00A0KiB  ·  at least 4 items"
    );
    // A cut short folder says so in a sentence, so its tooltip is text: the size and the share are the table's values.
    const auto tip = shapeTip(tree, scene, pieceOf(1));
    TEST_CHECK(tip.rows.empty() && tip.text.starts_with(std::string("iris\n") + kNotMeasured + "  ·  about "));
    const auto bigTip = shapeTip(tree, scene, pieceOf(2));
    TEST_CHECK(bigTip.rows.empty() && bigTip.text.starts_with("big\nat least 2.0\u00A0KiB  ·  about "));
    // A finished folder beside finished siblings states its share plainly.
    auto finished = cutShortTree();
    for (auto& node : finished.nodes) {
      node.lowerBound = false;
      node.partial = false;
    }
    const auto finishedScene = buildScene(finished, hostInput(340, 534, true)).value();
    TEST_CHECK(!shapeTip(finished, finishedScene, pieceOf(2)).rows[2].value.starts_with("about "));
  }

  // The state is said once, never as a chain of contradicting facts.
  void tooltipSaysTheStateOnce() {
    auto tree = cutShortTree();
    tree.nodes[1].truncated = true;
    tree.nodes[4].items = 0;
    tree.nodes[4].bytes = 0;
    tree.nodes[4].files = 0;
    tree.nodes[4].truncated = true;
    const auto notMeasured = entryText(entryFacts(tree, 1));
    TEST_CHECK(notMeasured.starts_with("iris\nnot measured  ·  modified "));
    TEST_CHECK(
        !notMeasured.contains("not expanded") && notMeasured.find(kNotMeasured) == notMeasured.rfind(kNotMeasured)
    );
    const auto running = entryText(entryFacts(tree, 4));
    TEST_CHECK(running.starts_with("live\nmeasuring…  ·  modified "));
    TEST_CHECK(running.find(kMeasuring) == running.rfind(kMeasuring) && !running.contains("not expanded"));
    TEST_CHECK(entryText(entryFacts(tree, 2))
                   .starts_with("big\nat least 2.0\u00A0KiB  ·  at least 4 items  ·  at least 3 files"));
    // A folder with room to say more keeps its other states.
    tree.nodes[3].truncated = true;
    TEST_CHECK(entryText(entryFacts(tree, 3)).contains("not expanded"));
  }

  // Why a folder is not measured is said once, in the same words, by the Iris and the Nest tooltips: a folder that
  // reads "at least" gets the sentence of what was not counted, an exact one its rows.
  void tooltipsSayWhyAFolderIsNotMeasured() {
    auto tree = luciole::test::workbenchTree(
        {{.parent = -1, .name = "fx", .folder = true, .items = 4},
         {.parent = 0, .name = "mnt", .folder = true},
         {.parent = 0, .name = "locked", .folder = true},
         {.parent = 0, .name = "deep", .folder = true, .bytes = 2048, .items = 4, .files = 3},
         {.parent = 0, .name = "plain", .folder = true, .bytes = 700, .items = 2, .files = 2}},
        0
    );
    tree.nodes[1].otherFilesystem = true;
    tree.nodes[1].truncated = true;
    tree.nodes[1].lowerBound = true;
    tree.nodes[2].unreadable = true;
    tree.nodes[2].lowerBound = true;
    tree.nodes[3].truncated = true;
    tree.nodes[3].lowerBound = true;
    const auto scene = buildScene(tree, hostInput(340, 534, true)).value();
    const auto textOf = [&](NodeId id) {
      const auto tip = shapeTip(tree, scene, pieceOf(id));
      TEST_CHECK(tip.rows.empty());
      return tip.text;
    };
    TEST_CHECK(textOf(1) == entryText(entryFacts(tree, 1, &scene.settings)));
    TEST_CHECK(textOf(1).ends_with("\nNot counted: mnt is on another filesystem."));
    TEST_CHECK(textOf(1).starts_with("mnt\nnot measured"));
    TEST_CHECK(textOf(2).ends_with("\nNot counted: 1 unreadable folder (locked)."));
    TEST_CHECK(textOf(3).contains("not expanded") && textOf(3).ends_with("were not counted in full."));
    TEST_CHECK(shapeTip(tree, scene, pieceOf(4)).rows.size() == 3);
    // Nest says the same words, once, and not "not expanded" beside a size that says it is not measured.
    const auto mnt = entryText(entryFacts(tree, 1));
    TEST_CHECK(
        mnt.find("on another filesystem") == mnt.rfind("on another filesystem") && !mnt.contains("not expanded")
    );
    TEST_CHECK(
        entryText(entryFacts(tree, 2)).ends_with("\nNot counted: 1 unreadable folder (locked).")
        && entryText(entryFacts(tree, 3)).contains("not expanded")
    );
  }

  // The hub totals the folder, so its tooltip says why the total reads "at least" beside where a click goes.
  void theHubTooltipSaysWhyTheTotalIsALowerBound() {
    auto tree = luciole::test::workbenchTree(
        {{.parent = -1, .name = "fx", .folder = true, .bytes = 900, .items = 2},
         {.parent = 0, .name = "mnt", .folder = true},
         {.parent = 0, .name = "plain", .folder = true, .bytes = 700, .items = 2, .files = 2}},
        0
    );
    tree.nodes[1].otherFilesystem = true;
    tree.nodes[1].truncated = true;
    tree.nodes[1].lowerBound = true;
    tree.nodes[0].lowerBound = true;
    const auto scene = buildScene(tree, hostInput(340, 534, true)).value();
    const ShapeId hub{.node = 0, .role = shapeRole(SceneRole::Hub)};
    TEST_CHECK(shapeTip(tree, scene, hub).text == "Top of this view\nNot counted: mnt is on another filesystem.");
    tree.nodes[0].lowerBound = false;
    const auto exact = buildScene(tree, hostInput(340, 534, true)).value();
    TEST_CHECK(shapeTip(tree, exact, hub).text == "Top of this view");
  }

  // r holds a (7 files), then b and c, which fold into the More record; its value is the sum of b and c, measuring if
  // either is measuring, else a lower bound if either is cut short.
  void sumsCarryTheirParts() {
    const auto moreValue = [](bool bCut, bool bLive, bool cCut, bool cLive, WorkbenchMeasure measure) {
      auto tree = luciole::test::workbenchTree(
          {{.parent = -1, .name = "r", .folder = true, .files = 15},
           {.parent = 0, .name = "a", .folder = true, .bytes = 70, .items = 7, .files = 7},
           {.parent = 0, .name = "b", .folder = true, .bytes = 50, .items = 5, .files = 5},
           {.parent = 0, .name = "c", .folder = true, .bytes = 30, .items = 3, .files = 3}},
          0
      );
      tree.nodes[2].lowerBound = bCut;
      tree.nodes[2].partial = bLive;
      tree.nodes[3].lowerBound = cCut;
      tree.nodes[3].partial = cLive;
      const auto topology = buildFlowTopology(tree, 0, {}, {.firstMax = 1});
      for (const auto index : topology.columns[1]) {
        if (topology.records[index].kind == FlowRecordKind::More) {
          return flowRecordValue(tree, topology, index, measure);
        }
      }
      return std::string("no more record");
    };
    constexpr auto kBytes = WorkbenchMeasure::Bytes;
    TEST_CHECK(moreValue(false, false, false, false, kBytes) == "80 bytes");
    TEST_CHECK(moreValue(true, false, false, false, kBytes) == "at least 80 bytes");
    TEST_CHECK(moreValue(false, false, true, false, WorkbenchMeasure::Items) == "at least 8 items");
    TEST_CHECK(moreValue(false, false, true, false, WorkbenchMeasure::Files) == "at least 8 files");
    // Still running outranks cut short, and a running part with a value shows the sum as it stands.
    TEST_CHECK(moreValue(true, false, false, true, kBytes) == "80 bytes");
    TEST_CHECK(moreValue(false, true, false, false, kBytes) == "80 bytes");
  }

  // The same folding in Nest: the "+N more" tile's size line.
  void nestMoreTileCarriesItsParts() {
    constexpr double kWidth = 600;
    constexpr double kHeight = 500;
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "r", .folder = true, .items = 300}};
    for (int i = 0; i < 300; ++i) {
      rows.push_back({.parent = 0, .name = std::format("d{:03}", i), .folder = true, .bytes = 1000.0 - i, .items = 1});
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    const auto moreSize = [&] {
      const auto scene = buildNestScene(tree, {}, kWidth, kHeight, {});
      for (const auto& piece : scene.pieces) {
        if (const auto* more = std::get_if<NestMoreLabel>(&piece.label);
            more != nullptr && piece.kind == NestPieceKind::More) {
          return more->size;
        }
      }
      return std::string("no more tile");
    };
    TEST_CHECK(moreSize().ends_with("KiB") && !moreSize().starts_with("at least"));
    tree.nodes[300].lowerBound = true;
    TEST_CHECK(moreSize().starts_with("at least "));
    tree.nodes[299].partial = true;
    TEST_CHECK(!moreSize().starts_with("at least ") && moreSize().ends_with("KiB"));
    auto label = nestMoreLabel(3, 2048, MeasureState::Done, {.x = 0, .y = 0, .w = 90, .h = 90});
    TEST_CHECK(label.size == "2.0\u00A0KiB");
    label = nestMoreLabel(3, 2048, MeasureState::LowerBound, {.x = 0, .y = 0, .w = 90, .h = 90});
    TEST_CHECK(label.size == "at least 2.0\u00A0KiB");
    label = nestMoreLabel(3, 0, MeasureState::LowerBound, {.x = 0, .y = 0, .w = 90, .h = 90});
    TEST_CHECK(label.size == kNotMeasured);
    label = nestMoreLabel(3, 0, MeasureState::Measuring, {.x = 0, .y = 0, .w = 90, .h = 90});
    TEST_CHECK(label.size == kMeasuring);
    label = nestMoreLabel(3, 2048, MeasureState::Measuring, {.x = 0, .y = 0, .w = 90, .h = 90});
    TEST_CHECK(label.size == "2.0\u00A0KiB");
  }

  // Cut-short folders are not running: the hub note, the ranking and the part note read as they do for a folder whose
  // entries were left out.
  void aCutShortFolderIsNotRunning() {
    auto tree = cutShortTree();
    tree.nodes[0].truncated = true;
    TEST_CHECK(hubNote(tree, 0, 0, {}) == "Not expanded in this snapshot: at least 3 items");
    tree.nodes[0].partial = true;
    TEST_CHECK(hubNote(tree, 0, 0, {}) == "Loading…");

    auto ranked = cutShortTree();
    ranked.nodes[4].partial = false;
    const auto deep = luciole::workbench::jumpAhead(ranked, {}, {});
    TEST_CHECK(!deep.partial);
    ranked.nodes[4].partial = true;
    TEST_CHECK(luciole::workbench::jumpAhead(ranked, {}, {}).partial);
  }
} // namespace

int main() {
  labelsSayMeasuring();
  zeroOfAFinishedFolderStays();
  theHubSaysMeasuring();
  readoutAndTooltipSayMeasuring();
  fiveWordings();
  theShortPrefix();
  hubReadoutAndTooltipsForACutShortFolder();
  tooltipSaysTheStateOnce();
  tooltipsSayWhyAFolderIsNotMeasured();
  theHubTooltipSaysWhyTheTotalIsALowerBound();
  sumsCarryTheirParts();
  nestMoreTileCarriesItsParts();
  aCutShortFolderIsNotRunning();
  return 0;
}
