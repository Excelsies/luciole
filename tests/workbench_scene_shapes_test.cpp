#include "test_check.h"
#include "workbench/atlas_camera.h"
#include "workbench/flow_hit.h"
#include "workbench/format.h"
#include "workbench/iris_hit.h"
#include "workbench/pane_layout.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"
#include "workbench_scene_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <print>
#include <string>
#include <utility>
#include <vector>

namespace {
  using namespace luciole::workbench;

  const LayoutSettings kSettings{};
  const ColorPalette kPalette{};

  using Key = luciole::test::SceneKey;
  using SceneHit = std::function<std::optional<Key>(double, double)>;
  using luciole::test::sameRect;

  Key keyOf(const Shape& shape) { return luciole::test::sceneKeyOf(shape); }

  luciole::test::SceneFixture fixture() { return luciole::test::sceneFixture(); }

  bool sameKey(const std::optional<std::size_t>& hit, const SceneShapes& out, const std::optional<Key>& key) {
    return hit.has_value() == key.has_value() && (!hit || keyOf(out.shapes[*hit]) == *key);
  }

  // On a 3 px grid: where the shapes give one piece also 2 px around and no other piece is that close, the scene names
  // that piece; where the scene names a piece, a shape of that piece lies within `slack`.
  void checkAgreement(const SceneShapes& out, const SceneHit& sceneHit, double width, double height, float slack) {
    std::size_t agreed = 0;
    for (double y = 0; y <= height; y += 3) {
      for (double x = 0; x <= width; x += 3) {
        const ShapePoint p{.x = static_cast<float>(x), .y = static_cast<float>(y)};
        const auto hit = hitShape(out.shapes, p, 0);
        const auto key = hit ? std::optional(keyOf(out.shapes[*hit])) : std::nullopt;
        // No other piece within 2 px: the topmost shape within reach is the same one.
        bool stable = sameKey(hitShape(out.shapes, p, 2), out, key);
        for (const auto& [dx, dy] : {std::pair(-2.0F, 0.0F), {2.0F, 0.0F}, {0.0F, -2.0F}, {0.0F, 2.0F}}) {
          stable = stable && sameKey(hitShape(out.shapes, {.x = p.x + dx, .y = p.y + dy}, 0), out, key);
        }
        const auto expected = sceneHit(x, y);
        if (stable && key) {
          if (expected != key) {
            std::println(stderr, "at {} {}: shapes give node {}", x, y, key->node);
          }
          TEST_CHECK(expected == key);
          ++agreed;
        }
        if (expected) {
          const bool near = std::ranges::any_of(out.shapes, [&](const Shape& shape) {
            return shape.hittable && keyOf(shape) == *expected && shapeContains(shape, p, slack);
          });
          if (!near) {
            std::println(stderr, "at {} {}: no shape of node {} near", x, y, expected->node);
          }
          TEST_CHECK(near);
        }
      }
    }
    TEST_CHECK(agreed > 200);
  }

  const Shape* findShape(const SceneShapes& out, Key key) { return luciole::test::findSceneShape(out, key); }

  Key nestKey(const NestPiece& piece) {
    const bool merged = piece.kind == NestPieceKind::More
        || piece.kind == NestPieceKind::MergedSegment
        || (piece.kind == NestPieceKind::LineSegment && !piece.nodes.empty());
    return {piece.node, merged ? SceneRole::More : SceneRole::Piece};
  }

