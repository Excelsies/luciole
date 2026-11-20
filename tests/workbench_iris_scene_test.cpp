#include "test_check.h"
#include "workbench/iris_hit.h"
#include "workbench/iris_scene.h"
#include "workbench_fixture.h"
#include "workbench_iris_fixture.h"

#include <algorithm>
#include <format>
#include <limits>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::workbench::iris;
  using luciole::test::referenceTree;
  using luciole::test::WorkbenchRow;
  using ring::Point;
  using namespace luciole::test::iris;

  constexpr double kExact = 1e-9;

  void testFullSceneMainRing() {
    const auto tree = folderX();
    const auto scene = buildScene(tree, fullInput()).value();
    TEST_CHECK(scene.folder == 5 && scene.entryCount == 2 && !scene.dial.compact);
    TEST_CHECK(scene.main.level == 0 && scene.main.folder == 5);
    TEST_CHECK(scene.main.band.r0 == 121 && scene.main.band.r1 == 241);
    const auto& sectors = scene.main.sectors;
    TEST_CHECK(sectors.size() == 2);
    TEST_CHECK(sectors[0].kind == SectorKind::Entry && sectors[0].node == 7);
    TEST_CHECK(sectors[1].kind == SectorKind::Entry && sectors[1].node == 8);
    TEST_NEAR(sectors[0].arc.a0, 0.0, kExact);
    TEST_NEAR(sectors[0].arc.a1, 239.70167702445636, kExact);
    TEST_NEAR(sectors[1].arc.a1, 360.0, kExact);
    TEST_CHECK(sectors[0].color == nodeColor(tree, 7, LayoutSettings{}));
    TEST_CHECK(sectors[1].color == nodeColor(tree, 8, LayoutSettings{}));
  }

  void testFullSceneContextRings() {
    const auto tree = folderX();
    const auto scene = buildScene(tree, fullInput()).value();
    TEST_CHECK(scene.rings.size() == 2);
    const auto& parent = scene.rings[0];
    TEST_CHECK(parent.level == 1 && parent.folder == 1);
    TEST_CHECK(parent.band.r0 == 244 && parent.band.r1 == 266);
    TEST_CHECK(parent.sectors.size() == 2);
    TEST_CHECK(parent.sectors[0].node == 5 && parent.sectors[0].onPath);
    TEST_CHECK(parent.sectors[1].node == 6 && !parent.sectors[1].onPath);
    TEST_NEAR(parent.sectors[0].arc.a0, -124.22400284511889, kExact);
    TEST_NEAR(parent.sectors[1].arc.a1, 235.7759971548811, kExact);
    TEST_CHECK(parent.sectors[1].color == nodeColor(tree, 6, LayoutSettings{}));
    const auto& top = scene.rings[1];
    TEST_CHECK(top.level == 2 && top.folder == 0);
    TEST_CHECK(top.band.r0 == 269 && top.band.r1 == 281);
    TEST_CHECK(top.sectors.size() == 3);
    TEST_CHECK(top.sectors[0].node == 1 && top.sectors[0].onPath);
    TEST_CHECK(top.sectors[1].node == 2 && top.sectors[2].node == 4);
    TEST_NEAR(top.sectors[1].arc.a1, 248.65524582244825, kExact);
    TEST_NEAR(top.sectors[2].arc.a1, 257.0537741085803, kExact);
  }

  void testFullSceneLabelRows() {
    const auto tree = folderX();
    const auto scene = buildScene(tree, fullInput()).value();
    TEST_CHECK(scene.rows.mode == RowMode::Straight);
    TEST_CHECK(scene.rows.right.size() == 1 && scene.rows.left.size() == 1);
    const auto& right = scene.rows.right[0];
    TEST_CHECK(right.sector == 0 && right.side == ring::Side::Right && right.shown);
    TEST_NEAR(right.degrees, 119.85083851222818, kExact);
    TEST_NEAR(right.y, 560.3140948565173, 1e-6);
    TEST_NEAR(right.box.x, 1036.0, kExact);
    TEST_NEAR(right.box.y, 548.3140948565173, 1e-6);
    TEST_NEAR(right.box.width, 352.0, kExact);
    TEST_NEAR(right.box.height, 24.0, kExact);
    TEST_NEAR(right.nameWidth, 208.0, kExact);
    TEST_CHECK(right.name == "deep" && right.metric == "400 bytes");
    TEST_CHECK(right.leader.size() == 3);
    TEST_NEAR(right.leader[0].x, 909.025119456129, 1e-6);
    TEST_NEAR(right.leader[1].y, 560.3140948565173, 1e-6);
    TEST_NEAR(right.leader[2].x, 1030.0, kExact);
    const auto& left = scene.rows.left[0];
    TEST_CHECK(left.sector == 1 && left.side == ring::Side::Left && left.name == "f.log");
    TEST_NEAR(left.y, 257.6859051434829, 1e-6);
    TEST_NEAR(left.box.x, 12.0, kExact);
    TEST_NEAR(left.leader[2].x, 370.0, kExact);
    TEST_CHECK(scene.arcLabels.empty());
  }

  void testFullSceneColumnsReadTopToBottom() {
    const auto folder = fileFolder("file-");
    const auto scene = buildScene(folder.tree, fullInput()).value();
    const auto& right = scene.rows.right;
    const auto& left = scene.rows.left;
    TEST_CHECK(right.size() + left.size() == scene.main.sectors.size());
    TEST_CHECK(right.size() >= 5 && left.size() >= 5);
    TEST_CHECK(scene.main.sectors.back().kind == SectorKind::More);
    TEST_CHECK(right.front().sector == 0 && left.front().sector == scene.main.sectors.size() - 1);
    for (const auto* column : {&right, &left}) {
      for (std::size_t i = 1; i < column->size(); ++i) {
        TEST_CHECK((*column)[i].y - (*column)[i - 1].y >= ring::kRowHeight - 1e-9);
      }
    }
    for (std::size_t i = 1; i < left.size(); ++i) {
      TEST_CHECK(left[i].degrees < left[i - 1].degrees && left[i].sector + 1 == left[i - 1].sector);
    }
    TEST_CHECK(left.front().name == "+11 more" && right.size() == 25 && left.size() == 25);
    TEST_CHECK(std::ranges::all_of(right, [](const LabelRow& row) { return row.leader.size() >= 3; }));
  }

  void testFullSceneHubAndStack() {
    const auto tree = folderX();
    const auto scene = buildScene(tree, fullInput()).value();
    const auto& hub = scene.hub;
    TEST_CHECK((hub.center == Point{700, 409}));
    TEST_CHECK(hub.radius == 113 && hub.fontSize == 13 && hub.nameWidth == 169);
    TEST_CHECK(hub.chipWidth == 143 && hub.chipFontSize == 11);
    TEST_CHECK(!hub.tiny && hub.showItems && hub.showTrail && hub.canGoUp);
    TEST_CHECK(hub.name == "x" && hub.total == "500 bytes" && hub.items == "2 items" && hub.note.empty());
    const auto& stack = scene.stack;
    TEST_CHECK(stack.box.x == 550 && stack.box.y == 10 && stack.box.width == 300 && stack.box.height == 75);
    TEST_CHECK(stack.fontSize == 12 && stack.textWidth == 280);
    TEST_CHECK(stack.rows.size() == 2);
    TEST_CHECK(stack.rows[0].node == 0 && stack.rows[0].levels == 2 && stack.rows[0].text == "r");
    TEST_NEAR(stack.rows[0].opacity, 0.88, kExact);
    TEST_CHECK(stack.rows[0].box.y == 55 && stack.rows[0].box.height == 15);
    TEST_CHECK(stack.rows[1].node == 1 && stack.rows[1].levels == 1 && stack.rows[1].text == "a");
    TEST_CHECK(stack.rows[1].opacity == 1 && stack.rows[1].box.y == 70);
  }

  void testCompactSceneArcsAndHiddenRows() {
    const auto folder = fileFolder("file-");
    const auto scene = buildScene(folder.tree, hostInput(340, 380, true)).value();
    TEST_CHECK(scene.dial.compact && !scene.dial.legend && scene.rings.empty());
    TEST_CHECK(compactArcCount(scene.dial) == 7);
    const auto& sectors = scene.main.sectors;
    TEST_CHECK(sectors.size() == 8 && scene.entryCount == 60);
    TEST_NEAR(sectors[0].arc.a1, 48.37497902299476, kExact);
    TEST_CHECK(sectors[7].kind == SectorKind::More && sectors[7].merged.size() == 53);
    TEST_CHECK(sectors[7].merged.front() == 8 && sectors[7].merged.back() == 60);
    TEST_NEAR(sectors[7].arc.a1 - sectors[7].arc.a0, 30.0, kExact);
    TEST_CHECK(scene.rows.mode == RowMode::Legend && scene.rows.right.size() == 8 && scene.rows.left.empty());
    for (const auto& row : scene.rows.right) {
      TEST_CHECK(!row.shown && row.leader.empty());
    }
    TEST_CHECK(scene.rows.right[7].name == "+53 more" && scene.rows.right[7].metric.empty());
    const auto& hub = scene.hub;
    TEST_CHECK(hub.radius == 64 && hub.fontSize == 12 && hub.nameWidth == 108 && hub.chipWidth == 82);
    TEST_CHECK(!hub.tiny && hub.showItems && hub.showTrail && !hub.canGoUp);
    TEST_CHECK(hub.total == "1.7\u00A0MiB" && hub.items == "60 items");
    TEST_CHECK(scene.stack.rows.empty() && scene.stack.box.x == 20 && scene.stack.box.y == 7);
    TEST_CHECK(scene.stack.box.height == 42 && scene.stack.fontSize == 11);
  }

  void testCompactSceneNamesInsideArcs() {
    const auto folder = fileFolder("file-");
    const auto scene = buildScene(folder.tree, hostInput(340, 380, true)).value();
    const std::vector<Point> centers{
        {215.06962496955734, 102.6569439118807}, {274.8277639829772, 169.66623486119175},
        {265.1415154018143, 258.20952858924204}, {193.84187913860893, 310.38512373294526},
        {107.81478873327572, 293.7358776874554}, {61.36944707726549, 220.3032647700073},
        {82.21219077797494, 136.71802241938363},
    };
    TEST_CHECK(scene.arcLabels.size() == 7);
    for (std::size_t i = 0; i < scene.arcLabels.size(); ++i) {
      const auto& label = scene.arcLabels[i];
      TEST_CHECK(label.sector == i && label.text == folder.names[i] && label.width == 68 && label.height == 13);
      TEST_NEAR(label.center.x, centers[i].x, 1e-6);
      TEST_NEAR(label.center.y, centers[i].y, 1e-6);
      TEST_CHECK(label.ink == scene.main.sectors[i].color.ink);
    }
  }

  void testCompactSceneShortensNamesThatDoNotFit() {
    const auto folder = fileFolder("a-rather-long-report-name-");
    auto input = hostInput(340, 380, true);
    input.labelAdvance = 7.5;
    const auto scene = buildScene(folder.tree, input).value();
    TEST_CHECK(scene.arcLabels.size() == 7);
    const std::vector<std::string> texts{
        "a-rat…0.txt", "a-rat….txt", "a-rat….txt", "a-rath…3.txt", "a-rat….txt", "a-rat….txt", "a-rat…6.txt",
    };
    const std::vector<double> widths{85, 77, 77, 92, 77, 77, 85};
    for (std::size_t i = 0; i < texts.size(); ++i) {
      TEST_CHECK(scene.arcLabels[i].text == texts[i]);
      TEST_CHECK(scene.arcLabels[i].width == widths[i]);
    }
    TEST_NEAR(scene.arcLabels[3].center.x, 197.04102583029865, 1e-6);
    TEST_NEAR(scene.arcLabels[3].center.y, 324.7942548811114, 1e-6);
    input.labelAdvance = 100;
    TEST_CHECK(buildScene(folder.tree, input).value().arcLabels.empty());
    for (const double advance : {kNan, 0.0, -6.0}) {
      input.labelAdvance = advance;
      TEST_CHECK(buildScene(folder.tree, input).value().arcLabels.empty());
    }
  }

  void testCompactSceneLegend() {
    const auto folder = fileFolder("file-");
    const auto scene = buildScene(folder.tree, hostInput(600, 240, true)).value();
    TEST_CHECK(scene.dial.legend && compactArcCount(scene.dial) == 7);
    const auto& rows = scene.rows.right;
    TEST_CHECK(scene.rows.mode == RowMode::Legend && rows.size() == 8 && scene.rows.left.empty());
    for (std::size_t j = 0; j < rows.size(); ++j) {
      TEST_CHECK(rows[j].shown && rows[j].sector == j && rows[j].leader.empty());
      TEST_NEAR(rows[j].y, 110.0 + (static_cast<double>(j) - 3.5) * 24.0, kExact);
      TEST_CHECK(rows[j].box.x == 228 && rows[j].box.width == 230 && rows[j].nameWidth == 136);
    }
    TEST_NEAR(rows[0].box.y, 14.0, kExact);
    TEST_CHECK(rows[0].name == "file-00.txt" && rows[0].metric == "58.6\u00A0KiB");
    TEST_CHECK(rows[7].name == "+53 more");
    TEST_CHECK(scene.stack.rows.empty() && scene.stack.box.height == 0);
  }

  void testCompactContextSegmentsAreAtLeast12Pixels() {
    auto tree = folderX();
    auto settings = LayoutSettings{};
    settings.emphasis = 0.7;
    auto full = fullInput();
    full.settings = settings;
    const auto wide = buildScene(tree, full).value();
    TEST_CHECK(wide.rings[1].sectors.size() == 3 && wide.rings[1].sectors[2].node == 4);
    auto input = hostInput(340, 380, true);
    input.settings = settings;
    const auto scene = buildScene(tree, input).value();
    TEST_CHECK(scene.dial.thickness.size() == 2 && scene.rings.size() == 2);
    TEST_CHECK(scene.rings[0].band.r0 == 125 && scene.rings[1].band.r1 == 151);
    const auto& top = scene.rings[1];
    TEST_CHECK(top.sectors.size() == 3);
    TEST_CHECK(top.sectors[2].kind == SectorKind::Rest && top.sectors[2].node == kNoNode);
    TEST_CHECK((top.sectors[2].merged == std::vector<NodeId>{4}));
    for (const auto& ringItem : scene.rings) {
      const double middle = (ringItem.band.r0 + ringItem.band.r1) / 2;
      for (const auto& sector : ringItem.sectors) {
        TEST_CHECK((sector.arc.a1 - sector.arc.a0) * ring::kRadian * middle >= 12 - 1e-9);
      }
    }
  }

  // A chain of folders "", l1 ... l6, each holding the next and one file; opened at l6 below six ancestors.
  Tree deepChain() {
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "", .folder = true, .bytes = 700, .items = 2}};
    const std::vector<std::string_view> names{"l1", "l2", "l3", "l4", "l5", "l6"};
    for (int level = 1; level <= 6; ++level) {
      const int folder = static_cast<int>(rows.size());
      rows.push_back(
          {.parent = level == 1 ? 0 : folder - 2,
           .name = names[static_cast<std::size_t>(level - 1)],
           .folder = true,
           .bytes = 600.0 - level * 90,
           .items = 2}
      );
      rows.push_back({.parent = level == 1 ? 0 : folder - 2, .name = "note.txt", .bytes = 10});
    }
    rows.push_back({.parent = 11, .name = "leaf.txt", .bytes = 50});
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 11;
    tree.ancestors = {9, 7, 5, 3, 1, 0};
    return tree;
  }

  void testAncestorStackCollapsesFarLevels() {
    const auto tree = deepChain();
    const auto scene = buildScene(tree, fullInput()).value();
    TEST_CHECK(scene.rings.size() == 4);
    TEST_CHECK(scene.rings[3].level == 4 && scene.rings[3].folder == 3);
    const auto& rows = scene.stack.rows;
    TEST_CHECK(rows.size() == 5);
    TEST_CHECK(rows[0].node == 0 && rows[0].levels == 6 && rows[0].opacity == 0.5);
    TEST_CHECK(rows[0].text == "⋯ 2 more levels · /");
    TEST_CHECK(rows[1].node == 3 && rows[1].levels == 4 && rows[1].text == "l2");
    TEST_NEAR(rows[1].opacity, 0.64, kExact);
    TEST_CHECK(rows[4].node == 9 && rows[4].levels == 1 && rows[4].text == "l5" && rows[4].opacity == 1);
    TEST_CHECK(rows[0].box.y == 10 && rows[4].box.y == 70);
    const auto compact = buildScene(tree, hostInput(340, 380, true)).value();
    TEST_CHECK(compact.rings.size() == 4 && compact.dial.thickness.size() == 4);
    TEST_CHECK(compact.stack.rows.size() == 3);
    TEST_CHECK(compact.stack.rows[0].text == "⋯ 4 more levels · /");
    TEST_CHECK(compact.stack.rows[2].text == "l5" && compact.stack.rows[2].box.height == 14);
    TEST_CHECK(compact.hub.radius == 42 && compact.hub.showItems && !compact.hub.showTrail);
  }

  void testTinyHubLeavesTextToTheReadout() {
    const auto tree = folderX();
    const auto scene = buildScene(tree, hostInput(240, 220, true)).value();
    TEST_CHECK(scene.hub.radius == 25 && scene.hub.tiny && !scene.hub.showItems && !scene.hub.showTrail);
    TEST_CHECK(scene.hub.fontSize == 12 && scene.hub.nameWidth == 42 && scene.hub.chipFontSize == 11);
    TEST_CHECK(compactArcCount(scene.dial) == 5);
    TEST_CHECK(scene.stack.rows.empty());
  }

  void testHiddenFolderStaysOnItsParentsRing() {
    auto tree = referenceTree();
    tree.root = 3;
    tree.ancestors = {0};
    const auto scene = buildScene(tree, fullInput()).value();
    TEST_CHECK(scene.entryCount == 0 && scene.main.sectors.empty());
    TEST_CHECK(scene.rows.right.empty() && scene.rows.left.empty());
    const auto& parent = scene.rings[0];
    TEST_CHECK(parent.sectors.size() == 4);
    TEST_CHECK(parent.sectors[3].node == 3 && parent.sectors[3].onPath);
    TEST_NEAR((parent.sectors[3].arc.a0 + parent.sectors[3].arc.a1) / 2, 0.0, kExact);
    TEST_CHECK(scene.hub.name == ".hidden" && scene.hub.note == "This folder is empty");
    TEST_CHECK(!hitTest(scene, ring::polar(scene.dial.center, 180, 45)).has_value());
  }

  void testArcLabelsSkipEmptyNames() {
    const auto tree = luciole::test::workbenchTree(
        {{.parent = -1, .name = "r", .folder = true, .bytes = 20, .items = 2},
         {.parent = 0, .name = "", .bytes = 10},
         {.parent = 0, .name = "x", .bytes = 10}},
        0
    );
    const auto scene = buildScene(tree, hostInput(340, 380, true)).value();
    TEST_CHECK(scene.main.sectors.size() == 2);
    TEST_CHECK(scene.arcLabels.size() == 1 && scene.arcLabels[0].text == "x");
    TEST_CHECK(scene.arcLabels[0].sector == 1);
    const auto stray = luciole::test::workbenchTree(
        {{.parent = -1, .name = "r", .folder = true, .bytes = 10, .items = 1},
         {.parent = 0, .name = "\x80", .bytes = 10}},
        0
    );
    const auto strayScene = buildScene(stray, hostInput(340, 380, true)).value();
    TEST_CHECK(strayScene.arcLabels.size() == 1 && strayScene.arcLabels[0].text == "\x80");
  }

  void testHubNoteSaysWhyAFolderShowsNoArcs() {
    const auto tree = referenceTree();
    TEST_CHECK(hubNote(tree, 0, 3, "").empty());
    TEST_CHECK(hubNote(tree, 4, 0, "") == "This folder is empty");
    TEST_CHECK(hubNote(tree, 4, 0, "zz") == "Nothing matches “zz”");
    TEST_CHECK(hubNote(tree, kNoNode, 0, "").empty());
    const auto unlisted = luciole::test::workbenchTree({{.parent = -1, .name = "u", .folder = true, .items = 12}}, 0);
    TEST_CHECK(hubNote(unlisted, 0, 0, "zz") == "Not listed yet: 12 items");
    auto flagged = unlisted;
    flagged.nodes[0].truncated = true;
    TEST_CHECK(hubNote(flagged, 0, 0, "zz") == "Not expanded in this snapshot: 12 items");
    // A walk cut short counted the entries it read: the count is a lower bound.
    flagged.nodes[0].lowerBound = true;
    TEST_CHECK(hubNote(flagged, 0, 0, "zz") == "Not expanded in this snapshot: at least 12 items");
    auto unlistedBound = unlisted;
    unlistedBound.nodes[0].lowerBound = true;
    TEST_CHECK(hubNote(unlistedBound, 0, 0, "zz") == "Not listed yet: at least 12 items");
    flagged.nodes[0].lowerBound = false;
    flagged.nodes[0].otherFilesystem = true;
    TEST_CHECK(hubNote(flagged, 0, 0, "zz") == "On another filesystem, not scanned");
    flagged.nodes[0].unreadable = true;
    TEST_CHECK(hubNote(flagged, 0, 0, "zz") == "This folder could not be read");
    auto locked = tree;
    locked.nodes[4].unreadable = true;
    TEST_CHECK(hubNote(locked, 4, 0, "") == "This folder could not be read");
    TEST_CHECK(hubNote(locked, 4, 2, "").empty());
    const auto oneHidden = luciole::test::workbenchTree(
        {{.parent = -1, .name = "h", .folder = true, .items = 1}, {.parent = 0, .name = ".a", .bytes = 5}}, 0
    );
    TEST_CHECK(hubNote(oneHidden, 0, 0, "") == "Only 1 hidden entry (Ctrl+H shows them)");
    const auto twoHidden = luciole::test::workbenchTree(
        {{.parent = -1, .name = "h", .folder = true, .items = 2},
         {.parent = 0, .name = ".a", .bytes = 5},
         {.parent = 0, .name = ".b", .bytes = 5}},
        0
    );
    TEST_CHECK(hubNote(twoHidden, 0, 0, "") == "Only 2 hidden entries (Ctrl+H shows them)");
    TEST_CHECK(buildScene(twoHidden, fullInput()).value().hub.note == "Only 2 hidden entries (Ctrl+H shows them)");
    const std::string longFilter(5000, 'q');
    const auto note = hubNote(tree, 4, 0, longFilter);
    TEST_CHECK(note.starts_with("Nothing matches “qqq") && note.size() < 260);
  }

  void testTrailChips() {
    TEST_CHECK(trailChips(0).empty());
    const auto short3 = trailChips(3);
    TEST_CHECK(short3.size() == 3 && short3[2].index == 2 && !short3[2].gap);
    const auto long5 = trailChips(5);
    TEST_CHECK(long5.size() == 3);
    TEST_CHECK(long5[0].index == 0 && !long5[0].gap && long5[1].index == 1 && !long5[1].gap);
    TEST_CHECK(long5[2].index == 4 && long5[2].gap);
  }

  void testInnerBandShowsAFolderArcsChildren() {
    const auto tree = folderX();
    const auto scene = buildScene(tree, fullInput()).value();
    const auto band = innerBand(tree, scene, 7, LayoutSettings{});
    TEST_CHECK(band.has_value());
    TEST_NEAR(band->arc.a1, 239.70167702445636, kExact);
    TEST_CHECK(band->backing.r0 == 121 && band->backing.r1 == 138);
    TEST_CHECK(band->band.r0 == 125 && band->band.r1 == 135);
    TEST_CHECK(band->sectors.size() == 1 && band->sectors[0].node == 9);
    TEST_NEAR(band->sectors[0].arc.a0, 1.5, kExact);
    TEST_NEAR(band->sectors[0].arc.a1, 238.20167702445636, kExact);
    TEST_CHECK(band->sectors[0].color == nodeColor(tree, 9, LayoutSettings{}));
    TEST_CHECK(!innerBand(tree, scene, 8, LayoutSettings{}).has_value());
    TEST_CHECK(!innerBand(tree, scene, 1, LayoutSettings{}).has_value());
    TEST_CHECK(!innerBand(tree, scene, kNoNode, LayoutSettings{}).has_value());
    const auto narrow = luciole::test::workbenchTree(
        {{.parent = -1, .name = "n", .folder = true, .bytes = 10020, .items = 2},
         {.parent = 0, .name = "big.bin", .bytes = 10000},
         {.parent = 0, .name = "small", .folder = true, .bytes = 10, .items = 1},
         {.parent = 2, .name = "s.txt", .bytes = 10}},
        0
    );
    const auto narrowScene = buildScene(narrow, fullInput()).value();
    TEST_CHECK(narrowScene.main.sectors[1].node == 2);
    TEST_CHECK(narrowScene.main.sectors[1].arc.a1 - narrowScene.main.sectors[1].arc.a0 < kInnerBandMinDegrees);
    TEST_CHECK(!innerBand(narrow, narrowScene, 2, LayoutSettings{}).has_value());
    const auto hiddenOnly = luciole::test::workbenchTree(
        {{.parent = -1, .name = "n", .folder = true, .bytes = 20, .items = 1},
         {.parent = 0, .name = "h", .folder = true, .bytes = 10, .items = 1},
         {.parent = 1, .name = ".x", .bytes = 10}},
        0
    );
    const auto hiddenScene = buildScene(hiddenOnly, fullInput()).value();
    TEST_CHECK(!innerBand(hiddenOnly, hiddenScene, 1, LayoutSettings{}).has_value());
    const auto tail = luciole::test::workbenchTree(
        {{.parent = -1, .name = "t", .folder = true, .bytes = 100000, .items = 2},
         {.parent = 0, .name = "big", .folder = true, .bytes = 100000, .items = 6},
         {.parent = 0, .name = "tiny.txt"},
         {.parent = 1, .name = "huge.bin", .bytes = 100000},
         {.parent = 1, .name = "e1"},
         {.parent = 1, .name = "e2"},
         {.parent = 1, .name = "e3"},
         {.parent = 1, .name = "e4"},
         {.parent = 1, .name = "e5"}},
        0
    );
    const auto tailScene = buildScene(tail, fullInput()).value();
    const auto tailBand = innerBand(tail, tailScene, 1, LayoutSettings{});
    TEST_CHECK(tailBand.has_value() && tailBand->sectors.size() == 2);
    TEST_CHECK(tailBand->sectors[0].node == 3);
    TEST_CHECK(tailBand->sectors[1].kind == SectorKind::Rest && tailBand->sectors[1].node == kNoNode);
    TEST_NEAR(tailBand->sectors[1].arc.a1, tailBand->arc.a1 - 1.5, kExact);
  }

  void testScenesRefuseWhatCannotBeLaidOut() {
    auto tree = folderX();
    TEST_CHECK(!buildScene(tree, hostInput(kNan, 600, false)).has_value());
    TEST_CHECK(!buildScene(tree, hostInput(600, 0, true)).has_value());
    TEST_CHECK(!buildScene(tree, hostInput(-5, 600, true)).has_value());
    auto unknown = tree;
    unknown.root = kNoNode;
    TEST_CHECK(!buildScene(unknown, fullInput()).has_value());
    TEST_CHECK(!buildScene(Tree{}, fullInput()).has_value());
    tree.ancestors = {1, kNoNode, 0};
    const auto broken = buildScene(tree, fullInput()).value();
    TEST_CHECK(broken.rings.size() == 1);
  }
} // namespace

int main() {
  testFullSceneMainRing();
  testFullSceneContextRings();
  testFullSceneLabelRows();
  testFullSceneColumnsReadTopToBottom();
  testFullSceneHubAndStack();
  testCompactSceneArcsAndHiddenRows();
  testCompactSceneNamesInsideArcs();
  testCompactSceneShortensNamesThatDoNotFit();
  testCompactSceneLegend();
  testCompactContextSegmentsAreAtLeast12Pixels();
  testAncestorStackCollapsesFarLevels();
  testTinyHubLeavesTextToTheReadout();
  testHiddenFolderStaysOnItsParentsRing();
  testArcLabelsSkipEmptyNames();
  testHubNoteSaysWhyAFolderShowsNoArcs();
  testTrailChips();
  testInnerBandShowsAFolderArcsChildren();
  testScenesRefuseWhatCannotBeLaidOut();
  std::println("workbench iris scene checks passed");
  return 0;
}
