#include "nest_pixel_stage.h"
#include "render/scene/text_node.h"
#include "scene_capture.h"
#include "test_check.h"
#include "workbench/nest_scene.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <print>
#include <string>
#include <utility>
#include <vector>

// The Nest drawn offscreen with real text for a folder of small files beside large ones: every entry has its own
// tile, in its own color, and the tiles are at least the minimum size.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::test::nest_pixels;
  using ::test::GlPixels;
  using ::test::SceneCapture;

  // A folder four levels down holding `sizes`, each a file or a folder of three items.
  Tree folderOf(const std::vector<double>& sizes, bool folders) {
    std::vector<luciole::test::WorkbenchRow> rows{
        {.parent = -1, .name = "/", .folder = true, .bytes = 1e9, .items = 1}
    };
    static const std::vector<std::string> kLevels{"home", "sample", "Projects", "kinds"};
    for (const auto& name : kLevels) {
      rows.push_back(
          {.parent = static_cast<int>(rows.size()) - 1, .name = name, .folder = true, .bytes = 1e9, .items = 1}
      );
    }
    const int folder = static_cast<int>(rows.size()) - 1;
    static std::vector<std::string> names;
    names.clear();
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      names.push_back(folders ? std::format("folder{}", i) : std::format("file-{:02}.dat", i));
    }
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      rows.push_back(
          {.parent = folder, .name = names[i], .folder = folders, .bytes = sizes[i], .items = folders ? 3.0 : 0.0}
      );
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = static_cast<NodeId>(folder);
    tree.ancestors = parentChain(tree, tree.root);
    return tree;
  }

  // Whether the 3 px just inside a tile's top left corner shows the tile's fill: the tile is drawn, not covered by
  // a "+N more" tile or left out.
  bool drawn(const GlPixels& pixels, const Shape& tile, float scale) {
    const auto p = at(pixels, (tile.rect.x + 3) * scale, (tile.rect.y + 3) * scale);
    return distance(p, Rgb{tile.fill.r, tile.fill.g, tile.fill.b}) < 30;
  }

  void
  checkFolder(SceneCapture& capture, Tree tree, int width, int height, const std::string& name, std::size_t entries) {
    const auto copy = tree;
    Stage stage(std::move(tree), width, height);
    const auto pixels = stage.settle(capture, 1);
    keep(pixels, name);
    const auto scene = buildNestScene(copy, {}, width, height);
    std::size_t tiles = 0;
    std::size_t visible = 0;
    for (const auto& piece : scene.pieces) {
      if (piece.kind != NestPieceKind::Tile) {
        continue;
      }
      ++tiles;
      const auto* shape = pieceShape(stage.shapes(), piece.node);
      TEST_CHECK(shape != nullptr);
      visible += drawn(pixels, *shape, 1) ? 1U : 0U;
      TEST_CHECK(
          piece.rect.w >= scene.profile.minTile.w - 2 - 1e-6 && piece.rect.h >= scene.profile.minTile.h - 2 - 1e-6
      );
    }
    std::println("{}: {} of {} entries have a tile, {} drawn in their color", name, tiles, entries, visible);
    TEST_CHECK(tiles == entries);
    TEST_CHECK(visible == tiles);
    TEST_CHECK(pieceShape(stage.shapes(), scene.folder, SceneRole::More) == nullptr);
  }

  void smallFilesBesideLargeOnes(SceneCapture& capture) {
    std::vector<double> sizes{692300, 147700, 49300};
    for (int i = 0; i < 20; ++i) {
      sizes.push_back(80.0 + 90.0 * i);
    }
    checkFolder(capture, folderOf(sizes, false), 340, 370, "nest-kinds-340x370.png", sizes.size());
    checkFolder(capture, folderOf(sizes, false), 500, 400, "nest-kinds-500x400.png", sizes.size());
  }

  // Whether a visible text of the view starts inside `rect` and begins like `name`: the tile shows its name.
  bool showsName(const ShapeView& view, const ShapeRect& rect, const std::string& name) {
    return std::ranges::any_of(view.labelLayer().children(), [&](const auto& child) {
      const auto* label = dynamic_cast<const TextNode*>(child.get());
      if (label == nullptr || !label->visible() || label->text().empty()) {
        return false;
      }
      const bool inside = label->x() >= rect.x
          && label->x() <= rect.x + rect.width
          && label->y() >= rect.y
          && label->y() <= rect.y + rect.height;
      return inside && name.starts_with(label->text().substr(0, 3));
    });
  }

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
    std::vector<double> sizes{692300, 147700, 49300};
    for (int i = 0; i < 20; ++i) {
      sizes.push_back(80.0 + 90.0 * i);
    }
    std::vector<luciole::test::WorkbenchRow> rows{
        {.parent = -1, .name = "/", .folder = true, .bytes = 1e9, .items = 1},
        {.parent = 0, .name = "kinds", .folder = true, .bytes = 1e9, .items = 1},
    };
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      rows.push_back({.parent = 1, .name = names[i], .folder = false, .bytes = sizes[i], .items = 0.0});
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, tree.root);
    return tree;
  }

  // Every drawn tile of a folder of mixed kinds shows its name, at three sizes of the pane.
  void everyDrawnTileShowsItsName(SceneCapture& capture) {
    for (const auto& [width, height] : std::vector<std::pair<int, int>>{{340, 370}, {1100, 700}, {1920, 1080}}) {
      const auto tree = kindsFolder();
      Stage stage(tree, width, height);
      const auto pixels = stage.settle(capture, 1);
      keep(pixels, std::format("nest-names-{}x{}.png", width, height));
      std::size_t tiles = 0;
      std::size_t named = 0;
      for (const auto& shape : stage.shapes().shapes()) {
        if (!shape.hittable || sceneRole(shape.id.role) != SceneRole::Piece || shape.kind != ShapeKind::RoundedRect) {
          continue;
        }
        ++tiles;
        const bool shows = showsName(stage.shapes(), shape.rect, tree.nodes[shape.id.node].name);
        named += shows ? 1U : 0U;
        if (!shows) {
          std::println("{}x{}: no name on the tile of {}", width, height, tree.nodes[shape.id.node].name);
        }
      }
      std::println("{}x{}: {} tiles, {} with their name", width, height, tiles, named);
      TEST_CHECK(tiles >= 18);
      TEST_CHECK(named == tiles);
    }
  }

  void largeFoldersStayWhole(SceneCapture& capture) {
    const std::vector<double> sizes{120e6, 70e6, 46e6, 20e6, 10e6, 5e6, 2e6, 1e6, 5e5};
    checkFolder(capture, folderOf(sizes, true), 340, 385, "nest-folders-340x385.png", sizes.size());
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  smallFilesBesideLargeOnes(*capture);
  largeFoldersStayWhole(*capture);
  everyDrawnTileShowsItsName(*capture);
  std::println("nest tiles pixel checks passed");
  return 0;
}