  void testNest() {
    auto fx = fixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    TEST_CHECK(!scene.rings.empty());
    const auto out = nestShapes(fx.tree, scene, {}, kPalette);
    for (const auto& piece : scene.pieces) {
      if (piece.kind == NestPieceKind::Tile || piece.kind == NestPieceKind::More) {
        const auto* shape = findShape(out, nestKey(piece));
        TEST_CHECK(shape != nullptr && shape->kind == ShapeKind::RoundedRect);
        TEST_CHECK(sameRect(shape->rect, piece.rect.x, piece.rect.y, piece.rect.w, piece.rect.h));
        continue;
      }
      // Every corner of the scene's outline lies on the piece's shapes.
      for (const auto& point : piece.outline) {
        const ShapePoint p{.x = static_cast<float>(point.x), .y = static_cast<float>(point.y)};
        TEST_CHECK(std::ranges::any_of(out.shapes, [&](const Shape& shape) {
          return keyOf(shape) == nestKey(piece) && shapeContains(shape, p, 0.05F);
        }));
      }
    }
    const auto sceneHit = [&scene](double x, double y) -> std::optional<Key> {
      const auto piece = nestHit(scene, x, y);
      return piece ? std::optional(nestKey(scene.pieces[*piece])) : std::nullopt;
    };
    // Tiles take hits 1 px outside their square corners; drawn with a 6 px radius they are 3.9 px away there.
    checkAgreement(out, sceneHit, 900, 600, 4.0F);
  }

  void testNestStatesAndLabels() {
    auto fx = fixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    const auto plain = nestShapes(fx.tree, scene, {}, kPalette);
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const auto* base = findShape(plain, {heavy, SceneRole::Piece});
    TEST_CHECK(base != nullptr && base->strokeWidth == 1 && base->pattern == ShapePattern::Solid);
    TEST_CHECK(findShape(plain, {fx.partial, SceneRole::Piece})->pattern == ShapePattern::Hatched);
    ShapeMarks marks;
    marks.hovered = ShapeId{.node = heavy, .role = shapeRole(SceneRole::Piece)};
    const auto hoveredScene = nestShapes(fx.tree, scene, marks, kPalette);
    const auto* hovered = findShape(hoveredScene, {heavy, SceneRole::Piece});
    TEST_CHECK(hovered->strokeWidth > 0 && hovered->fill != base->fill);
    // A linked node deep below a tile marks the tile that holds it.
    const NodeId deep = fx.tree.children[fx.tree.nodes[heavy].firstChild];
    const auto linkedScene = nestShapes(fx.tree, scene, {.hovered = {}, .linked = deep}, kPalette);
    const auto* linked = findShape(linkedScene, {heavy, SceneRole::Piece});
    TEST_CHECK(linked->strokeWidth > hovered->strokeWidth);
    const auto cursorScene = nestShapes(fx.tree, scene, {.hovered = {}, .linked = kNoNode, .cursor = heavy}, kPalette);
    const auto* cursor = findShape(cursorScene, {heavy, SceneRole::Piece});
    TEST_CHECK(cursor->strokeWidth > 0);
    // The on-path segment of the parent's ring has an outline and a bridge.
    TEST_CHECK(findShape(plain, {fx.work, SceneRole::Detail}) != nullptr);
    TEST_CHECK(findShape(plain, {fx.work, SceneRole::Bridge}) != nullptr);
    TEST_CHECK(!plain.labels.empty());
    TEST_CHECK(plain.labels.size() <= kMaxSceneLabels);
  }

  void testNestPeek() {
    auto fx = fixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const auto tile = nestFocus(fx.tree, scene, heavy).value();
    const auto peek = buildNestPeek(fx.tree, kSettings, scene, tile, kPalette).value();
    const auto out = nestPeekShapes(fx.tree, peek, {}, kPalette);
    const auto peekKey = [&peek](std::size_t index) {
      const auto& piece = peek.pieces[index];
      return piece.kind == NestPieceKind::PeekHead ? Key{piece.node, SceneRole::Head} : nestKey(piece);
    };
    for (std::size_t i = 1; i < peek.pieces.size(); ++i) {
      const auto& r = peek.pieces[i].rect;
      const auto* shape = findShape(out, peekKey(i));
      TEST_CHECK(shape != nullptr && sameRect(shape->rect, r.x, r.y, r.w, r.h));
    }
    const auto sceneHit = [&](double x, double y) -> std::optional<Key> {
      const auto piece = nestPeekHit(peek, x, y);
      return piece ? std::optional(peekKey(*piece)) : std::nullopt;
    };
    // The card hits its head anywhere outside a tile; its corners are rounded by 9 px, 3.73 px away from the corner.
    checkAgreement(out, sceneHit, 900, 600, 3.8F);
  }

