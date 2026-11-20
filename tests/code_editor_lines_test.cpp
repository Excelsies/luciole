#include "imaging/text_tile_renderer.h"
#include "test_check.h"
#include "ui/controls/code_editor_lines.h"

#include <cstdint>
#include <print>
#include <string>
#include <vector>

namespace {
  using namespace luciole;
  using code_editor::StripWindow;
  using code_editor::stripWindow;
  using formats::StyledLine;
  using formats::StyleRole;

  void testWindows() {
    TEST_CHECK(stripWindow(0, 0, 400) == StripWindow{});
    TEST_CHECK(stripWindow(-5, 0, 400) == StripWindow{});
    TEST_CHECK((stripWindow(300, 0, 400) == StripWindow{.x0 = 0, .width = 300}));
    TEST_CHECK((stripWindow(kMaxTextTileWidth, 9000, 400) == StripWindow{.x0 = 0, .width = kMaxTextTileWidth}));
    // A wider line gets the widest texture centered on the viewport, inside the line.
    constexpr int kWide = 100000;
    TEST_CHECK((stripWindow(kWide, 0, 400) == StripWindow{.x0 = 0, .width = kMaxTextTileWidth}));
    const auto middle = stripWindow(kWide, 50000, 400);
    TEST_CHECK(middle.width == kMaxTextTileWidth && middle.x0 < 50000 && middle.x0 + middle.width > 50400);
    TEST_CHECK(middle.x0 == 50200 - (kMaxTextTileWidth / 2));
    TEST_CHECK(
        (stripWindow(kWide, kWide, 400) == StripWindow{.x0 = kWide - kMaxTextTileWidth, .width = kMaxTextTileWidth})
    );
    TEST_CHECK(stripWindow(kWide, -300, -20).x0 == 0);
  }

  void testImages() {
    // Opaque, clear, half covered white and a quarter covered red, in a strip two pixels wide and two high.
    const TextStrip strip{
        .x0 = 0, .width = 2, .height = 2, .rgba = {10, 20, 30, 255, 0, 0, 0, 0, 128, 128, 128, 128, 64, 0, 0, 64}
    };
    const auto image = code_editor::imageOf(strip);
    TEST_CHECK(code_editor::kStripBorder == 1 && image.size() == 4 * 4 * 4);
    const auto pixel = [&](int x, int y) {
      const auto at = static_cast<std::size_t>((y * 4) + x) * 4;
      return std::vector<std::uint8_t>(
          image.begin() + static_cast<std::ptrdiff_t>(at), image.begin() + static_cast<std::ptrdiff_t>(at + 4)
      );
    };
    TEST_CHECK((pixel(1, 1) == std::vector<std::uint8_t>{10, 20, 30, 255}));
    TEST_CHECK((pixel(2, 1) == std::vector<std::uint8_t>{0, 0, 0, 0}));
    TEST_CHECK((pixel(1, 2) == std::vector<std::uint8_t>{255, 255, 255, 128}));
    TEST_CHECK((pixel(2, 2) == std::vector<std::uint8_t>{255, 0, 0, 64}));
    // The frame is clear all round.
    for (int i = 0; i < 4; ++i) {
      TEST_CHECK(pixel(i, 0)[3] == 0 && pixel(i, 3)[3] == 0 && pixel(0, i)[3] == 0 && pixel(3, i)[3] == 0);
    }
    // More color than coverage stays within a byte.
    const auto bright = code_editor::imageOf({.x0 = 0, .width = 1, .height = 1, .rgba = {200, 0, 0, 100}});
    TEST_CHECK(bright.size() == 36 && bright[16] == 255 && bright[19] == 100);
    TEST_CHECK(code_editor::imageOf({.x0 = 0, .width = 2, .height = 1, .rgba = {1, 2, 3, 4}}).empty());
    TEST_CHECK(code_editor::imageOf(TextStrip{}).empty());
  }

  StyledLine lineOf(std::vector<formats::StyledRun> runs) {
    StyledLine line;
    line.runs = std::move(runs);
    line.swatches = {{.color = {}, .column = 2}, {.color = {}, .column = 6}};
    return line;
  }

  std::string joined(const StyledLine& line) {
    std::string out;
    for (const auto& run : line.runs) {
      out += run.text;
    }
    return out;
  }

  void testComposedText() {
    const auto line =
        lineOf({{.text = "ab\u00E9", .role = StyleRole::Keyword}, {.text = "cdef", .role = StyleRole::String}});
    const auto inside = code_editor::withText(line, 2, "XY");
    TEST_CHECK(joined(inside) == "abXY\u00E9cdef" && inside.runs.size() == 4);
    TEST_CHECK(inside.runs[0].role == StyleRole::Keyword && inside.runs[1].role == StyleRole::Plain);
    TEST_CHECK(inside.runs[2].text == "\u00E9" && inside.runs[2].role == StyleRole::Keyword);
    TEST_CHECK(inside.swatches[0].column == 4 && inside.swatches[1].column == 8);
    const auto boundary = code_editor::withText(line, 3, "Z");
    TEST_CHECK(joined(boundary) == "ab\u00E9Zcdef" && boundary.runs[1].text == "Z");
    TEST_CHECK(boundary.swatches[0].column == 2 && boundary.swatches[1].column == 7);
    const auto past = code_editor::withText(line, 40, "end");
    TEST_CHECK(joined(past) == "ab\u00E9cdefend" && past.runs.back().role == StyleRole::Plain);
    TEST_CHECK(past.swatches[1].column == 6);
    const auto empty = code_editor::withText(StyledLine{}, 0, "k");
    TEST_CHECK(joined(empty) == "k" && empty.runs.size() == 1);
    TEST_CHECK(code_editor::withText(line, 1, "").runs.size() == 2);
  }
} // namespace

int main() {
  testWindows();
  testImages();
  testComposedText();
  std::println("code-editor-lines: all checks passed");
  return 0;
}
