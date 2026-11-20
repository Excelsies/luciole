#include "test_check.h"
#include "workbench/nest_texture.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"
#include "workbench_nest_fixture.h"
#include "workbench_scene_fixture.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <format>
#include <print>
#include <string>
#include <utility>
#include <vector>

// The texture of Nest's folder tiles: sources sorted once per folder, patches laid out per scene within a budget,
// drawn under any marks without data work.
namespace {
  using namespace luciole::workbench;
  using luciole::test::drawn;
  using luciole::test::sameRect;
  using luciole::test::sameRgb;
  using luciole::test::nest::checkRect;
  using luciole::test::nest::kEps;

  const LayoutSettings kSettings{};
  const ColorPalette kPalette{};

  // top holds "here" and "other"; "here" holds `folders` folders of `files` files each, heaviest first.
  Tree wideTree(std::size_t folders, std::size_t files) {
    Tree tree;
    const auto add = [&tree](std::string name, NodeId parent, bool folder, double bytes, double items) {
      Node node;
      node.name = std::move(name);
      node.parent = parent;
      node.folder = folder;
      node.bytes = bytes;
      node.items = items;
      node.category = static_cast<std::uint8_t>(fileCategory(node.name, folder));
      tree.nodes.push_back(std::move(node));
      return static_cast<NodeId>(tree.nodes.size() - 1);
    };
    const auto list = [&tree](NodeId parent, NodeId first, std::size_t count) {
      tree.nodes[parent].firstChild = static_cast<std::uint32_t>(tree.children.size());
      tree.nodes[parent].childCount = static_cast<std::uint32_t>(count);
      for (std::size_t i = 0; i < count; ++i) {
        tree.children.push_back(first + static_cast<NodeId>(i));
      }
    };
    const auto top = add("top", kNoNode, true, 1e12, 2);
    const auto here = add("here", top, true, 1e11, static_cast<double>(folders));
    add("other", top, true, 1e9, 0);
    list(top, here, 2);
    const auto firstFolder = static_cast<NodeId>(tree.nodes.size());
    for (std::size_t f = 0; f < folders; ++f) {
      add(std::format("d{}", f), here, true, 1e9 * static_cast<double>(folders - f), static_cast<double>(files));
    }
    list(here, firstFolder, folders);
    for (std::size_t f = 0; f < folders; ++f) {
      const auto firstFile = static_cast<NodeId>(tree.nodes.size());
      for (std::size_t k = 0; k < files; ++k) {
        add(std::format("f{}_{}.txt", f, k), firstFolder + static_cast<NodeId>(f), false,
            1000.0 + static_cast<double>(k % 13) * 100, 0);
      }
      list(firstFolder + static_cast<NodeId>(f), firstFile, files);
    }
    tree.root = here;
    tree.ancestors = parentChain(tree, here);
    return tree;
  }

  NestTextureSources allSources(const Tree& tree, const NestScene& scene) {
    NestTextureSources sources;
    for (const auto folder : missingNestSources(scene, sources)) {
      sources.emplace(folder, nestTextureSource(tree, kSettings, folder, kPalette));
    }
    return sources;
  }

  std::size_t patchCount(const std::vector<std::vector<NestPatch>>& texture) {
    std::size_t count = 0;
    for (const auto& patches : texture) {
      count += patches.size();
    }
    return count;
  }

  void sourcesHoldTheHeaviestEntries() {
    auto fx = luciole::test::nest::fixture();
    fx.at(11);
    const auto l11 = fx.find("l11");
    const auto source = nestTextureSource(fx.tree, kSettings, l11, kPalette);
    TEST_CHECK(source.weights.size() == kNestTextureEntries && source.colors.size() == kNestTextureEntries);
    const auto list = entries(fx.tree, l11, kSettings);
    for (std::size_t i = 0; i < source.weights.size(); ++i) {
      TEST_NEAR(source.weights[i], list.weights[i], kEps);
    }
    TEST_CHECK(source.colors[0] == nodeColor(fx.tree, list.ids[0], kSettings, kPalette));
    TEST_CHECK(nestTextureSource(fx.tree, kSettings, fx.find("f0.dat"), kPalette).weights.empty());
    TEST_CHECK(nestTextureSource(fx.tree, kSettings, kNoNode, kPalette).weights.empty());
  }

