#include "test_check.h"
#include "workbench/nest_rings.h"
#include "workbench/nest_scene.h"
#include "workbench_fixture.h"
#include "workbench_nest_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::nest;
  using luciole::test::WorkbenchRow;

  void compactLineStandsForFarAncestors() {
    auto fx = fixture();
    const auto folder = fx.at(8);
    const auto scene = buildNestScene(fx.tree, kSettings, 340, 380);
    const auto line = std::ranges::find_if(scene.rings, &NestRing::line);
    TEST_CHECK(line != scene.rings.end());
    TEST_CHECK(line->pieceCount == parentChain(fx.tree, folder).size() - 3);
    for (std::size_t i = line->firstPiece; i < line->firstPiece + line->pieceCount; ++i) {
      TEST_CHECK(scene.pieces[i].kind == NestPieceKind::LineSegment && scene.pieces[i].onPath);
    }
  }

  // A single chain `depth` folders deep under "r"; the deepest folder holds two files and is the current one.
  Tree chainTree(std::size_t depth) {
    std::vector<std::string> names;
    names.reserve(depth + 3);
    names.emplace_back("r");
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "", .folder = true, .bytes = 300, .items = 1}};
    for (std::size_t i = 1; i <= depth; ++i) {
      names.push_back(std::format("c{}", i));
      rows.push_back(
          {.parent = static_cast<int>(i - 1), .name = "", .folder = true, .bytes = 300, .items = i == depth ? 2.0 : 1.0}
      );
    }
    for (const std::string_view file : {"a.dat", "b.dat"}) {
      names.emplace_back(file);
      rows.push_back({.parent = static_cast<int>(depth), .name = "", .folder = false, .bytes = 150, .items = 0});
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
      rows[i].name = names[i];
    }
    auto tree = luciole::test::workbenchTree(rows, 5000);
    tree.root = static_cast<NodeId>(depth);
    tree.ancestors = parentChain(tree, tree.root);
    return tree;
  }

  // Far ancestors too many for 6 px each share one line piece that still names every one of them.
  void crowdedLineKeepsItsAncestors() {
    const auto tree = chainTree(200);
    const auto scene = buildNestScene(tree, kSettings, 240, 220);
    const auto line = std::ranges::find_if(scene.rings, &NestRing::line);
    TEST_CHECK(line != scene.rings.end() && line->pieceCount == 1);
    const auto index = line->firstPiece;
    const auto& piece = scene.pieces[index];
    const std::vector<NodeId> far(tree.ancestors.begin() + 3, tree.ancestors.end());
    TEST_CHECK(far.size() == 197);
    TEST_CHECK(piece.kind == NestPieceKind::LineSegment && piece.onPath && piece.folder);
    TEST_CHECK(piece.node == far.front() && piece.nodes == far);
    TEST_CHECK(piece.color == nodeColor(tree, far.front(), kSettings));
    TEST_CHECK(nestFocus(tree, scene, far.front()) == std::optional<std::size_t>{index});
    TEST_CHECK(nestFocus(tree, scene, far[100]) == std::optional<std::size_t>{index});
    TEST_CHECK(nestFocus(tree, scene, far.back()) == std::optional<std::size_t>{index});
    TEST_CHECK(nestHit(scene, 120, 5.5) == std::optional<std::size_t>{index});
    const auto deepTree = chainTree(2000);
    const auto deep = buildNestScene(deepTree, kSettings, 1400, 571);
    const auto deepLine = std::ranges::find_if(deep.rings, &NestRing::line);
    TEST_CHECK(deepLine != deep.rings.end() && deepLine->pieceCount == 1);
    const auto& shared = deep.pieces[deepLine->firstPiece];
    TEST_CHECK(shared.nodes.size() == 1995 && shared.node == deepTree.ancestors[5]);
    TEST_CHECK(nestFocus(deepTree, deep, 0) == std::optional<std::size_t>{deepLine->firstPiece});
    // On a roomier line each far ancestor keeps a segment of its own and nodes stays empty.
    const auto roomy = buildNestScene(chainTree(8), kSettings, 1400, 900);
    const auto wide = std::ranges::find_if(roomy.rings, &NestRing::line);
    TEST_CHECK(wide != roomy.rings.end() && wide->pieceCount == 3);
    for (std::size_t i = wide->firstPiece; i < wide->firstPiece + wide->pieceCount; ++i) {
      TEST_CHECK(roomy.pieces[i].nodes.empty() && hasNode(tree, roomy.pieces[i].node));
    }
  }

  void ringsMatchKnownGeometry() {
    auto fx = fixture();
    fx.at(2);
    const auto scene = buildNestScene(fx.tree, kSettings, 340, 380);
    TEST_CHECK(scene.rings.size() == 2);
    const auto& outer = scene.rings[0];
    const auto& inner = scene.rings[1];
    TEST_CHECK(outer.level == 2 && outer.folder == 0 && inner.level == 1 && inner.folder == fx.find("l0"));
    checkRect(outer.band.rect, 4, 4, 332, 372);
    checkRect(inner.band.rect, 18, 18, 304, 344);
    TEST_NEAR(outer.band.perimeter, 1306.778745, kEps);
    TEST_NEAR(inner.band.perimeter, 1206.24778, kEps);
    TEST_CHECK(outer.extension == 2 && inner.extension == 3);
    const std::vector<std::pair<std::string_view, double>> ends{
        {"l1", 421.742993},    {"s1_5", 579.561945},  {"s1_4", 742.090017}, {"s1_3", 884.685835},
        {"s1_2", 1008.423889}, {"s1_1", 1117.325931}, {"s1_0", 1206.24778},
    };
    TEST_CHECK(inner.pieceCount == ends.size());
    for (std::size_t i = 0; i < ends.size(); ++i) {
      const auto& piece = scene.pieces[inner.firstPiece + i];
      TEST_CHECK(piece.kind == NestPieceKind::Segment && piece.node == fx.find(ends[i].first) && piece.ring == 1);
      TEST_NEAR(piece.s1, ends[i].second, kEps);
      TEST_CHECK(piece.onPath == (i == 0));
      TEST_CHECK(!piece.outline.empty() && piece.bridge.empty() != piece.onPath);
    }
    const auto& path = scene.pieces[inner.firstPiece];
    checkRect(path.rect, 249.871496, 24, 4, 4);
    TEST_CHECK(path.outline.size() == 22);
    TEST_NEAR(path.outline[0].x, 42.0, kEps);
    TEST_NEAR(path.outline[0].y, 18.0, kEps);
    checkRect(scene.pieces[inner.firstPiece + 1].rect, 306, 181.181048, 16, 157.818952);
    checkRect(scene.pieces[outer.firstPiece + 4].rect, 4, 179.518123, 12, 118.996994);
    const auto* top = std::get_if<NestRingLabel>(&path.label);
    TEST_CHECK(top != nullptr && top->name == "l1" && top->measure == "87.9\u00A0KiB" && top->turn == NestTurn::None);
    TEST_NEAR(top->maxWidth, 244.0, kEps);
    TEST_NEAR(top->center.x, 170.0, kEps);
    TEST_NEAR(top->center.y, 26.0, kEps);
    TEST_CHECK(top->fontSize == 12);
    const auto* right = std::get_if<NestRingLabel>(&scene.pieces[inner.firstPiece + 1].label);
    TEST_CHECK(right != nullptr && right->turn == NestTurn::Clockwise);
    TEST_NEAR(right->maxWidth, 143.818952, kEps);
    TEST_NEAR(right->center.x, 314.0, kEps);
    TEST_NEAR(right->center.y, 260.090524, kEps);
    const auto* left = std::get_if<NestRingLabel>(&scene.pieces[inner.firstPiece + 4].label);
    TEST_CHECK(left != nullptr && left->turn == NestTurn::CounterClockwise);
    TEST_NEAR(left->center.x, 26.0, kEps);
    TEST_NEAR(left->center.y, 277.130973, kEps);
    // Ring 2 is 12 px thick in the compact profile, too thin for 11 px text.
    for (std::size_t i = outer.firstPiece; i < outer.firstPiece + outer.pieceCount; ++i) {
      TEST_CHECK(std::holds_alternative<std::monostate>(scene.pieces[i].label));
    }
    // Hit tests: a tile in the core, the on-path segment of ring 1, the gap between rings, outside.
    const auto l2 = pieceOfNode(scene, fx.find("l2"));
    TEST_CHECK(l2.has_value());
    const auto& l2Rect = scene.pieces[*l2].rect;
    TEST_CHECK(nestHit(scene, l2Rect.x + 3, l2Rect.y + 3) == l2);
    // Tiles are inset 1 px inside the core and take hits up to 1 px outside their edge.
    TEST_CHECK(nestHit(scene, l2Rect.x - 0.5, l2Rect.y - 0.5) == l2);
    TEST_CHECK(nestHit(scene, 241, 26) == std::optional<std::size_t>{inner.firstPiece});
    TEST_CHECK(!nestHit(scene, 17, 100).has_value());
    TEST_CHECK(!nestHit(scene, 1, 1).has_value());
    TEST_CHECK(!nestHit(scene, kNan, 26).has_value());
    // The parent's ring holds the grandparent's entries; l0 is on the path there.
    const auto l0 = nestFocus(fx.tree, scene, fx.find("l0"));
    TEST_CHECK(l0.has_value() && scene.pieces[*l0].ring == 0 && scene.pieces[*l0].onPath);
  }

  void normalRingsCoverTheirBands() {
    auto fx = fixture();
    fx.at(12);
    const auto scene = buildNestScene(fx.tree, kSettings, 1400, 571);
    for (const auto& ring : scene.rings) {
      double covered = 0;
      for (std::size_t i = ring.firstPiece; i < ring.firstPiece + ring.pieceCount; ++i) {
        covered += scene.pieces[i].s1 - scene.pieces[i].s0;
      }
      TEST_NEAR(covered, ring.band.perimeter, 1e-6);
    }
    // Every segment of the parent's ring, one per entry, gets a label on its longest straight side.
    const auto& nearest = scene.rings.back();
    TEST_CHECK(nearest.level == 1 && nearest.thickness == 40);
    std::size_t labeled = 0;
    for (std::size_t i = nearest.firstPiece; i < nearest.firstPiece + nearest.pieceCount; ++i) {
      labeled += std::holds_alternative<NestRingLabel>(scene.pieces[i].label) ? 1U : 0U;
    }
    TEST_CHECK(labeled >= 1);
    // Rings 3 and beyond never carry labels.
    for (const auto& ring : scene.rings) {
      if (ring.level >= 3) {
        for (std::size_t i = ring.firstPiece; i < ring.firstPiece + ring.pieceCount; ++i) {
          TEST_CHECK(std::holds_alternative<std::monostate>(scene.pieces[i].label));
        }
      }
    }
  }

  void ringLabelsTakeTheLongestStraightSide() {
    auto fx = fixture();
    fx.at(4);
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600);
    const auto& ring = scene.rings[3];
    TEST_CHECK(ring.level == 1);
    const auto& piece = scene.pieces[ring.firstPiece + 2];
    TEST_CHECK(piece.node == fx.find("s3_4"));
    const auto* label = std::get_if<NestRingLabel>(&piece.label);
    TEST_CHECK(label != nullptr && label->turn == NestTurn::None);
    TEST_NEAR(label->maxWidth, 151.76354, kEps);
    TEST_NEAR(label->center.x, 709.11823, kEps);
    TEST_NEAR(label->center.y, 522.0, kEps);
  }

  // Ring 2 labels only segments of at least 80 px, even when a straight side would hold the text.
  void ringTwoLabelsNeedEightyPixels() {
    std::vector<std::string> names;
    names.reserve(38);
    for (int i = 0; i < 38; ++i) {
      names.push_back(std::format("x{}", i));
    }
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "g", .folder = true},
        {.parent = 0, .name = "big", .bytes = 20563},
        {.parent = 0, .name = "p", .folder = true, .bytes = 9999, .items = 1},
    };
    for (const auto& name : names) {
      rows.push_back({.parent = 0, .name = name, .bytes = 9999});
    }
    rows.push_back({.parent = 2, .name = "q", .folder = true});
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = static_cast<NodeId>(rows.size() - 1);
    tree.ancestors = parentChain(tree, tree.root);
    const auto scene = buildNestScene(tree, kSettings, 800, 600);
    TEST_CHECK(scene.rings.size() == 2 && scene.rings[0].level == 2 && scene.rings[0].thickness == 22);
    const auto& ring = scene.rings[0];
    std::size_t labeled = 0;
    std::size_t shortOnes = 0;
    for (std::size_t i = ring.firstPiece; i < ring.firstPiece + ring.pieceCount; ++i) {
      const auto& piece = scene.pieces[i];
      const auto side = bandPiece(ring.band, piece.s0);
      const double length = piece.s1 - piece.s0;
      if (side != 0 || bandPiece(ring.band, piece.s1 - 1e-6) != 0 || length < 50) {
        continue;
      }
      const auto* label = std::get_if<NestRingLabel>(&piece.label);
      const bool hasLabel = label != nullptr;
      TEST_CHECK(hasLabel == (length >= 80));
      TEST_CHECK(!hasLabel || (label->fontSize == 11 && label->measure.empty()));
      labeled += hasLabel ? 1U : 0U;
      shortOnes += hasLabel ? 0U : 1U;
    }
    TEST_CHECK(labeled == 1 && shortOnes >= 5);
  }

  void mergedSegmentsHoldTheShortEntries() {
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "top", .folder = true}};
    rows.push_back({.parent = 0, .name = "big", .folder = true, .bytes = 1e9});
    std::vector<std::string> names;
    names.reserve(60);
    for (int i = 0; i < 60; ++i) {
      names.push_back(std::format("tiny{}", i));
    }
    for (const auto& name : names) {
      rows.push_back({.parent = 0, .name = name, .bytes = 1});
    }
    rows.push_back({.parent = 1, .name = "kid.txt", .bytes = 10});
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, 1);
    const auto scene = buildNestScene(tree, kSettings, 800, 600);
    TEST_CHECK(scene.rings.size() == 1);
    const auto& ring = scene.rings[0];
    const auto& last = scene.pieces[ring.firstPiece + ring.pieceCount - 1];
    TEST_CHECK(last.kind == NestPieceKind::MergedSegment && last.node == 0 && !last.onPath);
    TEST_CHECK(last.nodes.size() == 60);
    TEST_CHECK(std::holds_alternative<std::monostate>(last.label));
    const auto hit = nestHit(scene, (last.rect.x + last.rect.w / 2), (last.rect.y + last.rect.h / 2));
    TEST_CHECK(hit.has_value() && scene.pieces[*hit].kind == NestPieceKind::MergedSegment);
    const auto focus = nestFocus(tree, scene, last.nodes.front());
    TEST_CHECK(focus.has_value() && scene.pieces[*focus].kind == NestPieceKind::MergedSegment);
    TEST_CHECK(scene.pieces[ring.firstPiece].node == 1 && scene.pieces[ring.firstPiece].onPath);
  }

  void hiddenPathEntryKeepsItsSegment() {
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "top", .folder = true},
        {.parent = 0, .name = ".secret", .folder = true, .bytes = 5},
        {.parent = 0, .name = "visible", .bytes = 500},
        {.parent = 1, .name = "inside.txt", .bytes = 5},
    };
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, 1);
    const auto scene = buildNestScene(tree, kSettings, 800, 600);
    TEST_CHECK(scene.rings.size() == 1 && scene.rings[0].pieceCount == 2);
    const auto& ring = scene.rings[0];
    const auto& pinned = scene.pieces[ring.firstPiece + 1];
    TEST_CHECK(pinned.node == 1 && pinned.onPath && pinned.s1 - pinned.s0 >= 10 - kEps);
    // From the top folder the hidden entry has no tile, so neither it nor anything inside it has a focus.
    tree.root = 0;
    tree.ancestors.clear();
    const auto top = buildNestScene(tree, kSettings, 800, 600);
    TEST_CHECK(piecesOf(top, NestPieceKind::Tile).size() == 1);
    TEST_CHECK(!nestFocus(tree, top, 1).has_value());
    TEST_CHECK(!nestFocus(tree, top, 3).has_value());
    TEST_CHECK(nestFocus(tree, top, 2) == std::optional<std::size_t>{0});
  }

  // The plan, frame and rings built step by step; a plan entry the ancestors or the frame cannot honor is left out.
  void ringsFollowTheirPlanAndFrame() {
    auto f = fixture();
    f.at(3);
    const auto profile = nestProfile(900, 700);
    const auto plan = nestRingPlan(f.tree.ancestors.size(), profile);
    TEST_CHECK(plan.size() == 3 && plan[0].level == 3 && plan[2].level == 1);
    const auto frame = nestFrame(plan, 900, 700, profile);
    TEST_CHECK(frame.rects.size() == 3 && frame.radii.size() == 3);
    const auto fresh = [&] {
      NestScene scene;
      scene.folder = f.tree.root;
      scene.profile = profile;
      return scene;
    };
    auto all = fresh();
    addNestRings(f.tree, kSettings, ColorPalette{}, f.tree.ancestors, plan, frame, all);
    TEST_CHECK(all.rings.size() == 3 && all.rings[2].folder == f.tree.ancestors[0]);
    auto few = fresh();
    addNestRings(f.tree, kSettings, ColorPalette{}, std::span(f.tree.ancestors).first(1), plan, frame, few);
    TEST_CHECK(few.rings.size() == 1 && few.rings[0].level == 1);
    auto cut = frame;
    cut.rects.resize(1);
    auto shortFrame = fresh();
    addNestRings(f.tree, kSettings, ColorPalette{}, f.tree.ancestors, plan, cut, shortFrame);
    TEST_CHECK(shortFrame.rings.size() == 1 && shortFrame.rings[0].level == 3);
    const std::vector<NestRingPlan> farLine{{.level = kNestLineLevel, .thickness = 4, .from = 9}};
    auto line = fresh();
    addNestRings(
        f.tree, kSettings, ColorPalette{}, f.tree.ancestors, farLine, nestFrame(farLine, 900, 700, profile), line
    );
    TEST_CHECK(line.rings.empty() && line.pieces.empty());
    TEST_CHECK(nestRingPlan(0, profile).empty());
  }
} // namespace

int main() {
  ringsFollowTheirPlanAndFrame();
  compactLineStandsForFarAncestors();
  crowdedLineKeepsItsAncestors();
  ringsMatchKnownGeometry();
  normalRingsCoverTheirBands();
  ringLabelsTakeTheLongestStraightSide();
  ringTwoLabelsNeedEightyPixels();
  mergedSegmentsHoldTheShortEntries();
  hiddenPathEntryKeepsItsSegment();
  std::println("workbench nest rings checks passed");
  return 0;
}