  Tree manyFiles() { return luciole::test::manyFilesTree(); }

  void testLabelCap() {
    const auto tree = manyFiles();
    const auto scene = buildNestScene(tree, kSettings, 1800, 1200, kPalette);
    const auto out = nestShapes(tree, scene, {}, kPalette);
    TEST_CHECK(out.labels.size() == kMaxSceneLabels);
    const auto area = [&out](const ShapeLabel& label) {
      // The "+N more" tile labels with the folder's node.
      const auto* shape = findShape(out, {label.node, SceneRole::Piece});
      shape = shape != nullptr ? shape : findShape(out, {label.node, SceneRole::More});
      TEST_CHECK(shape != nullptr);
      return shape->rect.width * shape->rect.height;
    };
    TEST_CHECK(area(out.labels.front()) >= area(out.labels.back()));
    TEST_CHECK(nestShapes(tree, NestScene{}, {}, kPalette).shapes.empty());
  }

  Key sectorKey(const iris::SectorRing& ring, std::size_t index) {
    const auto& sector = ring.sectors[index];
    return sector.kind == iris::SectorKind::Entry ? Key{sector.node, SceneRole::Piece}
                                                  : Key{ring.folder, SceneRole::More};
  }

  struct RowCount {
    std::size_t entries = 0;
    std::size_t merged = 0;
  };

  // Hovering a shown label row tints and strokes the sector it names and no other sector.
  RowCount checkRowHover(const Tree& tree, const iris::Scene& scene) {
    const auto plain = irisShapes(tree, scene, {}, kPalette);
    RowCount count;
    for (const auto* rows : {&scene.rows.right, &scene.rows.left}) {
      for (const auto& row : *rows) {
        if (!row.shown) {
          continue;
        }
        const auto key = sectorKey(scene.main, row.sector);
        ShapeMarks marks;
        marks.hovered = ShapeId{.node = key.node, .role = shapeRole(SceneRole::Row)};
        const auto hovered = irisShapes(tree, scene, marks, kPalette);
        for (std::size_t i = 0; i < plain.shapes.size(); ++i) {
          const auto& before = plain.shapes[i];
          const auto& after = hovered.shapes[i];
          if (before.kind == ShapeKind::Sector && !before.hittable && keyOf(before).role != SceneRole::Mark) {
            TEST_CHECK((after.fill != before.fill) == (keyOf(before) == key));
          }
        }
        ++(key.role == SceneRole::More ? count.merged : count.entries);
      }
    }
    return count;
  }

  void testIris() {
    auto fx = fixture();
    iris::SceneInput input;
    input.width = 1400;
    input.height = 737;
    const auto scene = iris::buildScene(fx.tree, input).value();
    const auto out = irisShapes(fx.tree, scene, {}, kPalette);
    for (const auto* ring : {&scene.main}) {
      for (std::size_t i = 0; i < ring->sectors.size(); ++i) {
        const auto* shape = luciole::test::hitTarget(out, sectorKey(*ring, i));
        TEST_CHECK(shape != nullptr && shape->kind == ShapeKind::Sector);
        TEST_NEAR(shape->sector.a0, ring->sectors[i].arc.a0, 1e-3);
        TEST_NEAR(shape->sector.a1, ring->sectors[i].arc.a1, 1e-3);
        TEST_NEAR(shape->sector.r0, scene.dial.rIn, 1e-3);
        TEST_NEAR(shape->sector.r1, scene.dial.rOut, 1e-3);
      }
    }
    const auto* hub = findShape(out, {scene.folder, SceneRole::Hub});
    TEST_CHECK(hub != nullptr && hub->kind == ShapeKind::Circle);
    TEST_NEAR(hub->circle.radius, scene.dial.rHub, 1e-3);
    const auto sceneHit = [&](double x, double y) -> std::optional<Key> {
      // The ancestor stack lies above the dial, apart from everything iris::hitTest finds.
      for (const auto& row : scene.stack.rows) {
        const auto& box = row.box;
        if (x >= box.x && x <= box.x + box.width && y >= box.y && y <= box.y + box.height) {
          return Key{row.node, SceneRole::Stack};
        }
      }
      const auto hit = iris::hitTest(scene, {.x = x, .y = y});
      if (!hit) {
        return std::nullopt;
      }
      switch (hit->kind) {
      case iris::HitKind::Hub:
        return Key{scene.folder, SceneRole::Hub};
      case iris::HitKind::Row:
        return Key{sectorKey(scene.main, hit->sector).node, SceneRole::Row};
      case iris::HitKind::Arc:
        return sectorKey(scene.main, hit->sector);
      case iris::HitKind::Context:
        return sectorKey(scene.rings[hit->ring], hit->sector);
      }
      return std::nullopt;
    };
    checkAgreement(out, sceneHit, 1400, 737, 1.55F);
    TEST_CHECK(checkRowHover(fx.tree, scene).entries > 4);
  }