  void onlyMissingFoldersAreSorted() {
    auto fx = luciole::test::nest::fixture();
    fx.at(2);
    const auto scene = buildNestScene(fx.tree, kSettings, 340, 380);
    NestTextureSources sources;
    const auto missing = missingNestSources(scene, sources);
    std::vector<NodeId> folders;
    for (const auto& piece : scene.pieces) {
      if (piece.kind == NestPieceKind::Tile && piece.folder) {
        folders.push_back(piece.node);
      }
    }
    TEST_CHECK(!folders.empty() && missing == folders);
    sources.emplace(folders.front(), NestTextureSource{});
    TEST_CHECK(missingNestSources(scene, sources).size() == folders.size() - 1);
    TEST_CHECK(missingNestSources(scene, allSources(fx.tree, scene)).empty());
  }

  void patchesKeepTheirGeometry() {
    auto fx = luciole::test::nest::fixture();
    fx.at(2);
    const auto scene = buildNestScene(fx.tree, kSettings, 340, 380);
    const auto texture = nestTexturePatches(scene, allSources(fx.tree, scene));
    TEST_CHECK(texture.size() == scene.pieces.size());
    std::size_t l2 = scene.pieces.size();
    std::size_t file = scene.pieces.size();
    for (std::size_t i = 0; i < scene.pieces.size(); ++i) {
      l2 = scene.pieces[i].node == fx.find("l2") ? i : l2;
      file = scene.pieces[i].node == fx.find("s2_4") ? i : file;
    }
    TEST_CHECK(l2 < texture.size() && texture[l2].size() == 7);
    checkRect(texture[l2][0].rect, 41, 41, 73.525813, 83.731792);
    checkRect(texture[l2][1].rect, 41, 124.731792, 73.525813, 46.703937);
    TEST_CHECK(file < texture.size() && texture[file].empty());
    // A source for a scene of another size draws inside the tiles of that size.
    const auto wide = buildNestScene(fx.tree, kSettings, 1400, 900);
    const auto widened = nestTexturePatches(wide, allSources(fx.tree, scene));
    TEST_CHECK(widened.size() == wide.pieces.size() && patchCount(widened) > 0);
  }

  void smallAndNarrowPatchesAreLeftOut() {
    NestScene scene;
    NestPiece tile;
    tile.kind = NestPieceKind::Tile;
    tile.node = 7;
    tile.folder = true;
    tile.rect = {.x = 0, .y = 0, .w = 60, .h = 30};
    scene.pieces.push_back(tile);
    NestTextureSource source;
    source.weights.assign(kNestTextureEntries, 1.0);
    source.weights[0] = 400;
    source.colors.assign(kNestTextureEntries, NodeColor{});
    NestTextureSources sources;
    sources.emplace(7, source);
    const auto texture = nestTexturePatches(scene, sources);
    TEST_CHECK(texture.size() == 1 && !texture[0].empty() && texture[0].size() < kNestTextureEntries);
    for (const auto& patch : texture[0]) {
      TEST_CHECK(patch.rect.w >= kNestPatchLeast && patch.rect.h >= kNestPatchLeast);
    }
    scene.pieces[0].rect.w = 16;
    TEST_CHECK(nestTexturePatches(scene, sources)[0].empty());
    // A source whose colors do not match its weights draws nothing.
    sources[7].colors.pop_back();
    scene.pieces[0].rect.w = 60;
    TEST_CHECK(nestTexturePatches(scene, sources)[0].empty());
  }

