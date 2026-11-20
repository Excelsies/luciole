#include "test_check.h"
#include "workbench/ring_geometry.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"
#include "workbench_scene_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The drawn look of Nest and Iris: fills, outlines, gaps, hatches, dashes and state marks.
namespace {
  using namespace luciole::workbench;
  using luciole::test::drawn;
  using luciole::test::findSceneShape;
  using luciole::test::firstDrawn;
  using luciole::test::hitTarget;
  using luciole::test::sameRect;
  using luciole::test::sameRgb;
  using luciole::test::SceneKey;
  using luciole::test::sceneKeyOf;
  using luciole::test::WorkbenchRow;

  const LayoutSettings kSettings{};
  const ColorPalette kPalette{};

  // a > b > c > d > e > here: five ancestors, so Nest draws its two thin rings.
  Tree deepTree() {
    std::vector<WorkbenchRow> rows;
    const std::vector<std::string_view> chain{"a", "b", "c", "d", "e", "here"};
    const std::vector<std::string_view> others{"a1", "b1", "c1", "d1", "e1", "h1"};
    for (std::size_t i = 0; i < chain.size(); ++i) {
      const int parent = i == 0 ? -1 : static_cast<int>(2 * (i - 1));
      rows.push_back({.parent = parent, .name = chain[i], .folder = true, .bytes = 1e6 / (1.0 + i), .items = 2});
      rows.push_back({.parent = parent, .name = others[i], .folder = true, .bytes = 2e5, .items = 1});
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 10;
    tree.ancestors = parentChain(tree, tree.root);
    return tree;
  }

  void testNestTilesAndCore() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    const auto out = nestShapes(fx.tree, scene, {}, kPalette);
    std::size_t tiles = 0;
    for (const auto& piece : scene.pieces) {
      if (piece.kind == NestPieceKind::Tile) {
        const auto* tile = findSceneShape(out, {piece.node, SceneRole::Piece});
        TEST_CHECK(tile != nullptr && tile->kind == ShapeKind::RoundedRect);
        TEST_NEAR(tile->rect.radius, 6.0F, 1e-6);
        // A thin outline in the tile's own accent, faint until hovered.
        TEST_CHECK(tile->strokeWidth == 1 && sameRgb(tile->stroke, piece.color.accent));
        TEST_CHECK(tile->stroke.a > 0 && tile->stroke.a < 128);
        ++tiles;
      } else if (piece.kind == NestPieceKind::More) {
        const auto* more = findSceneShape(out, {piece.node, SceneRole::More});
        TEST_CHECK(more->pattern == ShapePattern::DashedOutline && sameRgb(more->stroke, kPalette.secondary));
        TEST_CHECK(more->fill.a < 64);
        TEST_NEAR(more->rect.radius, 5.0F, 1e-6);
      }
    }
    TEST_CHECK(tiles > 4);
    // Rings around the core: the core gets a faint primary frame that takes no hits.
    const auto frame = std::ranges::find_if(out.shapes, [&](const Shape& shape) {
      return sceneKeyOf(shape) == SceneKey{scene.folder, SceneRole::Detail} && shape.kind == ShapeKind::RoundedRect;
    });
    TEST_CHECK(frame != out.shapes.end());
    TEST_CHECK(!frame->hittable && sameRgb(frame->stroke, kPalette.primary) && frame->stroke.a < 128);
    TEST_CHECK(frame->rect.x < scene.core.x && frame->rect.x + frame->rect.width > scene.core.x + scene.core.w);
    // Ring pieces come first, tiles over them.
    const auto firstTile =
        std::ranges::find_if(out.shapes, [](const Shape& s) { return s.kind == ShapeKind::RoundedRect; });
    TEST_CHECK(std::ranges::any_of(out.shapes.begin(), firstTile, [](const Shape& s) {
      return s.kind == ShapeKind::Sector;
    }));
  }