  // The "+N more" row of a folder too full to show every entry marks the merged sector.
  void testIrisMergedRow() {
    const auto tree = manyFiles();
    iris::SceneInput input;
    input.width = 1400;
    input.height = 737;
    const auto scene = iris::buildScene(tree, input).value();
    const auto rows = checkRowHover(tree, scene);
    TEST_CHECK(rows.entries > 4 && rows.merged == 1);
  }

  // A compact dial names its arcs inside them.
  void testIrisCompactArcLabels() {
    auto fx = fixture();
    iris::SceneInput input;
    input.width = 340;
    input.height = 380;
    input.compact = true;
    const auto scene = iris::buildScene(fx.tree, input).value();
    TEST_CHECK(scene.dial.compact && !scene.arcLabels.empty());
    const auto out = irisShapes(fx.tree, scene, {}, kPalette);
    for (const auto& arc : scene.arcLabels) {
      const auto& sector = scene.main.sectors[arc.sector];
      const auto found =
          std::ranges::find_if(out.labels, [&arc](const ShapeLabel& label) { return label.text == arc.text; });
      TEST_CHECK(found != out.labels.end());
      TEST_CHECK(found->node == sector.node && found->align == LabelAlign::Center);
      TEST_NEAR(found->x + found->width / 2, arc.center.x, 1e-3);
      TEST_NEAR(found->y + found->height / 2, arc.center.y, 1e-3);
      TEST_NEAR(found->width, arc.width, 1e-3);
      TEST_NEAR(found->fontSize, 11.0F, 1e-6);
    }
  }

  void testAtlas() {
    auto fx = fixture();
    PackingCache packings;
    const AtlasSource source{.tree = fx.tree, .settings = kSettings, .packings = packings};
    const ViewSize view{.width = 800, .height = 600};
    const auto scene = buildScene(source, fx.work, restCamera(view), view);
    TEST_CHECK(scene.items.size() > 10);
    const auto out = atlasShapes(fx.tree, kSettings, scene, {}, kPalette, LabelFont{});
    // Hit targets follow the clickable items in painter order, each at the item's own radius.
    std::vector<const Shape*> hits;
    for (const auto& shape : out.shapes) {
      if (shape.hittable) {
        hits.push_back(&shape);
      }
    }
    std::size_t next = 0;
    for (const auto& item : scene.items) {
      if (!item.clickable) {
        continue;
      }
      TEST_CHECK(next < hits.size());
      const auto& shape = *hits[std::min(next++, hits.size() - 1)];
      if (item.huge) {
        TEST_CHECK(shape.kind == ShapeKind::Rect && sameRect(shape.rect, 0, 0, 800, 600));
      } else {
        TEST_CHECK(shape.kind == ShapeKind::Circle);
        TEST_NEAR(shape.circle.center.x, item.x, 1e-3);
        TEST_NEAR(shape.circle.radius, item.r, 1e-3);
      }
    }
    TEST_CHECK(next == hits.size());
    const auto sceneHit = [&](double x, double y) -> std::optional<Key> {
      const auto hit = hitTest(scene, x, y);
      if (!hit) {
        return std::nullopt;
      }
      const auto& item = scene.items[*hit];
      return item.role == BubbleRole::More ? Key{item.folder, SceneRole::More} : Key{item.node, SceneRole::Piece};
    };
    checkAgreement(out, sceneHit, 800, 600, 0.01F);
  }

