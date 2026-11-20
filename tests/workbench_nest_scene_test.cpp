#include "test_check.h"
#include "workbench/nest_scene.h"
#include "workbench_fixture.h"
#include "workbench_nest_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::nest;
  using luciole::test::WorkbenchRow;

  void compactHostsUseTheCompactProfile() {
    TEST_CHECK(nestProfile(340, 380).compact);
    TEST_CHECK(nestProfile(240, 220).compact);
    TEST_CHECK(!nestProfile(1100, 571).compact);
    TEST_CHECK(!nestProfile(1400, 900).compact);
    const auto normal = nestProfile(1400, 900);
    TEST_CHECK(
        (std::vector<double>(normal.ringThickness.begin(), normal.ringThickness.end())
         == std::vector<double>{40, 22, 12, 7, 4})
    );
    TEST_CHECK(normal.lineThickness == 4 && normal.gap == 3 && normal.coreGap == 4 && normal.pad == 8);
    TEST_CHECK(normal.minTile.w == 64 && normal.minTile.h == 28 && normal.minCore.w == 180 && normal.minCore.h == 110);
    const auto compact = nestProfile(340, 380);
    TEST_CHECK(
        (std::vector<double>(compact.ringThickness.begin(), compact.ringThickness.end())
         == std::vector<double>{16, 12, 12})
    );
    TEST_CHECK(compact.lineThickness == 3 && compact.gap == 2 && compact.coreGap == 3 && compact.pad == 4);
    TEST_CHECK(compact.peekMin.w == 220 && compact.peekMin.h == 150 && compact.peekHead == 24);
  }

  void compactScenesFitTheirHost() {
    auto fx = fixture();
    const std::vector<std::pair<double, double>> sizes{{340, 380}, {300, 300}, {260, 240}, {240, 220}};
    for (const auto& [width, height] : sizes) {
      for (const std::size_t depth : {1U, 3U, 12U}) {
        fx.at(depth);
        const auto scene = buildNestScene(fx.tree, kSettings, width, height);
        const auto& profile = scene.profile;
        const auto tiles = piecesOf(scene, NestPieceKind::Tile);
        TEST_CHECK(tiles.size() >= (depth == 12 && width <= 260 ? 1U : 2U));
        auto boxes = tiles;
        if (const auto* more = morePiece(scene)) {
          boxes.push_back(more);
        }
        for (const auto* tile : boxes) {
          TEST_CHECK(inside(tile->rect, width, height, profile.pad));
          TEST_CHECK(tile->rect.w >= profile.minTile.w - 2 - kEps && tile->rect.h >= profile.minTile.h - 2 - kEps);
        }
        std::vector<const NestRing*> near;
        for (const auto& ring : scene.rings) {
          if (!ring.line) {
            near.push_back(&ring);
          }
        }
        std::ranges::sort(near, {}, &NestRing::level);
        for (std::size_t i = 0; i < near.size() && i < 3; ++i) {
          TEST_CHECK(near[i]->thickness >= 12);
          TEST_CHECK(near[i]->band.rect.w >= 12 && near[i]->band.rect.h >= 12);
          TEST_CHECK(inside(near[i]->band.rect, width, height, profile.pad));
        }
        TEST_CHECK(scene.core.w >= profile.minCore.w || scene.rings.empty());
      }
    }
  }

  void compactDeepSceneShowsFourTiles() {
    auto fx = fixture();
    fx.at(12);
    const auto scene = buildNestScene(fx.tree, kSettings, 340, 380);
    TEST_CHECK(piecesOf(scene, NestPieceKind::Tile).size() >= 4);
  }

  void profileSwitchesAtTheEdge() {
    struct Edge {
      double width;
      double height;
      bool compact;
    };
    const std::vector<Edge> edges{
        {639, 419, true},  {639, 420, true},  {640, 419, true}, {640, 420, false},
        {641, 421, false}, {1000, 419, true}, {500, 900, true},
    };
    for (const auto& edge : edges) {
      TEST_CHECK(nestProfile(edge.width, edge.height).compact == edge.compact);
    }
    auto fx = fixture();
    fx.at(12);
    const std::vector<std::pair<double, double>> sizes{{639, 420}, {640, 419}, {640, 420}};
    for (const auto& [width, height] : sizes) {
      const auto scene = buildNestScene(fx.tree, kSettings, width, height);
      auto boxes = piecesOf(scene, NestPieceKind::Tile);
      TEST_CHECK(!boxes.empty());
      if (const auto* more = morePiece(scene)) {
        boxes.push_back(more);
      }
      for (const auto* tile : boxes) {
        TEST_CHECK(inside(tile->rect, width, height, scene.profile.pad));
      }
    }
  }

  void normalSizesKeepTheFullFrame() {
    auto fx = fixture();
    fx.at(12);
    const auto scene = buildNestScene(fx.tree, kSettings, 1400, 571);
    const std::vector<std::pair<std::size_t, double>> expected{{6, 4}, {5, 4}, {4, 7}, {3, 12}, {2, 22}, {1, 40}};
    TEST_CHECK(scene.rings.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
      TEST_CHECK(scene.rings[i].level == expected[i].first && scene.rings[i].thickness == expected[i].second);
    }
    TEST_CHECK(!scene.profile.compact);
    checkRect(scene.core, 116, 116, 1168, 339);
    TEST_CHECK(piecesOf(scene, NestPieceKind::Tile).size() == 96);
    TEST_CHECK(morePiece(scene) != nullptr && morePiece(scene)->nodes.size() == 304);
  }

  void peekNeedsARoomyCore() {
    auto fx = fixture();
    fx.at(12);
    const auto small = buildNestScene(fx.tree, kSettings, 240, 220);
    TEST_CHECK(!buildNestPeek(fx.tree, kSettings, small, 0).has_value());
    fx.at(11);
    const auto smallFolder = buildNestScene(fx.tree, kSettings, 240, 220);
    const auto folderTile = pieceOfNode(smallFolder, fx.find("l11"));
    TEST_CHECK(folderTile.has_value() && nestPeekAllowed(fx.tree, smallFolder, *folderTile));
    TEST_CHECK(!buildNestPeek(fx.tree, kSettings, smallFolder, *folderTile).has_value());
    fx.at(2);
    const auto scene = buildNestScene(fx.tree, kSettings, 340, 380);
    const auto& core = scene.core;
    const auto tile = pieceOfNode(scene, fx.find("l2"));
    TEST_CHECK(tile.has_value());
    const auto peek = buildNestPeek(fx.tree, kSettings, scene, *tile);
    TEST_CHECK(peek.has_value());
    TEST_CHECK(peek->card.x >= core.x - kEps && peek->card.y >= core.y - kEps);
    TEST_CHECK(peek->card.x + peek->card.w <= core.x + core.w + kEps);
    TEST_CHECK(peek->card.y + peek->card.h <= core.y + core.h + kEps);
    std::size_t tiles = 0;
    for (const auto& piece : peek->pieces) {
      tiles += piece.kind == NestPieceKind::PeekTile ? 1U : 0U;
    }
    TEST_CHECK(tiles >= 2);
  }

  void peekMatchesKnownGeometry() {
    auto fx = fixture();
    fx.at(2);
    const auto scene = buildNestScene(fx.tree, kSettings, 340, 380);
    const auto tile = pieceOfNode(scene, fx.find("l2"));
    TEST_CHECK(tile.has_value());
    TEST_CHECK(nestPeekAllowed(fx.tree, scene, *tile));
    const auto peek = buildNestPeek(fx.tree, kSettings, scene, *tile);
    TEST_CHECK(peek.has_value());
    checkRect(peek->card, 37, 37, 220, 150);
    checkRect(peek->head, 37, 37, 220, 24);
    TEST_CHECK(peek->count == 7 && peek->pieces.size() == 8);
    TEST_CHECK(peek->pieces[0].kind == NestPieceKind::PeekHead && peek->pieces[0].node == fx.find("l2"));
    TEST_CHECK(peek->pieces[1].node == fx.find("l3") && peek->pieces[2].node == fx.find("s3_5"));
    checkRect(peek->pieces[1].rect, 44, 62, 71.377269, 66.695204);
    checkRect(peek->pieces[2].rect, 44, 130.695204, 71.377269, 49.304796);
    TEST_CHECK(std::ranges::none_of(peek->pieces, [](const NestPiece& piece) {
      return piece.kind == NestPieceKind::PeekMore;
    }));
    TEST_CHECK(peek->name == "l2" && peek->facts == "7 items  ·  87.9\u00A0KiB");
    TEST_NEAR(peek->nameAt.x, 49.0, kEps);
    TEST_NEAR(peek->nameAt.y, 50.0, kEps);
    TEST_NEAR(peek->nameWidth, 70.0, kEps);
    TEST_NEAR(peek->factsAt.x, 245.0, kEps);
    TEST_CHECK(nestPeekHit(*peek, 50, 70) == std::optional<std::size_t>{1});
    TEST_CHECK(nestPeekHit(*peek, 40, 40) == std::optional<std::size_t>{0});
    TEST_CHECK(nestPeekHit(*peek, 255, 185) == std::optional<std::size_t>{0});
    TEST_CHECK(!nestPeekHit(*peek, 30, 30).has_value());
    TEST_CHECK(!nestPeekHit(*peek, kNan, 40).has_value());
    // A folder with items but no visible entries starts a dwell and opens nothing.
    const auto empty = pieceOfNode(scene, fx.find("s2_5"));
    TEST_CHECK(empty.has_value() && nestPeekAllowed(fx.tree, scene, *empty));
    TEST_CHECK(!buildNestPeek(fx.tree, kSettings, scene, *empty).has_value());
    const auto file = pieceOfNode(scene, fx.find("s2_4"));
    TEST_CHECK(file.has_value() && !nestPeekAllowed(fx.tree, scene, *file));
    TEST_CHECK(!nestPeekAllowed(fx.tree, scene, scene.pieces.size()));
    TEST_CHECK(!buildNestPeek(fx.tree, kSettings, scene, scene.pieces.size()).has_value());
    const auto& ring = scene.rings.back();
    TEST_CHECK(!nestPeekAllowed(fx.tree, scene, ring.firstPiece));
  }

  void peekGetsItsOwnMoreTile() {
    auto fx = fixture();
    fx.at(11);
    const auto scene = buildNestScene(fx.tree, kSettings, 1400, 900);
    const auto tile = pieceOfNode(scene, fx.find("l11"));
    TEST_CHECK(tile.has_value());
    const auto peek = buildNestPeek(fx.tree, kSettings, scene, *tile);
    TEST_CHECK(peek.has_value() && peek->count == 400);
    std::size_t tiles = 0;
    const NestPiece* more = nullptr;
    for (const auto& piece : peek->pieces) {
      if (piece.kind == NestPieceKind::PeekTile) {
        ++tiles;
        TEST_CHECK(piece.rect.w >= scene.profile.minPeekTile.w - 2 - kEps);
        TEST_CHECK(piece.rect.h >= scene.profile.minPeekTile.h - 2 - kEps);
      } else if (piece.kind == NestPieceKind::PeekMore) {
        more = &piece;
      }
    }
    TEST_CHECK(tiles >= 1 && tiles <= 11);
    TEST_CHECK(more != nullptr && more->nodes.size() == 400 - tiles && more->node == fx.find("l11"));
    TEST_CHECK(std::holds_alternative<NestMoreLabel>(more->label));
  }

  void focusFindsTilesSegmentsAndNothing() {
    auto fx = fixture();
    const auto folder = fx.at(3);
    const auto scene = buildNestScene(fx.tree, kSettings, 340, 380);
    const auto kid = scene.pieces[0].node;
    TEST_CHECK(nestFocus(fx.tree, scene, kid) == std::optional<std::size_t>{0});
    const auto grandchildren = children(fx.tree, kid);
    TEST_CHECK(!grandchildren.empty());
    TEST_CHECK(nestFocus(fx.tree, scene, grandchildren.front()) == std::optional<std::size_t>{0});
    TEST_CHECK(!nestFocus(fx.tree, scene, folder).has_value());
    const auto parent = nestFocus(fx.tree, scene, fx.tree.ancestors[0]);
    TEST_CHECK(parent.has_value() && scene.pieces[*parent].kind == NestPieceKind::Segment);
    TEST_CHECK(scene.pieces[*parent].node == fx.tree.ancestors[0] && scene.pieces[*parent].onPath);
    TEST_CHECK(!nestFocus(fx.tree, scene, kNoNode).has_value());
    // The top folder has no ring of its own to sit on.
    TEST_CHECK(!nestFocus(fx.tree, scene, 0).has_value());
    // An entry inside "+N more" maps to that tile.
    fx.at(12);
    const auto deep = buildNestScene(fx.tree, kSettings, 340, 380);
    const auto* more = morePiece(deep);
    TEST_CHECK(more != nullptr);
    const auto focus = nestFocus(fx.tree, deep, more->nodes.back());
    TEST_CHECK(focus.has_value() && &deep.pieces[*focus] == more);
  }

  void degenerateHostsGiveDefinedScenes() {
    auto fx = fixture();
    const std::vector<std::pair<double, double>> sizes{
        {0, 0}, {-5, 100}, {100, -5}, {kNan, 300}, {300, kNan}, {kInf, 300}, {1, 1},
        {2, 2}, {10, 10},  {59, 59},  {100, 100},  {239, 219},  {240, 220},
    };
    for (const std::size_t depth : {0U, 3U, 12U}) {
      fx.at(depth);
      for (const auto& [width, height] : sizes) {
        const auto scene = buildNestScene(fx.tree, kSettings, width, height);
        const bool usable = std::isfinite(width) && std::isfinite(height) && width >= 1 && height >= 1;
        if (!usable) {
          TEST_CHECK(scene.pieces.empty() && scene.rings.empty());
          TEST_CHECK(scene.width == 0 && scene.height == 0);
        }
        for (const auto* tile : piecesOf(scene, NestPieceKind::Tile)) {
          TEST_CHECK(tile->rect.w > 0 && tile->rect.h > 0 && inside(tile->rect, width, height, 0));
        }
        const auto hit = nestHit(scene, width / 2, height / 2);
        TEST_CHECK(!hit.has_value() || *hit < scene.pieces.size());
        for (const auto& piece : scene.pieces) {
          TEST_CHECK(finiteRect(piece.rect));
        }
      }
    }
  }

  void sceneMatchesKnownGeometry() {
    auto fx = fixture();
    fx.at(12);
    auto scene = buildNestScene(fx.tree, kSettings, 340, 380);
    TEST_CHECK(scene.profile.compact && scene.folder == fx.levels[11]);
    checkRect(scene.core, 56, 56, 228, 268);
    const std::vector<std::pair<std::size_t, std::size_t>> rings{{6, 9}, {3, 7}, {2, 7}, {1, 7}};
    TEST_CHECK(scene.rings.size() == rings.size());
    for (std::size_t i = 0; i < rings.size(); ++i) {
      TEST_CHECK(scene.rings[i].level == rings[i].first && scene.rings[i].pieceCount == rings[i].second);
    }
    auto tiles = piecesOf(scene, NestPieceKind::Tile);
    TEST_CHECK(tiles.size() == 28 && tiles[0]->node == fx.find("f173.dat"));
    checkRect(tiles[0]->rect, 57, 57, 74, 24.273684211);
    const auto* more = morePiece(scene);
    TEST_CHECK(more != nullptr && more->nodes.size() == 372 && more->node == scene.folder);
    checkRect(more->rect, 196.383897162, 266.392141680, 86.616102838, 56.607858320);
    const auto* moreLabel = std::get_if<NestMoreLabel>(&more->label);
    TEST_CHECK(moreLabel != nullptr && moreLabel->text == "+372 more" && moreLabel->size == "837.0\u00A0KiB");

    scene = buildNestScene(fx.tree, kSettings, 240, 220);
    TEST_CHECK(piecesOf(scene, NestPieceKind::Tile).size() == 3);
    TEST_CHECK(morePiece(scene) != nullptr && morePiece(scene)->nodes.size() == 397);

    scene = buildNestScene(fx.tree, kSettings, 639, 420);
    TEST_CHECK(piecesOf(scene, NestPieceKind::Tile).size() == 78 && morePiece(scene)->nodes.size() == 322);

    fx.at(3);
    scene = buildNestScene(fx.tree, kSettings, 260, 240);
    TEST_CHECK(scene.rings.size() == 3 && piecesOf(scene, NestPieceKind::Tile).size() == 7);
    TEST_CHECK(morePiece(scene) == nullptr);
    // The outermost ring goes first until the core meets its minimum.
    scene = buildNestScene(fx.tree, kSettings, 200, 200);
    TEST_CHECK(scene.rings.size() == 2 && scene.rings[0].level == 2 && scene.rings[1].level == 1);
    checkRect(scene.core, 37, 37, 126, 126);
    scene = buildNestScene(fx.tree, kSettings, 100, 100);
    TEST_CHECK(scene.rings.empty());
    checkRect(scene.core, 4, 4, 92, 92);
    TEST_CHECK(piecesOf(scene, NestPieceKind::Tile).size() == 2 && morePiece(scene)->nodes.size() == 5);
    checkRect(morePiece(scene)->rect, 5, 71.956521739, 90, 23.043478261);
    // "+N more" alone when the height cannot take a tile above the strip.
    scene = buildNestScene(fx.tree, kSettings, 80, 50);
    TEST_CHECK(piecesOf(scene, NestPieceKind::Tile).empty());
    TEST_CHECK(morePiece(scene)->nodes.size() == 7);
    checkRect(morePiece(scene)->rect, 5, 5, 70, 40);
    scene = buildNestScene(fx.tree, kSettings, 130, 60);
    // The heaviest entry above a "+N more" strip.
    TEST_CHECK(piecesOf(scene, NestPieceKind::Tile).size() == 1 && morePiece(scene)->nodes.size() == 6);
    checkRect(morePiece(scene)->rect, 5, 29, 120, 26);
  }

  void focusEndsOnAParentCycle() {
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "top", .folder = true},
        {.parent = 0, .name = "a", .folder = true},
        {.parent = 1, .name = "b", .folder = true},
    };
    auto tree = luciole::test::workbenchTree(rows, 0);
    const auto scene = buildNestScene(tree, kSettings, 800, 600);
    tree.nodes[1].parent = 2;
    TEST_CHECK(!nestFocus(tree, scene, 2).has_value());
  }

  void emptyFolderIsMarked() {
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "top", .folder = true}, {.parent = 0, .name = "none", .folder = true}
    };
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, 1);
    const auto scene = buildNestScene(tree, kSettings, 800, 600);
    TEST_CHECK(scene.empty && piecesOf(scene, NestPieceKind::Tile).empty() && morePiece(scene) == nullptr);
    TEST_CHECK(scene.rings.size() == 1);
    tree.root = kNoNode;
    tree.ancestors = {kNoNode, 0};
    const auto stranger = buildNestScene(tree, kSettings, 800, 600);
    TEST_CHECK(stranger.empty && stranger.rings.empty() && stranger.pieces.empty());
    const auto blank = buildNestScene(Tree{}, kSettings, 800, 600);
    TEST_CHECK(blank.empty && blank.pieces.empty());
    // A folder that holds no items opens no peek.
    tree.root = 0;
    tree.ancestors.clear();
    const auto top = buildNestScene(tree, kSettings, 800, 600);
    TEST_CHECK(top.pieces.size() == 1 && top.pieces[0].kind == NestPieceKind::Tile && top.pieces[0].folder);
    TEST_CHECK(!nestPeekAllowed(tree, top, 0));
  }

  // Eleven ancestors over a folder of three files of 9847, 1793 and 12 bytes, in a 340 by 476 host: the 12 byte
  // file would get a column 17.9 px wide, so the two smaller files share "+2 more" under the largest.
  void threeFilesAllShowBesideTheirHeaviest() {
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "/", .folder = true, .bytes = 20000, .items = 1}};
    for (const std::string_view name :
         {"home", "sample", "Projects", "luciole", "cache", "tmp", "native", "harness", "prove", "fixture",
          "Pictures"}) {
      rows.push_back(
          {.parent = static_cast<int>(rows.size()) - 1, .name = name, .folder = true, .bytes = 20000, .items = 1}
      );
    }
    const int folder = static_cast<int>(rows.size()) - 1;
    rows.push_back({.parent = folder, .name = "wide.png", .bytes = 9847});
    rows.push_back({.parent = folder, .name = "small.png", .bytes = 1793});
    rows.push_back({.parent = folder, .name = "caption.txt", .bytes = 12});
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = static_cast<NodeId>(folder);
    tree.ancestors = parentChain(tree, tree.root);
    const auto scene = buildNestScene(tree, kSettings, 340, 476);
    checkRect(scene.core, 56, 56, 228, 364);
    const auto tiles = piecesOf(scene, NestPieceKind::Tile);
    TEST_CHECK(tiles.size() == 3 && morePiece(scene) == nullptr);
    TEST_CHECK(tree.nodes[tiles.front()->node].name == "wide.png");
    for (const auto* tile : tiles) {
      TEST_CHECK(
          tile->rect.w >= scene.profile.minTile.w - 2 - kEps && tile->rect.h >= scene.profile.minTile.h - 2 - kEps
      );
      TEST_CHECK(inside(tile->rect, 340, 476, scene.profile.pad));
    }
  }

  void buildingIsCheap() {
    auto fx = fixture();
    fx.at(12);
    const std::clock_t start = std::clock();
    for (int i = 0; i < 50; ++i) {
      const auto scene = buildNestScene(fx.tree, kSettings, 340.0 + (i % 5), 380);
      TEST_CHECK(!scene.pieces.empty());
    }
    TEST_CHECK(static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC < 3.0);
  }
} // namespace

int main() {
  compactHostsUseTheCompactProfile();
  compactScenesFitTheirHost();
  compactDeepSceneShowsFourTiles();
  profileSwitchesAtTheEdge();
  normalSizesKeepTheFullFrame();
  peekNeedsARoomyCore();
  peekMatchesKnownGeometry();
  peekGetsItsOwnMoreTile();
  focusFindsTilesSegmentsAndNothing();
  degenerateHostsGiveDefinedScenes();
  sceneMatchesKnownGeometry();
  focusEndsOnAParentCycle();
  emptyFolderIsMarked();
  threeFilesAllShowBesideTheirHeaviest();
  buildingIsCheap();
  std::println("workbench nest scene checks passed");
  return 0;
}
