#include "formats/table_grid.h"
#include "png_writer.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "table_view_rig.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/table_cell_editor.h"
#include "ui/controls/table_edit_bar.h"
#include "ui/controls/table_view.h"
#include "ui/palette.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  using namespace luciole;
  using ::test::GlPixels;

  constexpr int kWidth = 520;
  constexpr int kHeight = 260;
  constexpr std::array<float, 2> kScales{1.0F, 1.5F};

  struct Region {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
  };

  // A node's rectangle in buffer pixels, inset by `inset` logical pixels.
  Region regionOf(const Node& node, float scale, float inset = 0) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    return {
        .left = static_cast<int>(std::ceil((x + inset) * scale)),
        .top = static_cast<int>(std::ceil((y + inset) * scale)),
        .right = static_cast<int>(std::floor((x + node.width() - inset) * scale)),
        .bottom = static_cast<int>(std::floor((y + node.height() - inset) * scale)),
    };
  }

  std::uint32_t packed(std::array<std::uint8_t, 4> p) {
    return (std::uint32_t{p[0]} << 24U) | (std::uint32_t{p[1]} << 16U) | (std::uint32_t{p[2]} << 8U) | p[3];
  }

  std::uint32_t packed(const Color& color) {
    const auto byte = [](float value) { return static_cast<std::uint8_t>(std::lround(value * 255.0F)); };
    return packed(std::array<std::uint8_t, 4>{byte(color.r), byte(color.g), byte(color.b), 255});
  }

  // The most common color of a region and the share of its pixels that differ from it.
  std::pair<std::uint32_t, double> background(const GlPixels& pixels, Region region) {
    std::unordered_map<std::uint32_t, std::size_t> counts;
    std::size_t total = 0;
    for (int y = std::max(region.top, 0); y < std::min(region.bottom, pixels.height); ++y) {
      for (int x = std::max(region.left, 0); x < std::min(region.right, pixels.width); ++x) {
        ++counts[packed(pixels.at(x, y))];
        ++total;
      }
    }
    std::pair<std::uint32_t, std::size_t> most{0, 0};
    for (const auto& [rgba, count] : counts) {
      most = count > most.second ? std::pair{rgba, count} : most;
    }
    return {most.first, total == 0 ? 0 : static_cast<double>(total - most.second) / static_cast<double>(total)};
  }

  // The brightest channel sum of a region, against its most common color: how strongly something is drawn on it.
  int contrast(const GlPixels& pixels, Region region) {
    const std::uint32_t plain = background(pixels, region).first;
    const auto sum = [](std::uint32_t rgba) {
      return static_cast<int>((rgba >> 24U) & 0xFFU)
          + static_cast<int>((rgba >> 16U) & 0xFFU)
          + static_cast<int>((rgba >> 8U) & 0xFFU);
    };
    int most = 0;
    for (int y = std::max(region.top, 0); y < std::min(region.bottom, pixels.height); ++y) {
      for (int x = std::max(region.left, 0); x < std::min(region.right, pixels.width); ++x) {
        most = std::max(most, std::abs(sum(packed(pixels.at(x, y))) - sum(plain)));
      }
    }
    return most;
  }

  void keep(const GlPixels& pixels, std::string_view name, float scale) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / std::format("table-edit-{}-{}x.png", name, scale);
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  // Updates and draws until the view asks for no more frames; returns the last capture.
  GlPixels settle(TableView& view, ::test::SceneCapture& capture, float scale, bool& frame) {
    GlPixels pixels;
    for (int pass = 0; pass < 8; ++pass) {
      frame = false;
      view.update();
      pixels = capture.capture(view, kWidth, kHeight, scale);
      if (!frame) {
        return pixels;
      }
    }
    TEST_CHECK(!frame);
    return pixels;
  }

  std::shared_ptr<const formats::TableGrid> inventory() {
    const std::array<std::array<std::string_view, 4>, 6> rows{{
        {"Desk lamp", "Office", "12", "34.50"},
        {"Notebook, A5", "Office", "240", "2.25"},
        {"Chair", "Furniture", "8", "89.00"},
        {"Monitor arm", "Office", "5", "45.75"},
        {"Tea", "Kitchen", "60", "4.10"},
        {"Kettle", "Kitchen", "3", "29.90"},
    }};
    return luciole::test::tableview::grid(
        rows.size(), 4, [&](std::size_t row, std::size_t column) { return std::string(rows[row][column]); },
        [](formats::TableGrid& table) {
          table.columnNames = {"Item", "Category", "Quantity", "Unit price"};
          table.firstSourceRow = 1;
        }
    );
  }

  void capturesEditing(::test::SceneCapture& capture) {
    for (const float scale : kScales) {
      TableView view;
      InputDispatcher dispatcher;
      dispatcher.setSceneRoot(&view);
      bool frame = false;
      view.onFrameNeeded = [&] { frame = true; };
      view.onFocus = [&](InputArea* area) { dispatcher.setFocus(area); };
      view.setEditable(true);
      view.setTable(inventory());
      view.cellText = [&view](TableCell source) {
        return std::string(formats::gridCellText(*view.table(), source.row - 1, source.column));
      };
      view.setEditAvailability({.undo = true, .redo = false});
      view.select({.row = 1, .column = 0});
      auto pixels = settle(view, capture, scale, frame);
      keep(pixels, "bar", scale);
      // The bar is drawn above the grid; enabled icons stand out more than the disabled redo.
      const TableEditBar& bar = view.editBar();
      float barX = 0;
      float barY = 0;
      Node::absolutePosition(&bar, barX, barY);
      float canvasX = 0;
      float canvasY = 0;
      Node::absolutePosition(&view.canvas(), canvasX, canvasY);
      TEST_CHECK(barY + bar.height() <= canvasY + 0.5F);
      const int undo = contrast(pixels, regionOf(*bar.button(TableEdit::Undo).glyph(), scale));
      const int redo = contrast(pixels, regionOf(*bar.button(TableEdit::Redo).glyph(), scale));
      const int insert = contrast(pixels, regionOf(*bar.button(TableEdit::InsertRowBelow).glyph(), scale));
      std::println("{}x icon contrast: insert row {}, undo {}, redo (disabled) {}", scale, insert, undo, redo);
      TEST_CHECK(insert > 150 && undo > 150);
      TEST_CHECK(redo > 0 && redo < undo);
      TEST_CHECK(bar.button(TableEdit::Undo).glyph()->height() <= view.textSize() + 0.5F);

      // F2 opens the field over the selected cell, filled with the surface and showing the cell's text.
      dispatcher.setFocus(&view.canvas());
      TEST_CHECK(view.offerKey({.sym = XKB_KEY_F2, .pressed = true}));
      pixels = settle(view, capture, scale, frame);
      keep(pixels, "field", scale);
      const Input& field = view.canvas().editor().field();
      TEST_CHECK(view.editingCell() && field.value() == "Notebook, A5");
      const auto inside = background(pixels, regionOf(field, scale, 3));
      std::println("{}x field: {:08x} fill, {:.3f} drawn on it", scale, inside.first, inside.second);
      TEST_CHECK(inside.first == packed(resolveColorSpec(colorSpecFromRole(ColorRole::Surface))));
      TEST_CHECK(inside.second > 0.05);
      // Beside the field the next cell keeps the grid's fill.
      float fieldX = 0;
      float fieldY = 0;
      Node::absolutePosition(&field, fieldX, fieldY);
      const auto beside = pixels.at(
          static_cast<int>(std::lround((fieldX + field.width() + 4) * scale)),
          static_cast<int>(std::lround((fieldY + 3) * scale))
      );
      TEST_CHECK(packed(beside) == packed(resolveColorSpec(colorSpecFromRole(ColorRole::SurfaceContainerLow))));
      // Typed text shows in the field.
      for (const char c : std::string_view(" B5")) {
        TEST_CHECK(view.offerKey(
            {.sym = static_cast<std::uint32_t>(c), .utf32 = static_cast<std::uint32_t>(c), .pressed = true}
        ));
      }
      pixels = settle(view, capture, scale, frame);
      keep(pixels, "typed", scale);
      TEST_CHECK(field.value() == "Notebook, A5 B5");
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
  capturesEditing(*capture);
  std::println("table-view-editing-pixels: all checks passed");
  return 0;
}
