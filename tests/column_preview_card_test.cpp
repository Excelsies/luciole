#include "column_preview_fixture.h"
#include "column_preview_rig.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "thumbnails/mesh_preview.h"
#include "thumbnails/sheet_preview.h"
#include "thumbnails/thumbnail_worker.h"
#include "ui/controls/scrollbar.h"
#include "ui/controls/table_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The page card around a text, a table and an archive's listing draws its outline whole: on all four sides, over the
// whole length of each side, at every column width and pixel scale. A table's and a listing's card is as tall as its
// rows, up to the room the pane gives it.
namespace {
  using luciole::TableView;
  using luciole::test::preview::Rig;
  using luciole::test::tiles::bytesOf;
  using luciole::test::tiles::distance;

  constexpr int kHeight = 420;

  // The pixels along each side of the card, past its corners, are the outline.
  void outlineIsWhole(const ::test::GlPixels& pixels, const Node& card, float scale, std::string_view what) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&card, x, y);
    const auto outline = bytesOf(palette.outlineVariant);
    const int left = static_cast<int>(std::floor(x * scale));
    const int top = static_cast<int>(std::floor(y * scale));
    const int right = static_cast<int>(std::floor((x + card.width()) * scale)) - 1;
    const int bottom = static_cast<int>(std::floor((y + card.height()) * scale)) - 1;
    const int corner = static_cast<int>(std::ceil((Style::radiusXl + 2.0F) * scale));
    TEST_CHECK(right - left > 4 * corner && bottom - top > 2 * corner);
    // Pixels missing along the top, bottom, left and right.
    std::array<int, 4> missing{};
    // The outline is Style::borderWidth wide, which at a fractional scale or position blends into two pixels: one of
    // the two at the edge is the outline's color.
    const auto on = [&](int side, int px, int py, int inwardX, int inwardY) {
      const bool found =
          distance(pixels.at(px, py), outline) < 30 || distance(pixels.at(px + inwardX, py + inwardY), outline) < 30;
      missing.at(static_cast<std::size_t>(side)) += found ? 0 : 1;
    };
    for (int column = left + corner; column < right - corner; ++column) {
      on(0, column, top, 0, 1);
      on(1, column, bottom, 0, -1);
    }
    for (int row = top + corner; row < bottom - corner; ++row) {
      on(2, left, row, 1, 0);
      on(3, right, row, -1, 0);
    }
    std::println(
        "{} at {}x: outline pixels missing: top {}, bottom {}, left {}, right {}", what, scale, missing[0], missing[1],
        missing[2], missing[3]
    );
    TEST_CHECK((missing == std::array<int, 4>{}));
  }

  struct Placed {
    TableView* table = nullptr;
    const Node* card = nullptr;
    float room = 0;
  };

  Placed placed(const Rig& rig) {
    auto* table = dynamic_cast<TableView*>(rig.view->content());
    TEST_CHECK(table != nullptr && table->parent() != nullptr && table->parent()->parent() != nullptr);
    const Node* card = table->parent();
    return {.table = table, .card = card, .room = card->parent()->height() - (2 * Style::spaceSm)};
  }

  bool verticalBar(const TableView& table) {
    for (const auto& child : table.canvas().children()) {
      const auto* bar = dynamic_cast<const Scrollbar*>(child.get());
      if (bar != nullptr && bar->visible() && bar->orientation() == ScrollOrientation::Vertical) {
        return true;
      }
    }
    return false;
  }

  bool sidewaysBar(const TableView& table) {
    for (const auto& child : table.canvas().children()) {
      const auto* bar = dynamic_cast<const Scrollbar*>(child.get());
      if (bar != nullptr && bar->visible() && bar->orientation() == ScrollOrientation::Horizontal) {
        return true;
      }
    }
    return false;
  }

  // Where the last row of the grid ends, in the scene: the lowest point of the canvas that is on a cell.
  float lastRowBottom(const TableView& table) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&table.canvas(), x, y);
    const auto last = table.table()->rowCount - 1;
    for (float at = table.canvas().height() - 0.5F; at > 0; at -= 0.5F) {
      const auto cell = table.canvas().cellAt(70.0F, at);
      if (cell && cell->row == last) {
        return y + at;
      }
    }
    TEST_CHECK(false);
    return 0;
  }

  float bottomOf(const Node& node) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    return y + node.height();
  }

  // The card ends the card's padding below the last row, with the sideways bar between them when the table needs one.
  void hugsItsRows(const Rig& rig, std::string_view what) {
    const auto [table, card, room] = placed(rig);
    const float bar = sidewaysBar(*table) ? Style::scrollbarWidth : 0.0F;
    const float gap = bottomOf(*card) - lastRowBottom(*table);
    std::println("{}: card {} of {} high, {} below the last row, bar {}", what, card->height(), room, gap, bar);
    TEST_CHECK(card->height() < room - 20.0F);
    TEST_CHECK(gap >= Style::spaceSm + bar - 0.01F && gap <= Style::spaceSm + bar + 1.0F);
    TEST_CHECK(!verticalBar(*table));
  }

  void fillsTheRoom(const Rig& rig, std::string_view what) {
    const auto [table, card, room] = placed(rig);
    std::println("{}: card {} of {} high", what, card->height(), room);
    TEST_CHECK(card->height() > room - 1.0F && card->height() < room + 0.5F);
  }

  const std::array<int, 3> kWidths{190, 262, 460};
  const std::array<float, 2> kScales{1.0F, 1.5F};

  void smallTablesAndListingsAreHugged(Rig& rig) {
    for (const auto* file : {"tiny.csv", "pack.zip", "wide-short.csv"}) {
      rig.show(file);
      for (const int width : kWidths) {
        for (const float scale : kScales) {
          (void)rig.settle(width, kHeight, scale);
          hugsItsRows(rig, std::format("{} in {} at {}x", file, width, scale));
        }
      }
    }
  }

  void largeTablesAndListingsFillTheRoom(Rig& rig) {
    for (const auto* file : {"data.csv", "large.zip", "wide-long.csv"}) {
      rig.show(file);
      for (const int width : kWidths) {
        for (const float scale : kScales) {
          (void)rig.settle(width, kHeight, scale);
          fillsTheRoom(rig, std::format("{} in {} at {}x", file, width, scale));
        }
      }
    }
  }

  // A table wider than its card scrolls sideways in a card that hugs its rows, and a long one scrolls up and down.
  void wideTablesStillScroll(Rig& rig) {
    rig.show("wide-short.csv");
    (void)rig.settle(262, kHeight, 1.0F);
    auto* table = placed(rig).table;
    TEST_CHECK(sidewaysBar(*table) && table->scrollX() == 0);
    table->select({.row = 3, .column = 29});
    for (int i = 0; i < 4; ++i) {
      (void)rig.frame(262, kHeight, 1.0F);
    }
    TEST_CHECK(table->scrollX() > 100.0F);
    TEST_CHECK(table->scrollY() == 0);
    hugsItsRows(rig, "wide-short.csv scrolled sideways");
    rig.show("wide-long.csv");
    (void)rig.settle(262, kHeight, 1.0F);
    auto* tall = placed(rig).table;
    tall->select({.row = 299, .column = 29});
    for (int i = 0; i < 4; ++i) {
      (void)rig.frame(262, kHeight, 1.0F);
    }
    TEST_CHECK(tall->scrollX() > 100.0F && tall->scrollY() > 1000.0F);
    fillsTheRoom(rig, "wide-long.csv scrolled both ways");
  }

  // Laid out again, a card is the height it was: nothing oscillates as the bars come and go with the width.
  void cardsAreStable(Rig& rig) {
    for (const auto* file : {"tiny.csv", "wide-short.csv", "data.csv", "pack.zip"}) {
      rig.show(file);
      for (int width = 200; width <= 480; width += 20) {
        for (const float scale : kScales) {
          (void)rig.settle(width, kHeight, scale);
          const float first = placed(rig).card->height();
          for (int i = 0; i < 3; ++i) {
            (void)rig.frame(width, kHeight, scale);
          }
          TEST_NEAR(placed(rig).card->height(), first, 0.01F);
        }
      }
    }
  }

  // Each file gets the height of its own rows, whatever was shown before it, and the preview size and a changed file
  // are followed.
  void nothingIsStale(Rig& rig) {
    rig.show("tiny.csv");
    (void)rig.settle(262, kHeight, 1.0F);
    const float tiny = placed(rig).card->height();
    for (const auto* file : {"data.csv", "tiny.csv", "pack.zip", "large.zip", "style.css", "tiny.csv", "data.tsv"}) {
      rig.show(file);
      (void)rig.settle(262, kHeight, 1.0F);
      if (std::string_view(file) == "style.css") {
        continue;
      }
      if (std::string_view(file) == "tiny.csv") {
        TEST_NEAR(placed(rig).card->height(), tiny, 0.01F);
      }
      if (std::string_view(file) == "data.csv" || std::string_view(file) == "large.zip") {
        fillsTheRoom(rig, file);
      } else {
        hugsItsRows(rig, file);
      }
    }
    rig.show("tiny.csv");
    (void)rig.settle(262, kHeight, 1.0F);
    rig.preview->setPreviewSize(150);
    (void)rig.settle(262, kHeight, 1.0F);
    hugsItsRows(rig, "tiny.csv at 150%");
    const float larger = placed(rig).card->height();
    TEST_CHECK(larger > tiny + 15.0F);
    rig.preview->setPreviewSize(100);
    (void)rig.settle(262, kHeight, 1.0F);
    TEST_NEAR(placed(rig).card->height(), tiny, 0.01F);
    hugsItsRows(rig, "tiny.csv back at 100%");
  }

  void aChangedFileIsFollowed(Rig& rig) {
    rig.show("tiny.csv");
    (void)rig.settle(262, kHeight, 1.0F);
    const float before = placed(rig).card->height();
    std::ofstream(rig.files.folder() / "tiny.csv", std::ios::app) << "epsilon.txt,9\nzeta.txt,10\neta.txt,11\n";
    bool grown = false;
    for (int round = 0; round < 400 && !grown; ++round) {
      Rig::pump();
      rig.show("tiny.csv");
      (void)rig.frame(262, kHeight, 1.0F);
      const auto* table = dynamic_cast<const TableView*>(rig.view->content());
      grown = table != nullptr && table->table() != nullptr && table->table()->rowCount == 7;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    TEST_CHECK(grown);
    (void)rig.settle(262, kHeight, 1.0F);
    TEST_CHECK(placed(rig).card->height() > before + 40.0F);
    hugsItsRows(rig, "tiny.csv after it changed");
  }

  void cardsHaveWholeOutlines(Rig& rig) {
    for (const auto* file : {"style.css", "data.csv", "pack.zip", "blob.bin"}) {
      rig.show(file);
      for (const int width : {190, 262, 460}) {
        for (const float scale : {1.0F, 1.5F}) {
          const auto pixels = rig.settle(width, kHeight, scale);
          const Node* card = rig.view->content()->parent();
          TEST_CHECK(card != nullptr);
          outlineIsWhole(pixels, *card, scale, std::string(file) + " in " + std::to_string(width));
        }
      }
    }
  }
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (!args.empty() && args.front() == "--sheet-preview-worker") {
    return luciole::runSheetPreviewWorker(std::span(args).subspan(1));
  }
  if (!args.empty() && args.front() == "--mesh-preview-worker") {
    return luciole::runMeshPreviewWorker(std::span(args).subspan(1));
  }
  if (!args.empty() && args.front() == "--thumbnail-worker") {
    return luciole::runThumbnailWorker(std::span(args).subspan(1));
  }
  if (!args.empty()) {
    return 2;
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  static const luciole::test::preview::PreviewFiles files;
  Rig rig(*capture, files);
  cardsHaveWholeOutlines(rig);
  smallTablesAndListingsAreHugged(rig);
  largeTablesAndListingsFillTheRoom(rig);
  wideTablesStillScroll(rig);
  cardsAreStable(rig);
  nothingIsStale(rig);
  aChangedFileIsFollowed(rig);
  std::println("column-preview-card: all checks passed");
  return 0;
}