  void aWideSceneStaysWithinTheBudget() {
    const auto tree = wideTree(400, 30);
    const auto scene = buildNestScene(tree, kSettings, 2560, 1400, kPalette);
    const auto texture = nestTexturePatches(scene, allSources(tree, scene));
    const std::size_t count = patchCount(texture);
    std::println("  {} pieces, {} patches", scene.pieces.size(), count);
    TEST_CHECK(count <= kNestTexturePatches && count > kNestTexturePatches - kNestTextureEntries);
    // The heaviest tiles keep their texture; the last ones go without.
    TEST_CHECK(!texture.front().empty());
    std::size_t lastTile = 0;
    for (std::size_t i = 0; i < scene.pieces.size(); ++i) {
      lastTile = scene.pieces[i].kind == NestPieceKind::Tile ? i : lastTile;
    }
    TEST_CHECK(texture[lastTile].empty());
  }

  void testTexturePatchesDraw() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = buildNestScene(fx.tree, kSettings, 900, 600, kPalette);
    const auto texture = nestTexturePatches(scene, allSources(fx.tree, scene));
    TEST_CHECK(texture.size() == scene.pieces.size());
    const auto out = nestShapes(fx.tree, scene, texture, {}, kPalette);
    std::size_t folders = 0;
    for (std::size_t p = 0; p < scene.pieces.size(); ++p) {
      const auto& piece = scene.pieces[p];
      if (piece.kind != NestPieceKind::Tile) {
        TEST_CHECK(texture[p].empty());
        continue;
      }
      const auto& patches = texture[p];
      const auto shapes = drawn(out, {piece.node, SceneRole::Detail});
      TEST_CHECK(shapes.size() == patches.size());
      for (std::size_t i = 0; i < std::min(shapes.size(), patches.size()); ++i) {
        const auto& rect = patches[i].rect;
        TEST_CHECK(sameRect(shapes[i]->rect, rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2));
        TEST_CHECK(sameRgb(shapes[i]->fill, patches[i].color.accent) && shapes[i]->fill.a < 64);
      }
      folders += patches.empty() ? 0 : 1;
    }
    TEST_CHECK(folders > 2);
    // Without a texture, and with one for another scene, the tiles are bare.
    const auto bare = nestShapes(fx.tree, scene, {}, kPalette);
    TEST_CHECK(bare.shapes.size() < out.shapes.size());
    const std::vector<std::vector<NestPatch>> stale(1);
    TEST_CHECK(nestShapes(fx.tree, scene, stale, {}, kPalette).shapes.size() == bare.shapes.size());
  }

  // Drawing the textured scene again for each new hover costs no more than drawing its shapes.
  void testMarksRedrawQuickly() {
    const auto tree = wideTree(40, 1000);
    const auto scene = buildNestScene(tree, kSettings, 1400, 900, kPalette);
    const auto texture = nestTexturePatches(scene, allSources(tree, scene));
    std::size_t textured = 0;
    for (const auto& patches : texture) {
      textured += patches.empty() ? 0 : 1;
    }
    TEST_CHECK(textured > 20);
    constexpr int kRuns = 10;
    std::size_t shapes = 0;
    // Processor time, so other tests running in parallel do not count.
    const std::clock_t start = std::clock();
    for (int i = 0; i < kRuns; ++i) {
      const auto node = tree.children[tree.nodes[tree.root].firstChild + static_cast<std::size_t>(i)];
      const ShapeMarks marks{.hovered = ShapeId{.node = node, .role = shapeRole(SceneRole::Piece)}};
      shapes += nestShapes(tree, scene, texture, marks, kPalette).shapes.size();
    }
    const double each = 1000.0 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC / kRuns;
    std::println("  {} textured tiles, {} shapes: {:.2f} ms a redraw", textured, shapes / kRuns, each);
    TEST_CHECK(shapes > kRuns * textured);
    TEST_CHECK(each < 10); // a debug build takes about 0.3 ms; sorting each folder again takes over 60
  }
} // namespace

int main() {
  sourcesHoldTheHeaviestEntries();
  onlyMissingFoldersAreSorted();
  patchesKeepTheirGeometry();
  smallAndNarrowPatchesAreLeftOut();
  aWideSceneStaysWithinTheBudget();
  testTexturePatchesDraw();
  testMarksRedrawQuickly();
  return 0;
}
