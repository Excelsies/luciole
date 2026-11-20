#include "nest_pixel_stage.h"
#include "render/scene/text_node.h"
#include "scene_capture.h"
#include "test_check.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <iterator>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Nest labels drawn offscreen with real text: the labels of a narrow pane, small tiles under a large file and the
// measure at the right end of a one-line tile.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::test::nest_pixels;
  using ::test::GlPixels;
  using ::test::SceneCapture;

  std::size_t codePoints(std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(text, [](char c) {
      return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U;
    }));
  }

  // A folder of five folders and six files in panes of 340 by 540 and 250 by 400, as a narrow Nest beside the list
  // shows it: no count or size is cut, and a cut name keeps three characters on each side of the ellipsis.
  void narrowPaneLabels(SceneCapture& capture) {
    std::vector<luciole::test::WorkbenchRow> rows{
        {.parent = -1, .name = "tmp", .folder = true, .bytes = 400000, .items = 1},
        {.parent = 0, .name = "proj", .folder = true, .bytes = 363315, .items = 11},
    };
    const std::array<std::pair<std::string_view, double>, 11> entries{{
        {"assets", 175000},
        {"build", 80000},
        {"docs", 48000},
        {"src", 44500},
        {"tests", 5500},
        {"data.csv", 4000},
        {"script.py", 2150},
        {"README.md", 1536},
        {"a-very-long-column.txt", 900},
        {"notes.txt", 800},
        {"config.toml", 600},
    }};
    for (const auto& [name, bytes] : entries) {
      const bool folder = !name.contains('.');
      rows.push_back({.parent = 1, .name = name, .folder = folder, .bytes = bytes, .items = folder ? 2.0 : 0.0});
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, 1);
    for (const auto& [width, height, scale] :
         {std::tuple{340, 540, 1.0F}, std::tuple{340, 540, 1.5F}, std::tuple{250, 400, 1.0F}}) {
      Stage stage(tree, width, height);
      const auto pixels = stage.settle(capture, scale);
      keep(pixels, std::format("nest-narrow-{}x{}-{}x.png", width, height, scale));
      std::size_t shown = 0;
      for (const auto& child : stage.shapes().labelLayer().children()) {
        const auto* label = dynamic_cast<const TextNode*>(child.get());
        if (label == nullptr || !label->visible()) {
          continue;
        }
        ++shown;
        const std::string_view text = label->text();
        const auto cut = text.find("…");
        std::println("{}x label \"{}\"", scale, text);
        if (cut == std::string_view::npos) {
          continue;
        }
        const auto head = text.substr(0, cut);
        const auto tail = text.substr(cut + std::string_view("…").size());
        TEST_CHECK(!text.starts_with('+') && !text.ends_with("iB") && !text.ends_with("bytes"));
        TEST_CHECK(codePoints(head) >= 3 && codePoints(tail) >= 3);
      }
      TEST_CHECK(shown >= 6);
    }
  }

  // A folder of six folders and seven files under ten ancestors, in a 340 by 476 pane: the large font file over a
  // row of small tiles and "+N more".
  void smallTilesUnderALargeFile(SceneCapture& capture) {
    std::vector<luciole::test::WorkbenchRow> rows{{.parent = -1, .name = "/", .folder = true, .bytes = 800000}};
    for (const std::string_view name :
         {"home", "user", "Projects", "luciole", "cache", "tmp", "native", "harness", "prove", "fixture"}) {
      rows.push_back(
          {.parent = static_cast<int>(rows.size()) - 1, .name = name, .folder = true, .bytes = 727611, .items = 1}
      );
    }
    const int folder = static_cast<int>(rows.size()) - 1;
    rows.back().items = 13;
    const std::array<std::tuple<std::string_view, double, double>, 13> entries{{
        {"DejaVuSans-Bold.ttf", 708920, 0},
        {"Pictures", 11652, 3},
        {"gradient.png", 6297, 0},
        {"Styles", 221, 3},
        {"Documents", 145, 4},
        {"config.json", 107, 0},
        {"Projects", 74, 2},
        {"README.md", 67, 0},
        {"data.csv", 54, 0},
        {"Music", 42, 2},
        {"notes.txt", 31, 0},
        {"todo.txt", 21, 0},
        {"Empty", 0, 0},
    }};
    for (const auto& [name, bytes, items] : entries) {
      rows.push_back({.parent = folder, .name = name, .folder = !name.contains('.'), .bytes = bytes, .items = items});
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = static_cast<NodeId>(folder);
    tree.ancestors = parentChain(tree, tree.root);
    Stage stage(std::move(tree), 340, 476);
    const auto pixels = stage.settle(capture, 1);
    keep(pixels, "nest-small-tiles-340x476-1x.png");
    for (const auto& child : stage.shapes().labelLayer().children()) {
      const auto* label = dynamic_cast<const TextNode*>(child.get());
      if (label != nullptr && label->visible()) {
        std::println("small tiles: \"{}\"", label->text());
        const std::string_view text = label->text();
        TEST_CHECK(!text.starts_with('+') || !text.contains("…"));
      }
    }
  }

  // Device columns of a tile's inside, 3 px in from its edges, that hold a pixel far from its fill.
  std::vector<bool> inkedColumns(const GlPixels& pixels, const ShapeRect& tile, Rgba8 fill, float scale) {
    const Rgb fillRgb{fill.r, fill.g, fill.b};
    const int left = static_cast<int>(std::ceil((tile.x + 3) * scale));
    const int right = static_cast<int>(std::floor((tile.x + tile.width - 3) * scale));
    const int top = static_cast<int>(std::ceil((tile.y + 3) * scale));
    const int bottom = static_cast<int>(std::floor((tile.y + tile.height - 3) * scale));
    std::vector<bool> inked(static_cast<std::size_t>(pixels.width), false);
    for (int x = std::max(left, 0); x < std::min(right, pixels.width); ++x) {
      for (int y = std::max(top, 0); y < std::min(bottom, pixels.height); ++y) {
        if (distance(at(pixels, x, y), fillRgb) > 40) {
          inked[static_cast<std::size_t>(x)] = true;
          break;
        }
      }
    }
    return inked;
  }

  // A folder of a large file and a small one in a 360 by 400 pane: the small file's tile is 350 by 31 px, one line
  // with its long name cut in the middle and its size at the right end, ending at the tile's padding, apart from the
  // name.
  void oneLineTileMeasure(SceneCapture& capture) {
    const std::vector<luciole::test::WorkbenchRow> rows{
        {.parent = -1, .name = "work", .folder = true, .bytes = 605000, .items = 2},
        {.parent = 0, .name = "report.pdf", .bytes = 600000},
        {.parent = 0, .name = "quarterly-report-of-the-north-region-final-draft.txt", .bytes = 5000},
    };
    const auto tree = luciole::test::workbenchTree(rows, 0);
    const NodeId small = 2;
    for (const float scale : {1.0F, 1.5F}) {
      Stage stage(tree, 360, 400);
      const auto pixels = stage.settle(capture, scale);
      keep(pixels, std::format("nest-one-line-360x400-{}x.png", scale));
      const auto* tile = pieceShape(stage.shapes(), small);
      TEST_CHECK(tile != nullptr);
      if (tile == nullptr) {
        return;
      }
      TEST_CHECK(tile->rect.width > 300 && tile->rect.height < 46);
      bool measure = false;
      bool cutName = false;
      for (const auto& child : stage.shapes().labelLayer().children()) {
        const auto* label = dynamic_cast<const TextNode*>(child.get());
        if (label != nullptr && label->visible()) {
          const std::string_view text = label->text();
          // Sizes join number and unit with a no-break space.
          measure = measure || text == "4.9\u00A0KiB";
          cutName = cutName || (text.starts_with("qua") && text.contains("…") && text.ends_with("draft.txt"));
        }
      }
      TEST_CHECK(measure && cutName);
      const auto inked = inkedColumns(pixels, tile->rect, tile->fill, scale);
      const auto first = std::ranges::find(inked, true);
      const auto last = std::ranges::find(inked | std::views::reverse, true);
      TEST_CHECK(first != inked.end() && last != inked.rend());
      if (first == inked.end()) {
        continue;
      }
      const auto end = static_cast<int>(std::distance(inked.begin(), last.base()));
      const float padding = (tile->rect.x + tile->rect.width - 8) * scale;
      // The widest run of empty columns between the name and the measure.
      int gap = 0;
      int run = 0;
      for (auto x = first; x != last.base(); ++x) {
        run = *x ? 0 : run + 1;
        gap = std::max(gap, run);
      }
      std::println("{}x one-line tile: ink ends at {}, padding at {:.1f}, widest gap {} px", scale, end, padding, gap);
      TEST_CHECK(static_cast<float>(end) <= padding + 1);
      TEST_CHECK(static_cast<float>(end) >= padding - 2 * scale);
      TEST_CHECK(static_cast<float>(gap) >= 8 * scale);
    }
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  narrowPaneLabels(*capture);
  smallTilesUnderALargeFile(*capture);
  oneLineTileMeasure(*capture);
  std::println("nest label pixel checks passed");
  return 0;
}
