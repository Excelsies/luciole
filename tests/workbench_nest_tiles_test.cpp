#include "test_check.h"
#include "workbench/nest_scene.h"
#include "workbench/nest_tiles.h"
#include "workbench_fixture.h"
#include "workbench_nest_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <format>
#include <functional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::nest;
  using luciole::test::WorkbenchRow;

  // A folder below `depth` ancestors holding the given sizes, heaviest first.
  Tree folderOf(const std::vector<double>& sizes, bool folders, std::size_t depth = 4) {
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "/", .folder = true, .bytes = 1e9, .items = 1}};
    for (std::size_t i = 0; i < depth; ++i) {
      rows.push_back(
          {.parent = static_cast<int>(rows.size()) - 1,
           .name = std::format("level{}", i),
           .folder = true,
           .bytes = 1e9,
           .items = 1}
      );
    }
    const int folder = static_cast<int>(rows.size()) - 1;
    static const std::vector<std::string> kNames = [] {
      std::vector<std::string> names;
      names.reserve(800);
      for (int i = 0; i < 800; ++i) {
        names.push_back(std::format("entry-{}.dat", i));
      }
      return names;
    }();
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      rows.push_back(
          {.parent = folder, .name = kNames[i], .folder = folders, .bytes = sizes[i], .items = folders ? 3.0 : 0.0}
      );
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = static_cast<NodeId>(folder);
    tree.ancestors = parentChain(tree, tree.root);
    return tree;
  }

  // 3 large files and 20 small ones, as in a folder of mixed kinds.
  std::vector<double> kindsSizes() {
    std::vector<double> sizes{692300, 147700, 49300};
    for (int i = 0; i < 20; ++i) {
      sizes.push_back(80.0 + 90.0 * i);
    }
    return sizes;
  }

  std::size_t foldedCount(const NestScene& scene) {
    const auto* more = morePiece(scene);
    return more == nullptr ? 0 : more->nodes.size();
  }

  void everyTileMeetsTheMinimum(const NestScene& scene) {
    auto boxes = piecesOf(scene, NestPieceKind::Tile);
    if (const auto* more = morePiece(scene)) {
      boxes.push_back(more);
    }
    for (const auto* tile : boxes) {
      TEST_CHECK(tile->rect.w >= scene.profile.minTile.w - 2 - kEps);
      TEST_CHECK(tile->rect.h >= scene.profile.minTile.h - 2 - kEps);
      TEST_CHECK(inside(tile->rect, scene.width, scene.height, scene.profile.pad));
    }
  }

  void smallFilesBesideLargeOnesAreNotFolded() {
    const auto tree = folderOf(kindsSizes(), false);
    for (const auto& [w, h] : std::vector<std::pair<double, double>>{{340, 370}, {500, 400}, {600, 410}}) {
      const auto scene = buildNestScene(tree, kSettings, w, h);
      const auto tiles = piecesOf(scene, NestPieceKind::Tile).size();
      TEST_CHECK(tiles >= 18);
      TEST_CHECK(foldedCount(scene) <= 5);
      everyTileMeetsTheMinimum(scene);
    }
  }

  void largeFoldersAreNeverFoldedBeforeSmallOnes() {
    const std::vector<double> sizes{120e6, 70e6, 46e6, 20e6, 10e6, 5e6, 2e6, 1e6, 5e5};
    const auto tree = folderOf(sizes, true);
    const auto scene = buildNestScene(tree, kSettings, 340, 385);
    const auto tiles = piecesOf(scene, NestPieceKind::Tile);
    TEST_CHECK(tiles.size() >= 7);
    // The folded entries are the lightest ones.
    if (const auto* more = morePiece(scene)) {
      double heaviestFolded = 0;
      for (const auto id : more->nodes) {
        heaviestFolded = std::max(heaviestFolded, tree.nodes[id].bytes);
      }
      for (const auto* tile : tiles) {
        TEST_CHECK(tree.nodes[tile->node].bytes >= heaviestFolded);
      }
    }
    everyTileMeetsTheMinimum(scene);
  }

  void anEntryFoldsOnlyWhenTheCoreIsFull() {
    for (const std::size_t count : {40U, 120U, 300U}) {
      std::vector<double> sizes;
      sizes.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        sizes.push_back(1000.0 + static_cast<double>((i * 7919) % 5000));
      }
      std::ranges::sort(sizes, std::greater<>{});
      const auto tree = folderOf(sizes, false);
      const auto scene = buildNestScene(tree, kSettings, 900, 700);
      everyTileMeetsTheMinimum(scene);
      if (foldedCount(scene) > 0) {
        // One more tile of the minimum size would not have fitted.
        const auto tiles = static_cast<double>(piecesOf(scene, NestPieceKind::Tile).size() + 2);
        const double area = scene.core.w * scene.core.h;
        TEST_CHECK(area < tiles * scene.profile.minTile.w * scene.profile.minTile.h * 2.2);
      }
    }
  }

  void equalFilesAllShowWhileTheyHaveRoom() {
    const std::vector<double> sizes(30, 4096.0);
    const auto tree = folderOf(sizes, false);
    const auto scene = buildNestScene(tree, kSettings, 700, 500);
    TEST_CHECK(foldedCount(scene) == 0);
    TEST_CHECK(piecesOf(scene, NestPieceKind::Tile).size() == 30);
    everyTileMeetsTheMinimum(scene);
  }

  // Widths of names at 7 px a code point, as a font with that advance gives.
  double sevenPx(std::string_view text) {
    double points = 0;
    for (const char c : text) {
      points += (static_cast<unsigned char>(c) & 0xC0U) != 0x80U ? 1.0 : 0.0;
    }
    return 7 * points;
  }

  // A folder of 3 large files and 20 small ones named as in a folder of mixed kinds.
  Tree kindsFolder() {
    const std::vector<std::string> names{
        "clip.webm",
        "budget.xlsx",
        "photo-2024-08.jpeg",
        "notes.md",
        "a.c",
        "archive-final.zip",
        "song.flac",
        "readme",
        "report.pdf",
        "data.sqlite3",
        "slides.pptx",
        "icon.svg",
        "talk.ogg",
        "build.log",
        "x.py",
        "config.toml",
        "diagram.png",
        "backup.tar.gz",
        "script.sh",
        "very-long-file-name-here.txt",
        "movie-trailer.mkv",
        "scan-0001.tiff",
        "todo.txt",
    };
    const auto sizes = kindsSizes();
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "/", .folder = true, .bytes = 1e9, .items = 1}};
    rows.push_back({.parent = 0, .name = "kinds", .folder = true, .bytes = 1e9, .items = 1});
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      rows.push_back({.parent = 1, .name = names[i], .folder = false, .bytes = sizes[i], .items = 0.0});
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, tree.root);
    return tree;
  }

  void everyTileShowsItsName() {
    const auto tree = kindsFolder();
    for (const auto& [w, h] : std::vector<std::pair<double, double>>{{340, 370}, {1100, 700}, {1920, 1080}}) {
      const auto scene = buildNestScene(tree, kSettings, w, h, {}, sevenPx);
      std::size_t tiles = 0;
      for (const auto* tile : piecesOf(scene, NestPieceKind::Tile)) {
        ++tiles;
        const auto* label = std::get_if<NestTileLabel>(&tile->label);
        TEST_CHECK(label != nullptr);
        if (label != nullptr) {
          const auto shown = fitNestText(label->name, label->nameWidth, sevenPx);
          if (shown.empty()) {
            std::println("{}x{}: no name for {}", w, h, label->name);
          }
          TEST_CHECK(!shown.empty());
        }
      }
      // Names that need room do not fold their neighbors that show theirs.
      TEST_CHECK(tiles >= 18);
      everyTileMeetsTheMinimum(scene);
    }
  }

  void aNameThatNeedsMoreRoomGetsIt() {
    const auto tree = kindsFolder();
    const auto without = buildNestScene(tree, kSettings, 340, 370);
    const auto with = buildNestScene(tree, kSettings, 340, 370, {}, sevenPx);
    // "clip.webm" cut to "cli….webm" is 63 px; a tile that shows it is at least 63 + 16 + 2 px wide.
    bool found = false;
    for (const auto* tile : piecesOf(with, NestPieceKind::Tile)) {
      if (tree.nodes[tile->node].name == "clip.webm") {
        found = true;
        TEST_CHECK(tile->rect.w >= 63 + 16 - kEps);
      }
    }
    TEST_CHECK(found);
    TEST_CHECK(!piecesOf(without, NestPieceKind::Tile).empty());
  }

  void aNameWithoutRoomFoldsItsEntryOnly() {
    // A long name beside three short ones: its need is bigger than a tile of equal weights can give in 3 columns.
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "/", .folder = true, .bytes = 1e9, .items = 1}};
    rows.push_back({.parent = 0, .name = "d", .folder = true, .bytes = 1e9, .items = 1});
    rows.push_back({.parent = 1, .name = "a.c", .folder = false, .bytes = 100, .items = 0});
    rows.push_back({.parent = 1, .name = "b.c", .folder = false, .bytes = 100, .items = 0});
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, tree.root);
    const auto scene = buildNestScene(tree, kSettings, 500, 400, {}, sevenPx);
    TEST_CHECK(piecesOf(scene, NestPieceKind::Tile).size() == 2);
    TEST_CHECK(morePiece(scene) == nullptr);
  }

  void layoutIsDeterministicAndBounded() {
    std::vector<double> sizes;
    sizes.reserve(5000);
    for (int i = 0; i < 5000; ++i) {
      sizes.push_back(1.0 + ((i * 7919) % 5000));
    }
    std::ranges::sort(sizes, std::greater<>{});
    const auto tree = folderOf(std::vector<double>(sizes.begin(), sizes.begin() + 700), false);
    const std::clock_t start = std::clock();
    for (int i = 0; i < 10; ++i) {
      const auto scene = buildNestScene(tree, kSettings, 1400, 900);
      TEST_CHECK(!scene.pieces.empty());
    }
    TEST_CHECK(static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC < 6.0);
  }

  void floorSharesRaiseOnlyTheSmallest() {
    const std::vector<double> weights{90, 6, 2, 2};
    const std::vector<double> floors(4, 0.05);
    const auto shares = floorShares(weights, floors);
    TEST_CHECK(shares.size() == 4);
    TEST_NEAR(shares[2], 0.05, 1e-12);
    TEST_NEAR(shares[3], 0.05, 1e-12);
    // The others keep their proportion: 90 to 6.
    TEST_NEAR(shares[0] / shares[1], 15.0, 1e-9);
    double sum = 0;
    for (const double share : shares) {
      sum += share;
    }
    TEST_NEAR(sum, 1.0, 1e-12);
    // No floor needed: plain shares.
    const auto plain = floorShares(weights, std::vector<double>(4, 0.01));
    TEST_NEAR(plain[0], 0.9, 1e-12);
    TEST_NEAR(plain[3], 0.02, 1e-12);
  }

  void floorsOfOneEntryRaiseOnlyThatEntry() {
    const std::vector<double> weights{50, 30, 10, 10};
    const std::vector<double> floors{0, 0, 0.25, 0.02};
    const auto shares = floorShares(weights, floors);
    TEST_CHECK(shares.size() == 4);
    TEST_NEAR(shares[2], 0.25, 1e-12);
    // Entry 3 keeps the proportion it has, 0.1 of what is free.
    TEST_NEAR(shares[3] / shares[1], 1.0 / 3.0, 1e-9);
    TEST_NEAR(shares[0] / shares[1], 50.0 / 30.0, 1e-9);
    double sum = 0;
    for (const double share : shares) {
      sum += share;
    }
    TEST_NEAR(sum, 1.0, 1e-12);
  }

  void floorSharesRefuseWhatCannotFit() {
    const std::vector<double> weights{1, 1, 1};
    TEST_CHECK(floorShares(weights, std::vector<double>(3, 0.4)).empty());
    TEST_CHECK(floorShares({}, std::vector<double>{0.1}).empty());
    TEST_CHECK(floorShares(weights, std::vector<double>(3, kNan)).empty());
    TEST_CHECK(floorShares(weights, std::vector<double>(3, -0.1)).empty());
    const std::vector<double> bad{1, 0, 1};
    TEST_CHECK(floorShares(bad, std::vector<double>(3, 0.1)).empty());
    const std::vector<double> infinite{1, kInf};
    TEST_CHECK(floorShares(infinite, std::vector<double>(2, 0.1)).empty());
    const std::vector<double> negative{1, -2};
    TEST_CHECK(floorShares(negative, std::vector<double>(2, 0.1)).empty());
  }

  void minimumTilesMeetTheMinimumOrGiveNothing() {
    const NestSize min{.w = 64, .h = 28};
    const std::vector<double> weights{600, 300, 40, 30, 20, 10};
    const TreemapRect rect{.x = 10, .y = 20, .w = 300, .h = 240};
    const auto tiles = minimumTiles(weights, rect, min);
    TEST_CHECK(tiles.size() == weights.size());
    double area = 0;
    for (const auto& tile : tiles) {
      TEST_CHECK(tile.w >= min.w - kEps && tile.h >= min.h - kEps);
      TEST_CHECK(tile.x >= rect.x - kEps && tile.y >= rect.y - kEps);
      TEST_CHECK(tile.x + tile.w <= rect.x + rect.w + kEps && tile.y + tile.h <= rect.y + rect.h + kEps);
      area += tile.w * tile.h;
    }
    TEST_NEAR(area, rect.w * rect.h, 1e-6);
    // Room for fewer tiles than weights.
    TEST_CHECK(minimumTiles(weights, {.x = 0, .y = 0, .w = 100, .h = 60}, min).empty());
    TEST_CHECK(minimumTiles({}, rect, min).empty());
    TEST_CHECK(minimumTiles(weights, {.x = 0, .y = 0, .w = kNan, .h = 60}, min).empty());
    TEST_CHECK(minimumTiles(weights, rect, {.w = 0, .h = 28}).empty());
  }

  void aStripOrAloneMoreWhenNothingFitsBesideIt() {
    const NestSize min{.w = 56, .h = 24};
    const std::vector<double> weights{40, 10, 10};
    // Squarified rows leave no tile beside "+N more", but a tile and a strip fit.
    const auto strip = layoutNestTiles(weights, {.x = 0, .y = 0, .w = 87, .h = 52}, min, 700);
    TEST_CHECK(strip.tiles.size() == 1 && strip.more.has_value());
    TEST_NEAR(strip.more->h, 28 - 2, kEps);
    TEST_NEAR(strip.more->w, 85, kEps);
    TEST_NEAR(strip.tiles[0].h, 52 - 28 - 2, kEps);
    // Too short for both.
    const auto alone = layoutNestTiles(weights, {.x = 0, .y = 0, .w = 60, .h = 40}, min, 700);
    TEST_CHECK(alone.tiles.empty() && alone.more.has_value());
    TEST_NEAR(alone.more->w, 58, kEps);
    // Under the minimum: nothing.
    const auto none = layoutNestTiles(weights, {.x = 0, .y = 0, .w = 50, .h = 90}, min, 700);
    TEST_CHECK(none.tiles.empty() && !none.more.has_value());
    const std::vector<double> unusable{0.0};
    const auto bad = layoutNestTiles(unusable, {.x = 0, .y = 0, .w = 500, .h = 500}, min, 700);
    TEST_CHECK(bad.tiles.empty() && !bad.more.has_value());
    const auto empty = layoutNestTiles({}, {.x = 0, .y = 0, .w = 500, .h = 500}, min, 700);
    TEST_CHECK(empty.tiles.empty() && !empty.more.has_value());
  }

  void maxShownBoundsTheTiles() {
    const NestSize min{.w = 56, .h = 24};
    const std::vector<double> weights(50, 10.0);
    const auto layout = layoutNestTiles(weights, {.x = 0, .y = 0, .w = 900, .h = 600}, min, 12);
    TEST_CHECK(layout.tiles.size() <= 12 && layout.more.has_value());
  }
} // namespace

int main() {
  floorSharesRaiseOnlyTheSmallest();
  floorsOfOneEntryRaiseOnlyThatEntry();
  floorSharesRefuseWhatCannotFit();
  minimumTilesMeetTheMinimumOrGiveNothing();
  aStripOrAloneMoreWhenNothingFitsBesideIt();
  maxShownBoundsTheTiles();
  smallFilesBesideLargeOnesAreNotFolded();
  largeFoldersAreNeverFoldedBeforeSmallOnes();
  anEntryFoldsOnlyWhenTheCoreIsFull();
  equalFilesAllShowWhileTheyHaveRoom();
  everyTileShowsItsName();
  aNameThatNeedsMoreRoomGetsIt();
  aNameWithoutRoomFoldsItsEntryOnly();
  layoutIsDeterministicAndBounded();
  std::println("workbench nest tiles checks passed");
  return 0;
}
