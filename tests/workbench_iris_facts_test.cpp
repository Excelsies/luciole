#include "test_check.h"
#include "workbench/format.h"
#include "workbench/iris_facts.h"
#include "workbench/iris_hit.h"
#include "workbench_fixture.h"
#include "workbench_iris_fixture.h"

#include <algorithm>
#include <format>
#include <limits>
#include <string>
#include <vector>

namespace {
  using namespace luciole::test::iris;
  using luciole::test::WorkbenchRow;

  ShapeId idOf(NodeId node, SceneRole role) { return {.node = node, .role = shapeRole(role)}; }

  ShapeMarks hovering(NodeId node, SceneRole role) {
    ShapeMarks marks;
    marks.hovered = idOf(node, role);
    return marks;
  }

  Scene compactScene(const Tree& tree) { return buildScene(tree, hostInput(340, 534, true)).value(); }

  // Ids of folderX: 1 a, 5 x (500 bytes, 2 items), 7 deep (400 bytes, a folder of 1 item), 8 f.log (100 bytes).
  void readoutNamesTheFirstThingThatApplies() {
    const auto tree = folderX();
    const auto scene = compactScene(tree);
    const auto folder = std::format("x  ·  {}  ·  2 items", sizeText(500));
    const auto deep = std::format("deep  ·  {}  ·  1 item", sizeText(400));
    const auto log = std::format("f.log  ·  {}", sizeText(100));
    TEST_CHECK(readoutText(tree, scene, {}, CursorStop::Marks) == folder);
    TEST_CHECK(readoutText(tree, scene, hovering(7, SceneRole::Piece), CursorStop::Marks) == deep);
    TEST_CHECK(readoutText(tree, scene, hovering(8, SceneRole::Row), CursorStop::Marks) == log);
    TEST_CHECK(readoutText(tree, scene, {.hovered = {}, .linked = 7, .cursor = 8}, CursorStop::Marks) == deep);
    TEST_CHECK(readoutText(tree, scene, {.hovered = {}, .linked = kNoNode, .cursor = 8}, CursorStop::Marks) == log);
    // While the cursor rests on "+N more" it names no entry.
    TEST_CHECK(readoutText(tree, scene, {.hovered = {}, .linked = kNoNode, .cursor = 8}, CursorStop::More) == folder);
    // Unknown ids are passed over, and the hub, stack rows and chips name nothing themselves.
    TEST_CHECK(readoutText(tree, scene, {.hovered = {}, .linked = 4000, .cursor = 8}, CursorStop::Marks) == log);
    TEST_CHECK(readoutText(tree, scene, hovering(5, SceneRole::Hub), CursorStop::Marks) == folder);
    TEST_CHECK(readoutText(tree, scene, hovering(1, SceneRole::Stack), CursorStop::Marks) == folder);
    // A context piece is named like an entry.
    TEST_CHECK(readoutText(tree, scene, hovering(1, SceneRole::Piece), CursorStop::Marks).starts_with("a  ·  "));
  }

  // The readout is the name, then the facts apart from it so that they can be shown whole.
  void readoutLineSplitsTheNameFromItsFacts() {
    const auto tree = folderX();
    const auto scene = compactScene(tree);
    const auto folder = readoutLine(tree, scene, {}, CursorStop::Marks);
    TEST_CHECK(folder.text == "x");
    TEST_CHECK(folder.facts == std::format("{}  ·  2 items", sizeText(500)));
    const auto file = readoutLine(tree, scene, hovering(8, SceneRole::Row), CursorStop::Marks);
    TEST_CHECK(file.text == "f.log" && file.facts == sizeText(100));
    // Under Size by Items the count is not told twice.
    auto input = hostInput(340, 534, true);
    input.settings.sizeBy = luciole::WorkbenchMeasure::Items;
    const auto items = buildScene(tree, input).value();
    TEST_CHECK(readoutLine(tree, items, {}, CursorStop::Marks).facts == "2 items");
    TEST_CHECK(readoutText(tree, scene, {}, CursorStop::Marks) == folder.text + "  ·  " + folder.facts);
  }

