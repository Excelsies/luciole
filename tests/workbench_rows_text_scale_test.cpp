#include "scene_capture.h"
#include "test_check.h"
#include "text_clear.h"
#include "text_tile_view_rig.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/jump_list_view.h"
#include "ui/workbench/list_rows.h"
#include "ui/workbench/quiet_column_view.h"
#include "workbench/column_plan.h"

#include <algorithm>
#include <array>
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

  // The icons of the columns ahead beside a list of icons `listIcon` px (0 is the default 32), in whole pixels: 16, 14
  // and 12 at the default, in proportion to the list's.
  float columnIcon(int listIcon, std::size_t column) {
    const std::array<std::array<float, 3>, 4> table{{{8, 7, 6}, {16, 14, 12}, {20, 18, 15}, {32, 28, 24}}};
    const std::size_t step = listIcon == 16 ? 0 : listIcon == 0 ? 1 : listIcon == 40 ? 2 : 3;
    return table.at(step).at(column);
  }

  void jumpRowsFit(::test::SceneCapture& capture, int listIcon) {
    JumpListView list;
    list.setListIconSize(listIcon);
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
    // the rows hold two lines and move with the list's icon: 38 at the default, 6 px less at 16, 16 more at 64
    const float line = capture.lineHeight(Style::fontSizeCaption) + capture.lineHeight(Style::fontSizeMini);
    const float want = std::max(38.0F + columnIcon(listIcon, 0) - 16.0F, std::ceil(line + 6));
    std::println("  jump row {} px, wanted {} px at icon {}", tiles[0]->height(), want, listIcon);
    TEST_CHECK(std::abs(tiles[0]->height() - want) < 1.0F);
    for (const auto* tile : tiles) {
      ::test::expectTextWithin(*tile, capture.textScale());
    }
    TEST_CHECK(::test::expectTextClear(list, capture.textScale()) == 12);
  }

  void quietRowsFit(::test::SceneCapture& capture, int listIcon) {
    FileIconCache icons;
    DirectoryEntry entry;
    entry.name = "Quarterly planning notes.txt";
    entry.type = FileType::Regular;
    for (std::size_t column = 0; column < workbench::kColumnsAhead; ++column) {
      // the column lays its rows out at the height its line of text asks for
      QuietColumnView view(icons, column, [] { return 0U; });
      view.setListIconSize(listIcon);
      (void)capture.capture(view, static_cast<int>(kRowWidth), 400, 1.0F);
      const float size = quietTextSize(Style::fontSizeBody, column);
      const float line = capture.lineHeight(size);
      std::println("  column {} row {} px for a {} px line", column, view.rowHeight(), line);
      // the icon is the column's own size in the list's proportion; its row is 10 px taller
      const float icon = columnIcon(listIcon, column);
      TEST_CHECK(quietIconSize(listIcon, column) == icon);
      TEST_CHECK(view.rowHeight() == std::max(10 + icon, std::ceil(line + 3)));
      TEST_CHECK(view.rowHeight() >= line + 2);
      TEST_CHECK(view.headerHeight() >= capture.lineHeight(size) + 8);
      if (capture.textScale() == 1.0F && listIcon == 0) {
        TEST_CHECK(view.rowHeight() == workbench::kColumnRowHeights[column]);
        TEST_CHECK(view.headerHeight() == workbench::kColumnHeaderHeight);
      }
      QuietRow row(icons, column);
      row.setIconSize(icon);
      row.bind(entry, {}, 0.5, size);
      (void)capture.capture(row, static_cast<int>(kRowWidth), static_cast<int>(view.rowHeight()), 1.0F);
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
    for (const int listIcon : {16, 0, 40, 64}) {
      jumpRowsFit(*capture, listIcon);
      quietRowsFit(*capture, listIcon);
    }
  }
  std::println("workbench-rows-text-scale: all checks passed");
  return 0;
}
