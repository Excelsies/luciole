#include "nest_pixel_stage.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "workbench/nest_scene.h"
#include "workbench/scene_shapes.h"
#include "workbench/treemap.h"
#include "workbench_fixture.h"
#include "workbench_scene_fixture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

// The Nest drawn offscreen with real text: its tile texture, the smooth outline of the open path, the peek card,
// the linked tile's glow, the "+N more" stop and the note of an empty folder.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::test::nest_pixels;
  using ::test::GlPixels;
  using ::test::SceneCapture;

  // The share of a tile's pixels, 3 px in from its edge, that differ from its fill: the texture's patches.
  double patterned(const GlPixels& pixels, const ShapeRect& tile, Rgba8 fill, float scale) {
    const Rgb fillRgb{fill.r, fill.g, fill.b};
    int inside = 0;
    int differing = 0;
    for (float y = tile.y + 4; y < tile.y + tile.height - 4; y += 1) {
      for (float x = tile.x + 4; x < tile.x + tile.width - 4; x += 1) {
        ++inside;
        differing += distance(at(pixels, x * scale, y * scale), fillRgb) > 6 ? 1 : 0;
      }
    }
    return inside == 0 ? 0 : static_cast<double>(differing) / inside;
  }

  // The outer edge of the open path's outline around one corner arc of the parent's ring: every sample on it shows
  // the outline color, at least halfway from the ring's fill toward it.
  void checkTheOutlineArc(const GlPixels& pixels, const Tree& tree, float scale) {
    const auto scene = buildNestScene(tree, {}, kWidth, kHeight);
    const NestRing* ring = nullptr;
    const NestPiece* path = nullptr;
    for (const auto& each : scene.rings) {
      for (std::size_t i = each.firstPiece; i < each.firstPiece + each.pieceCount; ++i) {
        if (each.level == 1 && scene.pieces[i].onPath) {
          ring = &each;
          path = &scene.pieces[i];
        }
      }
    }
    TEST_CHECK(ring != nullptr && path != nullptr);
    const auto& band = ring->band;
    std::optional<std::size_t> corner;
    for (const std::size_t i : {1U, 3U, 5U, 7U}) {
      if (band.starts[i] >= path->s0 && band.starts[i] + band.lengths[i] <= path->s1) {
        corner = i;
      }
    }
    TEST_CHECK(corner.has_value());
    const ColorPalette palette;
    const auto fill = of(palette.surfaceContainerHighest);
    const auto line = of(palette.primary);
    const double halfway = distance(fill, line) / 2;
    int samples = 0;
    for (int step = 1; step < 30; ++step) {
      const double s = band.starts[*corner] + band.lengths[*corner] * step / 30.0;
      const auto p = pointAt(band, s);
      const double edge = ring->thickness / 2;
      const auto x = (p.x + p.normalX * edge) * scale;
      const auto y = (p.y + p.normalY * edge) * scale;
      TEST_CHECK(distance(at(pixels, x, y), line) < halfway);
      ++samples;
    }
    TEST_CHECK(samples > 20);
  }

  void textureAndOutline(SceneCapture& capture) {
    auto fx = luciole::test::sceneFixture();
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const auto tree = fx.tree;
    Stage stage(std::move(fx.tree));
    for (const float scale : {1.0F, 2.0F}) {
      const auto pixels = stage.settle(capture, scale);
      keep(pixels, std::format("nest-texture-{}x.png", scale));
      const auto* tile = pieceShape(stage.shapes(), heavy);
      TEST_CHECK(tile != nullptr);
      const double share = patterned(pixels, tile->rect, tile->fill, scale);
      std::println("{}x: {:.2f} of the heaviest folder tile shows its texture", scale, share);
      TEST_CHECK(share > 0.3);
      checkTheOutlineArc(pixels, tree, scale);
    }
  }

  void peekCard(SceneCapture& capture) {
    auto fx = luciole::test::sceneFixture();
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    Stage stage(std::move(fx.tree));
    stage.settle(capture, 1);
    stage.view->setCursor(heavy);
    stage.key(XKB_KEY_space, ' ');
    stage.settle(capture, 1);
    stage.key(XKB_KEY_Right);
    const auto pixels = stage.settle(capture, 1);
    keep(pixels, "nest-peek-1x.png");
    const auto* card = pieceShape(stage.shapes(), heavy, SceneRole::Head);
    TEST_CHECK(card != nullptr);
    // The card's body is its own fill, not the tiles under it.
    const ColorPalette palette;
    const Rgb low = palette.surfaceContainerLow;
    const auto margin = at(pixels, card->rect.x + card->rect.width / 2, card->rect.y + card->rect.height - 3);
    TEST_CHECK(distance(margin, low) < 6);
  }

  void linkedGlow(SceneCapture& capture) {
    auto fx = luciole::test::sceneFixture();
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const NodeId second = fx.tree.children[fx.tree.nodes[fx.work].firstChild + 2];
    Stage stage(std::move(fx.tree));
    const auto plain = stage.settle(capture, 1);
    stage.view->setPointed(second);
    const auto pixels = stage.settle(capture, 1);
    keep(pixels, "nest-linked-1x.png");
    const auto* tile = pieceShape(stage.shapes(), second);
    TEST_CHECK(tile != nullptr && tile->id.node != heavy);
    // 4 px outside the tile's edge, the glow pulls every side toward the tertiary color.
    const ColorPalette palette;
    const auto& r = tile->rect;
    for (const auto& [x, y] : std::array<std::array<float, 2>, 4>{{
             {r.x + r.width / 2, r.y - 4},
             {r.x + r.width / 2, r.y + r.height + 4},
             {r.x - 4, r.y + r.height / 2},
             {r.x + r.width + 4, r.y + r.height / 2},
         }}) {
      TEST_CHECK(distance(at(pixels, x, y), palette.tertiary) < distance(at(plain, x, y), palette.tertiary) - 8);
    }
  }

  void moreStop(SceneCapture& capture) {
    Stage stage(luciole::test::manyFilesTree());
    stage.settle(capture, 1);
    stage.key(XKB_KEY_End);
    const auto pixels = stage.settle(capture, 1);
    keep(pixels, "nest-more-1x.png");
    const auto* more = pieceShape(stage.shapes(), 0, SceneRole::More);
    TEST_CHECK(more != nullptr);
    const ColorPalette palette;
    const auto& r = more->rect;
    TEST_CHECK(distance(at(pixels, r.x + r.width / 2, r.y - 1), palette.primary) < 40);
  }

  void emptyFolderNote(SceneCapture& capture) {
    auto fx = luciole::test::sceneFixture();
    // p0a, the first entry of work's first folder, holds nothing listed.
    auto tree = fx.tree;
    const NodeId first = tree.children[tree.nodes[fx.work].firstChild];
    tree.root = tree.children[tree.nodes[first].firstChild];
    tree.ancestors = parentChain(tree, tree.root);
    Stage stage(std::move(tree));
    const auto pixels = stage.settle(capture, 1);
    keep(pixels, "nest-empty-1x.png");
    const auto scene = buildNestScene(*stage.inputs.snapshot.tree, {}, kWidth, kHeight);
    TEST_CHECK(scene.empty && !scene.rings.empty());
    // Text in the middle of the core: some pixels there are far from the surface.
    const ColorPalette palette;
    int inked = 0;
    const double cx = scene.core.x + scene.core.w / 2;
    const double cy = scene.core.y + scene.core.h / 2;
    for (double x = cx - 60; x < cx + 60; x += 1) {
      for (double y = cy - 8; y < cy + 8; y += 1) {
        inked += distance(at(pixels, x, y), palette.surface) > 80 ? 1 : 0;
      }
    }
    TEST_CHECK(inked > 40);
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  textureAndOutline(*capture);
  peekCard(*capture);
  linkedGlow(*capture);
  moreStop(*capture);
  emptyFolderNote(*capture);
  std::println("nest part pixel checks passed");
  return 0;
}