  // A folder "sub" beside 60 small files of r, opened at sub: r's ring merges the files into one piece.
  Tree subBesideFiles() {
    std::vector<std::string> names;
    names.reserve(60);
    for (int i = 0; i < 60; ++i) {
      names.push_back(std::format("f{:02}.txt", i));
    }
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "r", .folder = true, .bytes = 1e6, .items = 61},
        {.parent = 0, .name = "sub", .folder = true, .bytes = 9e5, .items = 1},
        {.parent = 1, .name = "in.txt", .bytes = 9e5},
    };
    for (const auto& name : names) {
      rows.push_back({.parent = 0, .name = name, .bytes = 100});
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = {0};
    return tree;
  }

  void mergedPiecesCountTheirEntries() {
    const auto files = fileFolder("f");
    const auto scene = compactScene(files.tree);
    const auto more =
        std::ranges::find_if(scene.main.sectors, [](const Sector& s) { return s.kind == SectorKind::More; });
    TEST_CHECK(more != scene.main.sectors.end());
    const auto expected = std::format("{} more entries", more->merged.size());
    TEST_CHECK(readoutText(files.tree, scene, hovering(0, SceneRole::More), CursorStop::Marks) == expected);
    // The "+N more" row carries the folder's node.
    TEST_CHECK(readoutText(files.tree, scene, hovering(0, SceneRole::Row), CursorStop::Marks) == expected);
    const auto tree = subBesideFiles();
    const auto ringScene = compactScene(tree);
    const auto& ring = ringScene.rings.at(0);
    const auto rest = std::ranges::find_if(ring.sectors, [](const Sector& s) { return s.kind == SectorKind::Rest; });
    TEST_CHECK(rest != ring.sectors.end() && rest->merged.size() > 1);
    TEST_CHECK(
        readoutText(tree, ringScene, hovering(0, SceneRole::More), CursorStop::Marks)
        == std::format("{} more in r", rest->merged.size())
    );
  }

  void sharesAreWrittenAsPercentages() {
    TEST_CHECK(shareText(0.48) == "48%");
    TEST_CHECK(shareText(1) == "100%");
    TEST_CHECK(shareText(0.0123) == "1.2%");
    TEST_CHECK(shareText(0.0994) == "9.9%");
    TEST_CHECK(shareText(0.0995) == "10%");
    TEST_CHECK(shareText(1e-7) == "<0.1%");
    TEST_CHECK(shareText(0) == "0%");
    TEST_CHECK(shareText(-0.5).empty());
    TEST_CHECK(shareText(std::numeric_limits<double>::quiet_NaN()).empty());
    TEST_CHECK(shareText(std::numeric_limits<double>::infinity()).empty());
  }

  void pathsJoinNamesFromTheTop() {
    const auto tree = folderX();
    TEST_CHECK(nodePath(tree, 7) == "r/a/x/deep");
    TEST_CHECK(nodePath(tree, 0) == "r");
    TEST_CHECK(nodePath(tree, kNoNode).empty());
    auto rooted = folderX();
    rooted.nodes[0].name = "/";
    TEST_CHECK(nodePath(rooted, 7) == "/a/x/deep");
    TEST_CHECK(nodePath(rooted, 0) == "/");
  }

  void tipsFollowTheShape() {
    const auto tree = folderX();
    const auto scene = compactScene(tree);
    const auto entry = shapeTip(tree, scene, idOf(7, SceneRole::Piece));
    TEST_CHECK(entry.text.empty() && entry.rows.size() == 3);
    TEST_CHECK(entry.rows[0].key == "Name" && entry.rows[0].value == "deep");
    TEST_CHECK(entry.rows[1].key == "Size" && entry.rows[1].value == sizeText(400));
    TEST_CHECK(entry.rows[2].key == "Share" && entry.rows[2].value == shareText(share(tree, 7, scene.settings)));
    // Size means apparent bytes whatever the scene measures.
    auto files = folderX();
    files.nodes[7].files = 9;
    auto settings = LayoutSettings{};
    settings.sizeBy = luciole::WorkbenchMeasure::Files;
    auto input = hostInput(340, 534, true);
    input.settings = settings;
    const auto fileScene = buildScene(files, input).value();
    TEST_CHECK(shapeTip(files, fileScene, idOf(7, SceneRole::Row)).rows.at(1).value == sizeText(400));
    TEST_CHECK(shapeTip(tree, scene, idOf(5, SceneRole::Hub)).text == "Up to a");
    TEST_CHECK(shapeTip(tree, scene, idOf(1, SceneRole::Stack)).text == "r/a");
    // A chip names its folder by its place among the hub's chips.
    auto trailed = hostInput(340, 534, true);
    trailed.trail = {7};
    const auto chipScene = buildScene(tree, trailed).value();
    TEST_CHECK(shapeTip(tree, chipScene, chipId(0)).text == "r/a/x/deep");
    // The "+N more" row, merged pieces, details, unknown ids and chips the hub does not hold have none.
    for (const auto& id :
         {idOf(5, SceneRole::Row), idOf(5, SceneRole::More), idOf(7, SceneRole::Detail), idOf(4000, SceneRole::Piece),
          idOf(7, SceneRole::Chip), chipId(1), idOf(7, SceneRole::Stack)}) {
      const auto tip = shapeTip(tree, chipScene, id);
      TEST_CHECK(tip.rows.empty() && tip.text.empty());
    }
    // The top of the snapshot has nowhere to go up to, and says so.
    auto top = luciole::test::referenceTree();
    const auto topScene = compactScene(top);
    TEST_CHECK(shapeTip(top, topScene, idOf(0, SceneRole::Hub)).text == "Top of this view");
    TEST_CHECK(shapeTip(top, topScene, idOf(4000, SceneRole::Hub)).text.empty());
  }
} // namespace

int main() {
  readoutNamesTheFirstThingThatApplies();
  readoutLineSplitsTheNameFromItsFacts();
  mergedPiecesCountTheirEntries();
  sharesAreWrittenAsPercentages();
  pathsJoinNamesFromTheTop();
  tipsFollowTheShape();
  return 0;
}