  Key flameKey(const FlameItem& item) {
    if (item.bar) {
      return {item.node, SceneRole::Path};
    }
    if (item.far) {
      return {item.node, SceneRole::Far};
    }
    return item.more ? Key{item.owner, SceneRole::More} : Key{item.node, SceneRole::Piece};
  }

  void testFlame() {
    auto fx = fixture();
    LabelFont font;
    font.ascii.fill(7);
    font.other = 7;
    const auto scene =
        flameScene(fx.tree, kSettings, 1000, 266, {.firstAhead = font, .ahead = font, .ancestor = font, .value = font});
    const auto out = flameShapes(fx.tree, kSettings, scene, {}, kPalette);
    for (const auto& row : scene.rows) {
      for (const auto& item : row.items) {
        const auto box = flameBox(item.x, item.w, row.row.h, item.bar);
        const auto* shape = findShape(out, flameKey(item));
        TEST_CHECK(shape != nullptr && sameRect(shape->rect, box.x, row.row.y, box.w, row.row.h));
        TEST_NEAR(shape->rect.radius, item.bar ? 4.0 : box.r, 1e-3); // the current folder's bar is rounder
      }
    }
    const auto sceneHit = [&scene](double x, double y) -> std::optional<Key> {
      const auto hit = flameHitAt(scene, x, y);
      return hit ? std::optional(flameKey(scene.rows[hit->row].items[hit->item])) : std::nullopt;
    };
    // A cell takes hits across its 1 px gap and up to half a 4 px row gap, and its corners are rounded by up to
    // 3 px: at a corner that is 3.4 px from the drawn shape.
    checkAgreement(out, sceneHit, 1000, 266, 3.5F);
    TEST_CHECK(!out.labels.empty());
  }

  void testLadder() {
    auto fx = fixture();
    const auto scene = ladderScene(fx.tree, kSettings, 544, 100);
    TEST_CHECK(!scene.rows.empty());
    const auto out = ladderShapes(fx.tree, kSettings, scene, {}, kPalette);
    for (const auto& row : scene.rows) {
      for (const auto& piece : row.pieces) {
        const auto* shape = findShape(out, {piece.node, SceneRole::Piece});
        TEST_CHECK(shape != nullptr && sameRect(shape->rect, piece.x, row.y, piece.w, row.h));
        TEST_CHECK(shape->opacity == 1 && shape->fill.a == 255); // far bars fade by color, see the bars test
        TEST_CHECK((findShape(out, {piece.node, SceneRole::Mark}) != nullptr) == piece.onPath);
      }
    }
  }

  Key flowKey(const FlowTopology& topology, std::size_t index) {
    const auto& record = topology.records[index];
    switch (record.kind) {
    case FlowRecordKind::More:
      return {record.node, SceneRole::More};
    case FlowRecordKind::Files:
      return {record.node, SceneRole::Files, record.column};
    default:
      return {record.node, SceneRole::Piece};
    }
  }

  Key ribbonKey(const FlowTopology& topology, std::size_t index) {
    const auto& record = topology.records[index];
    return {record.node, record.kind == FlowRecordKind::More ? SceneRole::MoreRibbon : SceneRole::Ribbon};
  }

