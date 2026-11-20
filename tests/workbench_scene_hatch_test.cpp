#include "test_check.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"
#include "workbench_scene_fixture.h"

#include <cstddef>

// A partial piece of the flame graph or the ladder keeps its own stripes under an outline: the hatch color stays
// the quiet stripe tone while the outline takes the pin, hover or link color.
namespace {
  using namespace luciole::workbench;
  using luciole::test::findSceneShape;
  using luciole::test::hitTarget;
  using luciole::test::sameRgb;

  const LayoutSettings kSettings{};
  const ColorPalette kPalette{};

  FlameScene flame(const Tree& tree) {
    LabelFont font;
    font.ascii.fill(7);
    font.other = 7;
    return flameScene(tree, kSettings, 1000, 266, {.firstAhead = font, .ahead = font, .ancestor = font, .value = font});
  }

  bool quietStripes(const Shape& shape) {
    return shape.pattern == ShapePattern::Hatched
        && shape.hatch.has_value()
        && sameRgb(*shape.hatch, kPalette.onSurfaceVariant)
        && shape.hatch->a < 255;
  }

  void flameStripesUnderAPin() {
    auto fx = luciole::test::sceneFixture();
    for (auto& node : fx.tree.nodes) {
      node.partial = node.folder;
    }
    const auto scene = flame(fx.tree);
    const auto out = flameShapes(fx.tree, kSettings, scene, {}, kPalette);
    std::size_t pinned = 0;
    for (const auto& row : scene.rows) {
      for (const auto& item : row.items) {
        if (!item.pinned || row.row.h < 10) {
          continue;
        }
        const auto* shape = findSceneShape(out, {item.node, SceneRole::Piece});
        TEST_CHECK(shape != nullptr && shape->strokeWidth > 0 && sameRgb(shape->stroke, kPalette.primary));
        TEST_CHECK(quietStripes(*shape));
        ++pinned;
      }
    }
    TEST_CHECK(pinned > 0);
    NodeId hovered = kNoNode;
    for (const auto& row : scene.rows) {
      for (const auto& item : row.items) {
        const bool piece = row.row.kind == FlameRowKind::Ahead && !item.more && !item.bar;
        hovered = hovered == kNoNode && piece && fx.tree.nodes[item.node].partial ? item.node : hovered;
      }
    }
    TEST_CHECK(hovered != kNoNode);
    ShapeMarks marks;
    marks.hovered = ShapeId{.node = hovered, .role = shapeRole(SceneRole::Piece)};
    const auto hot = flameShapes(fx.tree, kSettings, scene, marks, kPalette);
    const auto* shape = findSceneShape(hot, {hovered, SceneRole::Piece});
    TEST_CHECK(shape != nullptr && sameRgb(shape->stroke, kPalette.onSurface) && quietStripes(*shape));
  }

  void ladderStripesUnderALink() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = ladderScene(fx.tree, kSettings, 900, 200);
    std::size_t checked = 0;
    for (const auto& row : scene.rows) {
      for (const auto& piece : row.pieces) {
        fx.tree.nodes[piece.node].partial = true;
      }
    }
    for (const auto& row : scene.rows) {
      for (const auto& piece : row.pieces) {
        const auto out = ladderShapes(fx.tree, kSettings, scene, {.hovered = {}, .linked = piece.node}, kPalette);
        const auto* shape = hitTarget(out, {piece.node, SceneRole::Piece});
        TEST_CHECK(shape != nullptr && shape->strokeWidth > 0 && shape->pattern == ShapePattern::Hatched);
        TEST_CHECK(shape->hatch.has_value() && *shape->hatch != shape->stroke && shape->hatch->a < 255);
        ++checked;
      }
    }
    TEST_CHECK(checked > 2);
  }

  // A folder whose totals are a lower bound is striped like one still measuring.
  void lowerBoundStripesLikePartial() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = ladderScene(fx.tree, kSettings, 900, 200);
    for (const bool bound : {false, true}) {
      for (const auto& row : scene.rows) {
        for (const auto& piece : row.pieces) {
          fx.tree.nodes[piece.node].lowerBound = bound;
        }
      }
      const auto out = ladderShapes(fx.tree, kSettings, scene, {}, kPalette);
      std::size_t checked = 0;
      for (const auto& row : scene.rows) {
        for (const auto& piece : row.pieces) {
          const auto* shape = hitTarget(out, {piece.node, SceneRole::Piece});
          TEST_CHECK(shape != nullptr && (shape->pattern == ShapePattern::Hatched) == bound);
          ++checked;
        }
      }
      TEST_CHECK(checked > 2);
    }
  }
} // namespace

int main() {
  flameStripesUnderAPin();
  ladderStripesUnderALink();
  lowerBoundStripesLikePartial();
  return 0;
}
