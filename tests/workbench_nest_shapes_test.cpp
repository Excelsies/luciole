#include "test_check.h"
#include "workbench/nest_scene.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"
#include "workbench_nest_fixture.h"
#include "workbench_scene_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <print>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// How the Nest draws the open path's outline, the linked tile's glow and the labels of tiles, "+N more" and the card.
namespace {
  using namespace luciole::workbench;
  using luciole::test::drawn;
  using luciole::test::sameRgb;

  const LayoutSettings kSettings{};
  const ColorPalette kPalette{};

  const NestPiece* onPathSegment(const NestScene& scene, std::size_t level) {
    for (const auto& ring : scene.rings) {
      if (ring.level != level) {
        continue;
      }
      for (std::size_t i = ring.firstPiece; i < ring.firstPiece + ring.pieceCount; ++i) {
        if (scene.pieces[i].onPath) {
          return &scene.pieces[i];
        }
      }
    }
    return nullptr;
  }

  // The outline of an on-path segment follows its corners with arcs: straight sides and arcs as bands, the two
  // ends as lines, and every point of the scene's outline lies on them.
  void thePathOutlineIsSmooth() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    const auto* segment = onPathSegment(scene, 1);
    TEST_CHECK(segment != nullptr && segment->node == fx.work);
    const auto out = nestShapes(fx.tree, scene, {}, kPalette);
    // The core's frame belongs to the current folder too; it is the one rounded rectangle.
    auto edges = drawn(out, {fx.work, SceneRole::Detail});
    std::erase_if(edges, [](const Shape* shape) { return shape->kind == ShapeKind::RoundedRect; });
    std::size_t sectors = 0;
    std::size_t lines = 0;
    for (const auto* shape : edges) {
      sectors += shape->kind == ShapeKind::Sector ? 1U : 0U;
      lines += shape->kind == ShapeKind::Line ? 1U : 0U;
      TEST_CHECK(shape->kind == ShapeKind::Sector || shape->kind == ShapeKind::Rect || shape->kind == ShapeKind::Line);
    }
    std::println("  outline: {} shapes, {} arcs, {} lines", edges.size(), sectors, lines);
    TEST_CHECK(sectors >= 1 && lines == 2);
    // The arcs are thin bands as wide as the line.
    for (const auto* shape : edges) {
      if (shape->kind == ShapeKind::Sector) {
        TEST_NEAR(shape->sector.r1 - shape->sector.r0, 1.5F, 1e-4F);
      }
    }
    for (const auto& point : segment->outline) {
      const ShapePoint p{.x = static_cast<float>(point.x), .y = static_cast<float>(point.y)};
      TEST_CHECK(std::ranges::any_of(edges, [&p](const Shape* shape) { return shapeContains(*shape, p, 0.8F); }));
    }
  }

  // The linked tile glows: one soft tertiary outline around its edge that fades out several px past it.
  void theLinkedTileGlows() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const auto plain = nestShapes(fx.tree, scene, {}, kPalette);
    TEST_CHECK(drawn(plain, {heavy, SceneRole::Mark}).empty());
    const auto linked = nestShapes(fx.tree, scene, {.hovered = {}, .linked = heavy}, kPalette);
    const auto glow = drawn(linked, {heavy, SceneRole::Mark});
    TEST_CHECK(glow.size() == 1);
    const Shape* tile = nullptr;
    for (const auto& shape : linked.shapes) {
      tile = shape.hittable && shape.id.node == heavy && sceneRole(shape.id.role) == SceneRole::Piece ? &shape : tile;
    }
    TEST_CHECK(tile != nullptr);
    const auto* ring = glow.front();
    TEST_CHECK(ring->kind == ShapeKind::RoundedRect && ring->fill.a == 0 && ring->strokeWidth > 0);
    TEST_CHECK(sameRgb(ring->stroke, kPalette.tertiary) && ring->stroke.a < 255);
    TEST_CHECK(ring->rect.x < tile->rect.x && ring->rect.width > tile->rect.width);
    TEST_CHECK(ring->softness >= 6 && ring->rect.x - ring->softness <= tile->rect.x - 6);
    // The glow lies over the neighboring tiles: it comes after every tile.
    const auto last = std::ranges::find_if(linked.shapes, [&](const Shape& shape) {
      return shape.id.node == heavy && sceneRole(shape.id.role) == SceneRole::Mark;
    });
    TEST_CHECK(std::none_of(last, linked.shapes.end(), [](const Shape& shape) {
      return shape.hittable && sceneRole(shape.id.role) == SceneRole::Piece;
    }));
  }

  // A tile name with room for an icon carries the entry's category, at the size and place the label gives.
  void tileNamesCarryTheirIcon() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    const auto out = nestShapes(fx.tree, scene, {}, kPalette);
    std::size_t withIcon = 0;
    for (const auto& piece : scene.pieces) {
      const auto* tile = std::get_if<NestTileLabel>(&piece.label);
      if (piece.kind != NestPieceKind::Tile || tile == nullptr) {
        continue;
      }
      const auto name = std::ranges::find_if(out.labels, [&](const ShapeLabel& label) {
        return label.node == piece.node && label.text == tile->name;
      });
      TEST_CHECK(name != out.labels.end());
      if (name == out.labels.end()) {
        continue;
      }
      TEST_NEAR(name->icon.size, static_cast<float>(tile->iconSize), 1e-4F);
      if (tile->iconSize == 0) {
        continue;
      }
      ++withIcon;
      const auto category = piece.folder ? FileCategory::Folder : fileCategory(fx.tree.nodes[piece.node].name, false);
      TEST_CHECK(name->icon.category == category);
      TEST_NEAR(name->x + name->icon.left, static_cast<float>(tile->icon.x), 1e-3F);
      TEST_NEAR(name->y + name->icon.top, static_cast<float>(tile->icon.y), 1e-3F);
      TEST_CHECK(name->icon.color == name->color);
      // Only the name carries the icon.
      for (const auto& label : out.labels) {
        TEST_CHECK(label.node != piece.node || label.text == tile->name || label.icon.size == 0);
      }
    }
    TEST_CHECK(withIcon >= 3);
  }
  const ShapeLabel* labelOf(const SceneShapes& out, NodeId node, std::string_view text) {
    const auto found = std::ranges::find_if(out.labels, [&](const ShapeLabel& label) {
      return label.node == node && label.text == text;
    });
    return found == out.labels.end() ? nullptr : &*found;
  }

  // Names are semibold and may be cut; a tile's measure is a whole trail of its name, under it on a tall tile and at
  // the right end of a one-line tile; "+N more" is whole, falls back to "+N", and carries its size line under it.
  void measuresAndCountsAreWholeTrails() {
    auto fx = luciole::test::nest::fixture();
    std::size_t under = 0;
    std::size_t beside = 0;
    // Depth 12 folds most of its 400 files; depth 2 has few, tall tiles.
    struct Case {
      std::size_t depth;
      bool flat;
    };
    for (const auto [depth, flat] : {Case{12, false}, Case{12, true}, Case{2, false}}) {
      fx.at(depth);
      auto scene = buildNestScene(fx.tree, kSettings, flat ? 900 : 340, flat ? 600 : 380, kPalette);
      // The same tiles labeled as if 30 px tall, so every label is one line.
      for (auto& piece : scene.pieces) {
        if (flat && piece.kind == NestPieceKind::Tile) {
          auto box = piece.rect;
          box.h = 30;
          piece.label = nestTileLabel(fx.tree, kSettings, piece.node, box).value_or(NestTileLabel{});
        }
      }
      const auto out = nestShapes(fx.tree, scene, {}, kPalette);
      for (const auto& piece : scene.pieces) {
        const auto* tile = std::get_if<NestTileLabel>(&piece.label);
        if (piece.kind != NestPieceKind::Tile || tile == nullptr || tile->name.empty()) {
          continue;
        }
        const auto* name = labelOf(out, piece.node, tile->name);
        TEST_CHECK(name != nullptr && labelOf(out, piece.node, tile->detail) == nullptr);
        if (name == nullptr) {
          continue;
        }
        TEST_CHECK(name->weight == LabelWeight::Semibold && name->fit == LabelFit::Shorten);
        TEST_CHECK(name->trail.has_value() && name->trail->text == tile->detail && name->trail->fontSize == 11);
        if (!name->trail) {
          continue;
        }
        if (tile->twoLines) {
          ++under;
          TEST_CHECK(name->trail->place == TrailPlace::Below);
          TEST_NEAR(name->x + name->trail->dx, static_cast<float>(tile->detailAt.x), 1e-3F);
          TEST_NEAR(name->trail->width, static_cast<float>(tile->detailWidth), 1e-3F);
          const double middle = tile->detailAt.y - 0.35 * 11;
          TEST_NEAR(name->y + name->height / 2 + name->trail->dy, static_cast<float>(middle), 1e-3F);
        } else {
          ++beside;
          TEST_CHECK(name->trail->place == TrailPlace::End);
          TEST_NEAR(name->trail->gap, 14.0F, 1e-4F);
          TEST_NEAR(name->trail->room, static_cast<float>(tile->detailWidth), 1e-3F);
          TEST_NEAR(name->x + name->width, static_cast<float>(tile->detailAt.x), 1e-3F);
        }
      }
      const auto more =
          std::ranges::find_if(scene.pieces, [](const NestPiece& piece) { return piece.kind == NestPieceKind::More; });
      TEST_CHECK(more != scene.pieces.end() || depth == 2);
      if (more == scene.pieces.end()) {
        continue;
      }
      const auto& moreLabel = std::get<NestMoreLabel>(more->label);
      const auto* count = labelOf(out, more->node, moreLabel.text);
      TEST_CHECK(count != nullptr);
      if (count == nullptr) {
        continue;
      }
      TEST_CHECK(count->fit == LabelFit::Whole && count->shortText == moreLabel.shortText);
      TEST_CHECK(count->weight == LabelWeight::Semibold);
      TEST_CHECK(count->trail.has_value() == !moreLabel.size.empty());
      if (count->trail) {
        TEST_CHECK(count->trail->place == TrailPlace::Below && count->trail->text == moreLabel.size);
        TEST_NEAR(count->trail->dy, 18.0F, 1e-4F);
        TEST_NEAR(count->trail->dx, 0.0F, 1e-4F);
        TEST_NEAR(count->trail->width, count->width, 1e-4F);
      }
    }
    std::println("  {} measures under names, {} beside them", under, beside);
    TEST_CHECK(under >= 2 && beside >= 2);
  }

  // The card's name is semibold and its facts whole.
  void peekFactsAreWhole() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const auto peek = buildNestPeek(fx.tree, kSettings, scene, nestFocus(fx.tree, scene, heavy).value(), kPalette);
    TEST_CHECK(peek.has_value());
    const auto out = nestPeekShapes(fx.tree, *peek, {}, kPalette);
    const auto* name = labelOf(out, peek->node, peek->name);
    const auto* facts = labelOf(out, peek->node, peek->facts);
    TEST_CHECK(name != nullptr && name->weight == LabelWeight::Semibold && name->fit == LabelFit::Shorten);
    TEST_CHECK(facts != nullptr && facts->fit == LabelFit::Whole && facts->weight == LabelWeight::Regular);
  }

  // The peek card casts a shadow: one soft layer under it that takes no hits, dropped below the card and fading
  // far past its edges.
  void thePeekCardCastsAShadow() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const auto peek = buildNestPeek(fx.tree, kSettings, scene, nestFocus(fx.tree, scene, heavy).value(), kPalette);
    TEST_CHECK(peek.has_value());
    const auto out = nestPeekShapes(fx.tree, *peek, {}, kPalette);
    const auto card = std::ranges::find_if(out.shapes, [](const Shape& shape) {
      return sceneRole(shape.id.role) == SceneRole::Head;
    });
    TEST_CHECK(card == out.shapes.begin() + 1);
    const auto& layer = out.shapes.front();
    TEST_CHECK(!layer.hittable && layer.kind == ShapeKind::RoundedRect && layer.fill.a > 0 && layer.fill.a < 255);
    TEST_CHECK(sameRgb(layer.fill, kPalette.shadow) && !sameRgb(layer.fill, kPalette.surface));
    TEST_CHECK(layer.rect.y > card->rect.y && layer.rect.width >= card->rect.width);
    TEST_CHECK(layer.softness >= 20);
  }
} // namespace

int main() {
  thePathOutlineIsSmooth();
  theLinkedTileGlows();
  tileNamesCarryTheirIcon();
  measuresAndCountsAreWholeTrails();
  peekFactsAreWhole();
  thePeekCardCastsAShadow();
  std::println("workbench nest shapes checks passed");
  return 0;
}