  void testFlow() {
    auto fx = fixture();
    const auto layout = flowLayout(fx.tree, kSettings, 900, 300);
    TEST_CHECK(!layout.ribbons.empty());
    // Every character 6 px wide at 12 px.
    LabelFont font;
    font.ascii.fill(6);
    font.other = 6;
    const auto out = flowShapes(fx.tree, kSettings, layout, {}, kPalette, font);
    const auto& records = layout.topology.records;
    for (std::size_t i = 0; i < records.size(); ++i) {
      const auto* bar = findShape(out, flowKey(layout.topology, i));
      TEST_CHECK(bar != nullptr && bar->kind == ShapeKind::RoundedRect);
      TEST_CHECK(sameRect(bar->rect, records[i].x, records[i].y, records[i].width, records[i].height));
    }
    for (const auto& ribbon : layout.ribbons) {
      const auto* shape = findShape(out, ribbonKey(layout.topology, ribbon.record));
      TEST_CHECK(shape != nullptr && shape->kind == ShapeKind::Ribbon);
      TEST_NEAR(shape->ribbon.top[1].x, ribbon.top[1].x, 1e-3);
      TEST_NEAR(shape->ribbon.top[2].y, ribbon.top[2].y, 1e-3);
      TEST_NEAR(shape->ribbon.bottom[0].y, ribbon.bottom[3].y, 1e-3);
      TEST_NEAR(shape->ribbon.bottom[3].y, ribbon.bottom[0].y, 1e-3);
    }
    // Ribbons take no hits; a label answers over its text: the name, and the value on a second line when it fits,
    // at 6 px a code point for the name and 5.5 px for the 11 px value.
    std::vector<double> widths;
    for (const auto& label : layout.labels) {
      const auto& topology = layout.topology;
      const auto name = 6.0 * static_cast<double>(codePointCount(flowRecordName(fx.tree, topology, label.record)));
      const auto value =
          5.5 * static_cast<double>(codePointCount(flowRecordValue(fx.tree, topology, label.record, kSettings.sizeBy)));
      const bool valueShown = label.lines == 2 && value <= label.maxWidth;
      widths.push_back(std::max(std::min(name, label.maxWidth), valueShown ? value : 0));
    }
    const auto sceneHit = [&layout, &widths](double x, double y) -> std::optional<Key> {
      if (const auto hit = flowHitAt(layout, widths, x, y)) {
        if (hit->kind == FlowHitKind::Stem) {
          return Key{layout.stem[hit->index].node, SceneRole::Stem};
        }
        return flowKey(layout.topology, hit->index);
      }
      return std::nullopt;
    };
    checkAgreement(out, sceneHit, 900, 300, 0.05F);
    // Hovering a record lights its ribbon and dims the others.
    const auto& target = layout.ribbons.front();
    ShapeMarks marks;
    marks.hovered = ShapeId{.node = records[target.record].node, .role = shapeRole(SceneRole::Piece)};
    const auto lit = flowShapes(fx.tree, kSettings, layout, marks, kPalette, LabelFont{});
    const auto* on = findShape(lit, ribbonKey(layout.topology, target.record));
    const auto* off = findShape(lit, ribbonKey(layout.topology, layout.ribbons.back().record));
    TEST_CHECK(on->opacity > findShape(out, ribbonKey(layout.topology, target.record))->opacity);
    TEST_CHECK(off->opacity < on->opacity);
    // Every files bar has the current folder's node; hovering one keeps only that column's bar at full strength.
    std::vector<Key> files;
    for (std::size_t i = 0; i < records.size(); ++i) {
      if (records[i].kind == FlowRecordKind::Files) {
        files.push_back(flowKey(layout.topology, i));
      }
    }
    TEST_CHECK(files.size() >= 2);
    for (const auto& key : files) {
      ShapeMarks filesMarks;
      filesMarks.hovered = ShapeId{.node = key.node, .role = shapeRole(SceneRole::Files, key.part)};
      const auto hovered = flowShapes(fx.tree, kSettings, layout, filesMarks, kPalette, LabelFont{});
      for (const auto& other : files) {
        const auto* bar = findShape(hovered, other);
        const auto* plain = findShape(out, other);
        TEST_CHECK(bar != nullptr && plain != nullptr);
        TEST_CHECK((bar->opacity == plain->opacity) == (other == key));
      }
    }
  }
} // namespace

int main() {
  testNest();
  testNestStatesAndLabels();
  testNestPeek();
  testLabelCap();
  testIris();
  testIrisMergedRow();
  testIrisCompactArcLabels();
  testAtlas();
  testFlame();
  testLadder();
  testFlow();
  return 0;
}
