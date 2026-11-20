#include "text_tile_view_rig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole;
  using namespace luciole::test::tiles;
  using ::test::GlPixels;

  constexpr std::array<float, 2> kScales{1.0F, 1.5F};

  struct Bounds {
    int left = 1 << 30;
    int top = 1 << 30;
    int right = -1;
    int bottom = -1;
    std::size_t count = 0;

    void add(int x, int y) {
      left = std::min(left, x);
      top = std::min(top, y);
      right = std::max(right, x);
      bottom = std::max(bottom, y);
      ++count;
    }
    int width() const { return right - left + 1; }
    int height() const { return bottom - top + 1; }
  };

  // The pixels within `tolerance` of a color, and their bounds.
  Bounds find(const GlPixels& pixels, const Pixel& color, int tolerance = 0) {
    Bounds box;
    for (int y = 0; y < pixels.height; ++y) {
      for (int x = 0; x < pixels.width; ++x) {
        if (distance(pixels.at(x, y), color) <= tolerance) {
          box.add(x, y);
        }
      }
    }
    return box;
  }

  // Rows holding ink, grouped into runs: the text lines.
  std::vector<Bounds> lines(const GlPixels& pixels) {
    std::vector<Bounds> out;
    bool open = false;
    for (int y = 0; y < pixels.height; ++y) {
      const bool ink = inkIn(pixels, 0, pixels.width, y, y) > 0;
      if (ink && !open) {
        out.emplace_back();
      }
      if (ink) {
        for (int x = 0; x < pixels.width; ++x) {
          if (inkIn(pixels, x, x + 1, y, y) > 0) {
            out.back().add(x, y);
          }
        }
      }
      open = ink;
    }
    return out;
  }

  GlPixels shown(::test::SceneCapture& capture, std::string text, std::string_view name, float scale, int height) {
    Rig rig(capture);
    rig.width = 420;
    rig.height = height;
    rig.scale = scale;
    rig.view->show(textOf(std::move(text), name));
    auto pixels = rig.settle();
    TEST_CHECK(rig.view->message().empty());
    TEST_CHECK(pixels.width == static_cast<int>(std::lround(420 * scale)));
    return pixels;
  }

  // Each color code gets a square swatch of its color, inside its line, with the code drawn right after it.
  void testCssSwatches(::test::SceneCapture& capture, float scale) {
    const auto pixels = shown(
        capture, ".red { color: #ff0000; }\n.green { background: #00c000; }\n.blue { border-color: #0000ff; }\n",
        "style.css", scale, 120
    );
    keep(pixels, "css", scale);
    int previousTop = -1;
    for (const auto rgb : {0xFF0000U, 0x00C000U, 0x0000FFU}) {
      const auto swatch = find(pixels, bytesOf(rgbHex(rgb)));
      std::println(
          "{}x #{:06x}: {} px at {},{} size {}x{}", scale, rgb, swatch.count, swatch.left, swatch.top, swatch.width(),
          swatch.height()
      );
      const double side = 6.5 * scale; // the fill inside the swatch border
      TEST_CHECK(static_cast<double>(swatch.count) >= side * side);
      TEST_CHECK(std::abs(swatch.width() - swatch.height()) <= 1);
      TEST_CHECK(swatch.top > previousTop);
      previousTop = swatch.bottom;
      const int span = 6 * swatch.width();
      TEST_CHECK(inkIn(pixels, swatch.right + 2, swatch.right + 2 + span, swatch.top, swatch.bottom) > 20);
      TEST_CHECK(inkIn(pixels, 0, swatch.left - 1, swatch.top, swatch.bottom) > 20);
    }
  }

  // A heading, then a table on a grid, then a code block on its own background, top down.
  void testMarkdownBlocks(::test::SceneCapture& capture, float scale) {
    const auto pixels = shown(
        capture,
        "# Heading\n\nSome *prose* that wraps at the width of the view when it runs long enough to need it.\n\n"
        "| Name | Size |\n|------|-----:|\n| a | 1 |\n| b | 22 |\n\n```cpp\nint main() { return 0; }\n```\n",
        "notes.md", scale, 360
    );
    keep(pixels, "markdown", scale);
    const auto heading = find(pixels, bytesOf(palette.primary), 60);
    const auto grid = find(pixels, bytesOf(palette.outlineVariant));
    const auto code = find(pixels, bytesOf(palette.surfaceContainerHigh));
    std::println(
        "{}x heading {} px from row {}, grid {} px rows {}-{}, code background {} px rows {}-{}", scale, heading.count,
        heading.top, grid.count, grid.top, grid.bottom, code.count, code.top, code.bottom
    );
    TEST_CHECK(heading.count >= 20);
    TEST_CHECK(grid.count >= static_cast<std::size_t>(200 * scale));
    TEST_CHECK(code.count >= static_cast<std::size_t>(2000 * scale * scale));
    TEST_CHECK(heading.top < grid.top && grid.bottom < code.top);
    // The code is drawn on its background.
    TEST_CHECK(inkIn(pixels, code.left, code.right, code.top, code.bottom) > code.count / 2);
  }

  // Printed with indentation over several lines, keys, strings and numbers each in their color.
  void testJsonPrinted(::test::SceneCapture& capture, float scale) {
    const auto pixels = shown(
        capture, R"({"name":"luciole","sizes":[1,22,333],"nested":{"ok":true,"none":null}})", "data.json", scale, 360
    );
    keep(pixels, "json", scale);
    const auto rows = lines(pixels);
    std::println("{}x json: {} lines", scale, rows.size());
    TEST_CHECK(rows.size() >= 10);
    TEST_CHECK(rows[1].left >= rows[0].left + static_cast<int>(8 * scale));
    TEST_CHECK(rows[3].left >= rows[1].left + static_cast<int>(8 * scale)); // an element of "sizes"
    TEST_CHECK(find(pixels, bytesOf(palette.primary), 60).count > 5);       // keys
    TEST_CHECK(find(pixels, bytesOf(palette.tertiary), 60).count > 5);      // strings
    TEST_CHECK(find(pixels, bytesOf(palette.secondary), 60).count > 5);     // numbers and literals
  }

  std::string proseText() {
    std::string out;
    for (int paragraph = 0; paragraph < 4; ++paragraph) {
      out += std::format("Paragraph {} opens plainly. ", paragraph);
      for (int w = 0; w < 18; ++w) {
        out += "Words follow words until the line has to turn. ";
      }
      out += "\n\n";
    }
    out += "A word wider than the view: " + std::string(90, 'W') + "\nlast line\n";
    return out;
  }

  // Plain prose wraps at word boundaries inside the view at two widths and two sizes: nothing scrolls sideways,
  // nothing is drawn past the right padding, a narrower view and larger text make the text taller, and the end of
  // the text, a word wider than the view included, is reached by scrolling down.
  void testPlainProse(::test::SceneCapture& capture, float scale) {
    Rig rig(capture);
    rig.scale = scale;
    rig.height = 520;
    rig.view->show(textOf(proseText(), "notes.txt"));
    int previousHeight = 0;
    for (const float size : {12.0F, 15.0F}) {
      rig.view->setTextSize(size);
      int wideHeight = 0;
      for (const int width : {440, 300}) {
        rig.width = width;
        auto pixels = rig.settle();
        TEST_CHECK(rig.view->message().empty());
        keep(pixels, std::format("prose-{}-{}", width, static_cast<int>(size)), scale);
        TEST_CHECK(rig.view->maxScrollX() == 0 && !rig.view->horizontalBar().Node::visible());
        const int edge = static_cast<int>(std::lround(width * scale));
        TEST_CHECK(rig.view->geometry().width == edge);
        const int gap = static_cast<int>(std::ceil(edge - (7.0F * scale)));
        const int bar = edge - static_cast<int>(6 * scale);
        TEST_CHECK(inkIn(pixels, gap, bar, 0, pixels.height - 1) == 0);
        TEST_CHECK(inkIn(pixels, static_cast<int>(8 * scale), static_cast<int>(40 * scale), 0, 40) > 20);
        const int tall = rig.view->geometry().height;
        std::println("{}x prose width {} size {}: {} px tall", scale, width, size, tall);
        TEST_CHECK(rig.view->geometry().lineTops.size() == 10);
        if (width == 440) {
          wideHeight = tall;
        } else {
          TEST_CHECK(tall * 10 > wideHeight * 12);
        }
        // The end of the text: the wide word's rows and the last line, all inside the right padding.
        rig.view->scrollTo(0, rig.view->maxScrollY());
        pixels = rig.settle();
        keep(pixels, std::format("prose-end-{}-{}", width, static_cast<int>(size)), scale);
        TEST_CHECK(inkIn(pixels, gap, bar, 0, pixels.height - 1) == 0);
        TEST_CHECK(inkIn(pixels, static_cast<int>(8 * scale), edge - static_cast<int>(14 * scale), 0, 60) > 100);
        rig.view->scrollTo(0, 0);
      }
      if (previousHeight != 0) {
        TEST_CHECK(rig.view->geometry().height > previousHeight);
      }
      previousHeight = rig.view->geometry().height;
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
  setPalette(testPalette());
  for (const float scale : kScales) {
    testCssSwatches(*capture, scale);
    testMarkdownBlocks(*capture, scale);
    testJsonPrinted(*capture, scale);
    testPlainProse(*capture, scale);
  }
  std::println("text-tile-view-pixels: all checks passed");
  return 0;
}