  void testNestRings() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    const auto out = nestShapes(fx.tree, scene, {}, kPalette);
    // Parts of a ring segment never stroke their own edges, so no seam shows where a segment turns a corner.
    for (const auto& shape : out.shapes) {
      if (shape.kind == ShapeKind::Sector) {
        TEST_CHECK(shape.strokeWidth == 0);
      }
    }
    // The on-path segment: a quiet fill, a primary outline along the scene's outline, and a primary bridge.
    const auto* path = findSceneShape(out, {fx.work, SceneRole::Piece});
    TEST_CHECK(sameRgb(path->fill, kPalette.surfaceContainerHighest) && path->strokeWidth == 0);
    std::size_t edges = 0;
    for (const auto& shape : out.shapes) {
      // The core's frame, a rounded rectangle, belongs to the current folder too.
      const bool edge =
          shape.kind == ShapeKind::Line || shape.kind == ShapeKind::Rect || shape.kind == ShapeKind::Sector;
      if (sceneKeyOf(shape) == SceneKey{fx.work, SceneRole::Detail} && edge) {
        TEST_CHECK(sameRgb(shape.fill, kPalette.primary) && !shape.hittable);
        ++edges;
      }
    }
    TEST_CHECK(edges >= 4);
    const auto* bridge = findSceneShape(out, {fx.work, SceneRole::Bridge});
    TEST_CHECK(bridge != nullptr && sameRgb(bridge->fill, kPalette.primary) && bridge->fill.a < 255);
    // Hovering a ring segment outlines it in onSurface.
    const NodeId sibling = fx.tree.children[fx.tree.nodes[fx.tree.ancestors[0]].firstChild + 1];
    ShapeMarks marks;
    marks.hovered = ShapeId{.node = sibling, .role = shapeRole(SceneRole::Piece)};
    const auto hot = nestShapes(fx.tree, scene, marks, kPalette);
    TEST_CHECK(std::ranges::any_of(hot.shapes, [&](const Shape& shape) {
      return sceneKeyOf(shape) == SceneKey{sibling, SceneRole::Detail} && sameRgb(shape.fill, kPalette.onSurface);
    }));
  }

  void testNestThinRingsAndMerged() {
    const auto tree = deepTree();
    const auto scene = buildNestScene(tree, kSettings, 900, 600, kPalette);
    const auto out = nestShapes(tree, scene, {}, kPalette);
    std::size_t thin = 0;
    for (const auto& ring : scene.rings) {
      for (std::size_t i = ring.firstPiece; i < ring.firstPiece + ring.pieceCount; ++i) {
        const auto& piece = scene.pieces[i];
        const auto* first = findSceneShape(out, {piece.node, SceneRole::Piece});
        if (ring.thickness > 7 || piece.onPath || first == nullptr || !piece.nodes.empty()) {
          continue;
        }
        // A thin ring's segment is its accent, translucent.
        TEST_CHECK(sameRgb(first->fill, piece.color.accent) && first->fill.a < 255);
        ++thin;
      }
    }
    TEST_CHECK(thin > 0);
  }

  // A parent of one large folder and 299 small files: its ring merges the files into hatched segments.
  void testNestMergedSegment() {
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "r", .folder = true, .bytes = 2e9, .items = 300},
        {.parent = 0, .name = "here", .folder = true, .bytes = 1e9, .items = 1},
    };
    std::vector<std::string> names(299);
    for (std::size_t i = 0; i < names.size(); ++i) {
      names[i] = std::format("file{:03}.txt", i);
      rows.push_back({.parent = 0, .name = names[i], .bytes = 1000.0 + static_cast<double>(i % 7) * 400});
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, tree.root);
    const auto scene = buildNestScene(tree, kSettings, 900, 600, kPalette);
    const auto out = nestShapes(tree, scene, {}, kPalette);
    std::size_t merged = 0;
    for (const auto& piece : scene.pieces) {
      if (piece.kind == NestPieceKind::MergedSegment) {
        const auto* shape = findSceneShape(out, {piece.node, SceneRole::More});
        TEST_CHECK(shape->pattern == ShapePattern::Hatched && shape->strokeWidth == 0);
        TEST_CHECK(sameRgb(shape->fill, kPalette.surfaceContainerHighest));
        TEST_CHECK(sameRgb(shape->stroke, kPalette.onSurfaceVariant)); // the stripes
        ++merged;
      }
    }
    TEST_CHECK(merged > 0);
  }

  void testNestStates() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const auto plain = nestShapes(fx.tree, scene, {}, kPalette);
    const auto* base = findSceneShape(plain, {heavy, SceneRole::Piece});
    // The cursor: a primary ring outside the tile, and a faint halo.
    const auto cursor = nestShapes(fx.tree, scene, {.hovered = {}, .linked = kNoNode, .cursor = heavy}, kPalette);
    const auto rings = drawn(cursor, {heavy, SceneRole::Mark});
    TEST_CHECK(rings.size() == 2);
    TEST_CHECK(std::ranges::any_of(rings, [&](const Shape* ring) {
      return sameRgb(ring->stroke, kPalette.primary) && ring->stroke.a == 255 && ring->rect.x < base->rect.x;
    }));
    // The linked node: a tertiary outline and tint on the tile itself.
    const auto linked = nestShapes(fx.tree, scene, {.hovered = {}, .linked = heavy}, kPalette);
    const auto* tile = findSceneShape(linked, {heavy, SceneRole::Piece});
    TEST_CHECK(sameRgb(tile->stroke, kPalette.tertiary) && tile->strokeWidth > base->strokeWidth);
    TEST_CHECK(tile->fill != base->fill);
  }

  void testNestPeekCard() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const auto peek =
        buildNestPeek(fx.tree, kSettings, scene, nestFocus(fx.tree, scene, heavy).value(), kPalette).value();
    const auto out = nestPeekShapes(fx.tree, peek, {}, kPalette);
    const auto* card = findSceneShape(out, {peek.node, SceneRole::Head});
    TEST_CHECK(sameRgb(card->fill, kPalette.surfaceContainerLow) && sameRgb(card->stroke, kPalette.primary));
    TEST_NEAR(card->rect.radius, 9.0F, 1e-6);
    for (const auto& piece : peek.pieces) {
      if (piece.kind == NestPieceKind::PeekTile) {
        const auto* tile = findSceneShape(out, {piece.node, SceneRole::Piece});
        TEST_NEAR(tile->rect.radius, 3.0F, 1e-6);
        TEST_CHECK(sameRgb(tile->stroke, piece.color.accent) && !sameRgb(tile->fill, piece.color.fill));
      }
    }
  }
  iris::Scene irisScene(const Tree& tree, double width, double height) {
    iris::SceneInput input;
    input.width = width;
    input.height = height;
    return iris::buildScene(tree, input).value();
  }

  // The dial of a 900 by 600 host, worked by hand from the dial formula: rTotal = min((600 - 111 - 30) / 2,
  // (900 - 620) / 2) = 140, rOut = 140 - 57 = 83, rIn = round(41.5) = 42, rHub = 34.
  void testIrisDialSize() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = irisScene(fx.tree, 900, 600);
    TEST_NEAR(scene.dial.rTotal, 140.0, 1e-9);
    TEST_NEAR(scene.dial.rOut, 83.0, 1e-9);
    TEST_NEAR(scene.dial.rIn, 42.0, 1e-9);
    TEST_NEAR(scene.dial.rHub, 34.0, 1e-9);
  }

  void testIrisSectors() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = irisScene(fx.tree, 1400, 737);
    const auto out = irisShapes(fx.tree, scene, {}, kPalette);
    const double middle = (scene.dial.rIn + scene.dial.rOut) / 2;
    std::size_t cut = 0;
    for (const auto& sector : scene.main.sectors) {
      const SceneKey key = sector.kind == iris::SectorKind::Entry ? SceneKey{sector.node, SceneRole::Piece}
                                                                  : SceneKey{scene.main.folder, SceneRole::More};
      const auto* shape = firstDrawn(out, key);
      const auto* hit = hitTarget(out, key);
      TEST_CHECK(shape != nullptr && hit != nullptr);
      // Hits cover the whole arc; the drawn sector leaves 1 px each side at the middle radius.
      TEST_NEAR(hit->sector.a0, sector.arc.a0, 1e-3);
      TEST_NEAR(hit->sector.a1, sector.arc.a1, 1e-3);
      const double span = sector.arc.a1 - sector.arc.a0;
      if (span * std::numbers::pi / 180 * middle > 6) {
        TEST_NEAR((shape->sector.a0 - sector.arc.a0) * std::numbers::pi / 180 * middle, 1.0, 1e-3);
        TEST_NEAR((sector.arc.a1 - shape->sector.a1) * std::numbers::pi / 180 * middle, 1.0, 1e-3);
        ++cut;
      }
      if (sector.kind == iris::SectorKind::Entry) {
        TEST_CHECK(sameRgb(shape->fill, sector.color.fill) && shape->fill.a == 255 && shape->strokeWidth == 0);
      } else {
        TEST_CHECK(shape->pattern == ShapePattern::Hatched && sameRgb(shape->fill, kPalette.surfaceContainerHighest));
      }
    }
    TEST_CHECK(cut > 4);
    // Context rings: translucent off the path, opaque and outlined in the accent on it.
    std::size_t onPath = 0;
    for (const auto& ring : scene.rings) {
      for (const auto& sector : ring.sectors) {
        if (sector.kind != iris::SectorKind::Entry) {
          continue;
        }
        const auto* shape = firstDrawn(out, {sector.node, SceneRole::Piece});
        if (sector.onPath) {
          TEST_CHECK(shape->fill.a == 255 && sameRgb(shape->stroke, sector.color.accent) && shape->strokeWidth > 1);
          ++onPath;
        } else {
          TEST_CHECK(shape->fill.a > 100 && shape->fill.a < 160);
        }
      }
    }
    TEST_CHECK(onPath == scene.rings.size());
    // The hub is one shape that both draws and takes the click that goes up a level.
    const auto* hub = hitTarget(out, {scene.folder, SceneRole::Hub});
    TEST_CHECK(hub != nullptr && firstDrawn(out, {scene.folder, SceneRole::Hub}) == nullptr);
    TEST_CHECK(sameRgb(hub->fill, kPalette.surfaceContainerLow) && sameRgb(hub->stroke, kPalette.outlineVariant));
  }

  void testIrisRowsAndLeaders() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = irisScene(fx.tree, 1400, 737);
    const auto out = irisShapes(fx.tree, scene, {}, kPalette);
    std::size_t routes = 0;
    for (const auto& row : scene.rows.right) {
      const auto& sector = scene.main.sectors[row.sector];
      if (!row.shown || sector.kind != iris::SectorKind::Entry) {
        continue;
      }
      const auto lines = drawn(out, {sector.node, SceneRole::Leader});
      TEST_CHECK(lines.size() + 1 == row.leader.size());
      if (lines.size() >= 3) {
        // The ends of a leader are stronger than its run along the dial.
        TEST_CHECK(lines.front()->fill.a > lines[1]->fill.a && lines.back()->fill.a > lines[1]->fill.a);
        ++routes;
      }
      // A dot in the entry's accent at the start of the row, and one where the leader leaves the ring.
      const auto details = drawn(out, {sector.node, SceneRole::Detail});
      TEST_CHECK(std::ranges::any_of(details, [&](const Shape* dot) {
        return dot->kind == ShapeKind::Circle
            && sameRgb(dot->fill, sector.color.accent)
            && dot->circle.center.x > row.box.x
            && dot->circle.center.x < row.box.x + 20;
      }));
      TEST_CHECK(std::ranges::any_of(details, [&](const Shape* dot) {
        return dot->kind == ShapeKind::Circle && std::abs(dot->circle.center.x - row.leader.front().x) < 1e-3;
      }));
    }
    TEST_CHECK(routes > 0);
    // A hovered row gets a raised background; its sector turns to its accent and its leader is drawn in it.
    const auto& row = scene.rows.right.front();
    const auto& sector = scene.main.sectors[row.sector];
    ShapeMarks marks;
    marks.hovered = ShapeId{.node = sector.node, .role = shapeRole(SceneRole::Row)};
    const auto hot = irisShapes(fx.tree, scene, marks, kPalette);
    const auto* rowShape = firstDrawn(hot, {sector.node, SceneRole::Row});
    TEST_CHECK(sameRgb(rowShape->fill, kPalette.surfaceContainerHigh) && rowShape->fill.a == 255);
    TEST_CHECK(firstDrawn(out, {sector.node, SceneRole::Row})->fill.a == 0);
    TEST_CHECK(sameRgb(firstDrawn(hot, {sector.node, SceneRole::Piece})->fill, sector.color.accent));
    for (const auto* line : drawn(hot, {sector.node, SceneRole::Leader})) {
      TEST_CHECK(sameRgb(line->fill, sector.color.accent) && line->fill.a == 255);
    }
    TEST_CHECK(hitTarget(out, {sector.node, SceneRole::Row})->fill.a == 0);
  }

  void testIrisMarksAndStack() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = irisScene(fx.tree, 1400, 737);
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const auto cursor = irisShapes(fx.tree, scene, {.hovered = {}, .linked = kNoNode, .cursor = heavy}, kPalette);
    const auto* ring = firstDrawn(cursor, {heavy, SceneRole::Mark});
    TEST_CHECK(ring != nullptr && sameRgb(ring->stroke, kPalette.primary) && ring->fill.a == 0);
    TEST_CHECK(ring->sector.r0 < scene.dial.rIn && ring->sector.r1 > scene.dial.rOut);
    const auto linked = irisShapes(fx.tree, scene, {.hovered = {}, .linked = heavy}, kPalette);
    const auto* focus = firstDrawn(linked, {heavy, SceneRole::Mark});
    TEST_CHECK(focus != nullptr && sameRgb(focus->stroke, kPalette.tertiary));
    TEST_CHECK(focus->pattern == ShapePattern::DashedOutline);
    // The ancestor stack: a hit target per row, as wide as its text, with the text inside its padding.
    TEST_CHECK(!scene.stack.rows.empty());
    const auto out = irisShapes(fx.tree, scene, {}, kPalette);
    for (const auto& row : scene.stack.rows) {
      const auto* shape = hitTarget(out, {row.node, SceneRole::Stack});
      TEST_CHECK(shape != nullptr && sameRect(shape->rect, row.box.x, row.box.y, row.box.width, row.box.height));
      TEST_NEAR(shape->opacity, row.opacity, 1e-6);
      TEST_CHECK(std::ranges::any_of(out.labels, [&](const ShapeLabel& label) {
        return label.node == row.node
            && label.text == row.text
            && label.align == LabelAlign::Start
            && std::abs(label.x - (row.box.x + iris::kStackPad)) < 1e-3;
      }));
    }
    const auto& top = scene.stack.rows.front();
    ShapeMarks marks;
    marks.hovered = ShapeId{.node = top.node, .role = shapeRole(SceneRole::Stack)};
    const auto hot = irisShapes(fx.tree, scene, marks, kPalette);
    TEST_CHECK(sameRgb(hitTarget(hot, {top.node, SceneRole::Stack})->fill, kPalette.surfaceContainerHigh));
  }

  void testIrisInnerBand() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = irisScene(fx.tree, 1400, 737);
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const auto band = iris::innerBand(fx.tree, scene, heavy, kSettings, kPalette).value();
    const auto shapes = irisInnerBandShapes(scene, band, kPalette);
    TEST_CHECK(shapes.size() == band.sectors.size() + 1);
    const auto& backing = shapes.front();
    TEST_CHECK(sameRgb(backing.fill, kPalette.surface) && backing.fill.a > 150 && backing.fill.a < 255);
    TEST_NEAR(backing.sector.r0, band.backing.r0, 1e-3);
    TEST_NEAR(backing.sector.r1, band.backing.r1, 1e-3);
    for (std::size_t i = 0; i < band.sectors.size(); ++i) {
      const auto& shape = shapes[i + 1];
      const auto& sector = band.sectors[i];
      TEST_CHECK(!shape.hittable && shape.kind == ShapeKind::Sector);
      TEST_NEAR(shape.sector.r0, band.band.r0, 1e-3);
      TEST_NEAR(shape.sector.r1, band.band.r1, 1e-3);
      TEST_CHECK(shape.sector.a0 > sector.arc.a0 && shape.sector.a1 < sector.arc.a1);
      const Rgb color = sector.kind == iris::SectorKind::Entry ? sector.color.accent : kPalette.outlineVariant;
      TEST_CHECK(sameRgb(shape.fill, color));
    }
  }

} // namespace

int main() {
  testNestTilesAndCore();
  testNestRings();
  testNestThinRingsAndMerged();
  testNestMergedSegment();
  testNestStates();
  testNestPeekCard();
  testIrisDialSize();
  testIrisSectors();
  testIrisRowsAndLeaders();
  testIrisMarksAndStack();
  testIrisInnerBand();
  return 0;
}
