#include "scene_capture.h"
#include "test_check.h"
#include "text_clear.h"
#include "text_tile_view_rig.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/jump_list_view.h"
#include "ui/workbench/list_rows.h"
#include "workbench/column_plan.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <print>
#include <string>
#include <utility>
#include <vector>

// The rows of the Jump ahead list and of the columns ahead, laid out with the toolkit's text shaping at the text
// scale a 16 pt system font gives: their two lines or one fit the row, and none lies over another.
namespace {
  using namespace luciole;

  constexpr float kLargeFont = 16.0F * 96.0F / 72.0F / 14.0F; // 16 pt in px over the 14 px body size
  constexpr float kRowWidth = 300;

  void collectTiles(const Node& node, std::vector<const JumpRowTile*>& out) {
    if (!node.visible()) {
      return;
    }
    if (const auto* tile = dynamic_cast<const JumpRowTile*>(&node); tile != nullptr) {
      out.push_back(tile);
    }
    for (const auto& child : node.children()) {
      collectTiles(*child, out);
    }
  }

  void jumpRowsFit(::test::SceneCapture& capture) {
    JumpListView list;
    JumpListContent content;
    for (int i = 0; i < 4; ++i) {
      workbench::JumpRow row;
      row.id = static_cast<workbench::NodeId>(i + 1);
      row.path = std::format("projects/noctalia/files/src{}", i);
      row.split = {.prefix = "projects/noctalia/files/", .leaf = std::format("src{}", i)};
      row.label = "1.2 GiB";
      row.share = 0.4;
      content.list.rows.push_back(row);
    }
    list.setContent(std::move(content), 1);
    (void)capture.capture(list, 320, 400, 1.0F);
    std::vector<const JumpRowTile*> tiles;
    collectTiles(list, tiles);
    TEST_CHECK(tiles.size() == 4);
    for (const auto* tile : tiles) {
      ::test::expectTextWithin(*tile, capture.textScale());
    }
    TEST_CHECK(::test::expectTextClear(list, capture.textScale()) == 12);
  }

  void quietRowsFit(::test::SceneCapture& capture) {
    FileIconCache icons;
    DirectoryEntry entry;
    entry.name = "Quarterly planning notes.txt";
    entry.type = FileType::Regular;
    for (std::size_t column = 0; column < workbench::kColumnsAhead; ++column) {
      QuietRow row(icons, column);
      row.bind(entry, {}, 0.5, quietTextSize(Style::fontSizeBody, column));
      // a row grows with its line of text past its base height
      const float line = 1.3F * quietTextSize(Style::fontSizeBody, column) * capture.textScale();
      const auto height =
          static_cast<int>(std::max(static_cast<float>(workbench::kColumnRowHeights[column]), std::ceil(line + 5.0F)));
      (void)capture.capture(row, static_cast<int>(kRowWidth), height, 1.0F);
      std::println("  column {} row {} px", column, height);
      TEST_CHECK(::test::expectTextClear(row, capture.textScale()) == 1);
      ::test::expectTextWithin(row, capture.textScale());
    }
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  for (const float textScale : {1.0F, 44.0F / 42.0F, kLargeFont, 2.0F}) {
    capture->setTextScale(textScale);
    std::println("text scale {:.2f}", textScale);
    jumpRowsFit(*capture);
    quietRowsFit(*capture);
  }
  std::println("workbench-rows-text-scale: all checks passed");
  return 0;
}
