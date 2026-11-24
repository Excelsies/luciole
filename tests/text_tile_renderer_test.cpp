#include "cpu_budget.h"
#include "formats/styled_text.h"
#include "imaging/text_tile_renderer.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <format>
#include <functional>
#include <latch>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using namespace luciole;
  using formats::StyleRole;
  using Pixel = std::array<std::uint8_t, 4>;

  constexpr Pixel kBlack{0, 0, 0, 255};
  constexpr Pixel kCodeBackground{0x33, 0x33, 0x33, 255};
  constexpr Pixel kOutline{0x80, 0x80, 0x80, 255};

  Color colorOf(const Pixel& pixel) { return rgbHex((pixel[0] << 16U) | (pixel[1] << 8U) | pixel[2]); }

  TextTileTheme theme() {
    TextTileTheme out;
    out.roles.fill(rgbHex(0xFFFFFF));
    out.roles[static_cast<std::size_t>(StyleRole::Keyword)] = rgbHex(0xFF0000);
    out.roles[static_cast<std::size_t>(StyleRole::Comment)] = rgbHex(0x00FF00);
    out.roles[static_cast<std::size_t>(StyleRole::String)] = rgbHex(0x0000FF);
    out.background = colorOf(kBlack);
    out.codeBackground = colorOf(kCodeBackground);
    out.outline = colorOf(kOutline);
    out.checkerLight = rgbHex(0xC0C0C0);
    out.checkerDark = rgbHex(0x404040);
    return out;
  }

  std::shared_ptr<const formats::StyledDocument>
  document(std::string_view text, std::string_view name, const formats::StyledTextLimits& limits = {}) {
    return std::make_shared<const formats::StyledDocument>(formats::styledText(text, formats::fileKind(name), limits));
  }

  TextTileOptions options(double width = 400.0, double scale = 1.0) {
    return {.width = width, .scale = scale, .theme = theme(), .fonts = {}};
  }

  std::string repeat(std::string_view piece, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
      out += piece;
    }
    return out;
  }

  // Every tile of a layout, read by document pixel.
  struct Canvas {
    std::vector<TextTile> tiles;
    int width = 0;
    int height = 0;

    explicit Canvas(TextTileLayout& layout) : width(layout.width()), height(layout.height()) {
      for (std::size_t k = 0; k < layout.tileCount(); ++k) {
        auto tile = layout.renderTile(k);
        TEST_CHECK(tile.has_value());
        tiles.push_back(std::move(*tile));
      }
    }

    Pixel at(int x, int y) const {
      TEST_CHECK(x >= 0 && x < width && y >= 0 && y < height);
      const auto& tile = tiles[static_cast<std::size_t>(y / kTextTileHeight)];
      const auto index = ((static_cast<std::size_t>(y - tile.top) * tile.width) + x) * 4;
      return {tile.rgba[index], tile.rgba[index + 1], tile.rgba[index + 2], tile.rgba[index + 3]};
    }

    // Pixels in the box that match.
    std::size_t count(const TextBox& box, const std::function<bool(const Pixel&)>& match) const {
      std::size_t found = 0;
      const int x0 = std::max(0, static_cast<int>(std::floor(box.x)));
      const int y0 = std::max(0, static_cast<int>(std::floor(box.y)));
      const int x1 = std::min(width, static_cast<int>(std::ceil(box.x + box.width)));
      const int y1 = std::min(height, static_cast<int>(std::ceil(box.y + box.height)));
      for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
          found += match(at(x, y)) ? 1 : 0;
        }
      }
      return found;
    }
  };

  bool reddish(const Pixel& p) { return p[0] > 120 && p[1] == 0 && p[2] == 0; }
  bool greenish(const Pixel& p) { return p[1] > 120 && p[0] == 0 && p[2] == 0; }
  bool anyRed(const Pixel& p) { return p[0] > 0 && p[1] == 0; }
  bool anyGreen(const Pixel& p) { return p[1] > 0 && p[0] == 0; }
  bool ink(const Pixel& p) { return p != kBlack && p != kCodeBackground; }

  TextBox box(TextTileLayout& layout, const TextPosition& position) {
    const auto found = layout.characterBox(position);
    TEST_CHECK(found.has_value());
    return *found;
  }

  // The boxes of columns [from, to) of a line, joined.
  TextBox span(TextTileLayout& layout, std::size_t block, std::size_t line, std::size_t from, std::size_t to) {
    const auto first = box(layout, {.block = block, .line = line, .column = from});
    const auto last = box(layout, {.block = block, .line = line, .column = to - 1});
    return {.x = first.x, .y = first.y, .width = last.x + last.width - first.x, .height = first.height};
  }

  void testKeywordAndCommentColors() {
    TextTileLayout layout(document("int x; // note", "a.c"), options());
    const Canvas canvas(layout);
    const auto keyword = span(layout, 0, 0, 0, 3);
    TEST_CHECK(canvas.count(keyword, reddish) > 0);
    TEST_CHECK(canvas.count(keyword, anyGreen) == 0);
    const auto comment = span(layout, 0, 0, 7, 14);
    TEST_CHECK(canvas.count(comment, greenish) > 0);
    TEST_CHECK(canvas.count(comment, anyRed) == 0);
    const auto plain = span(layout, 0, 0, 4, 5);
    TEST_CHECK(canvas.count(plain, ink) > 0);
    TEST_CHECK(canvas.count(plain, anyRed) == 0);
  }

  void testOpaqueSwatch() {
    TextTileLayout layout(document("a { color: #12ab34; }", "a.css"), options());
    const auto tile = layout.renderTile(0);
    TEST_CHECK(tile.has_value());
    TEST_CHECK(tile->swatches.size() == 1);
    const auto swatch = tile->swatches[0];
    TEST_CHECK((swatch.color == formats::Rgba{.r = 0x12, .g = 0xab, .b = 0x34, .a = 255}));
    const double em = TextTileFonts{}.codeSize;
    TEST_CHECK(swatch.size >= static_cast<int>(0.6 * em) && swatch.size <= static_cast<int>(std::ceil(em)));
    const Canvas canvas(layout);
    const TextBox inner{
        .x = swatch.x + 1.0, .y = swatch.y + 1.0, .width = swatch.size - 2.0, .height = swatch.size - 2.0
    };
    const Pixel exact{0x12, 0xab, 0x34, 255};
    const auto area = static_cast<std::size_t>(inner.width * inner.height);
    TEST_CHECK(canvas.count(inner, [&](const Pixel& p) { return p == exact; }) == area);
    TEST_CHECK(canvas.at(swatch.x, swatch.y) == kOutline);
    TEST_CHECK(canvas.at(swatch.x + swatch.size - 1, swatch.y + swatch.size - 1) == kOutline);
    // The swatch sits before the code and covers none of its text.
    const auto code = box(layout, {.block = 0, .line = 0, .column = 11});
    TEST_CHECK(swatch.x + swatch.size <= code.x);
    const auto before = box(layout, {.block = 0, .line = 0, .column = 10});
    TEST_CHECK(before.x + before.width <= swatch.x);
    TEST_CHECK(swatch.y >= code.y && swatch.y + swatch.size <= code.y + code.height);
  }

  void testTranslucentSwatch() {
    TextTileLayout layout(document("a { color: rgba(255, 0, 0, 0.25); }", "a.css"), options(400.0, 2.0));
    const Canvas canvas(layout);
    const auto& swatches = canvas.tiles[0].swatches;
    TEST_CHECK(swatches.size() == 1);
    const auto s = swatches[0];
    std::vector<Pixel> seen;
    for (int y = s.y + 2; y < s.y + s.size - 2; ++y) {
      for (int x = s.x + 2; x < s.x + s.size - 2; ++x) {
        const auto pixel = canvas.at(x, y);
        if (std::ranges::find(seen, pixel) == seen.end()) {
          seen.push_back(pixel);
        }
      }
    }
    // Light and dark cells, both tinted red.
    TEST_CHECK(seen.size() == 2);
    TEST_CHECK(seen[0][0] > seen[0][1] && seen[1][0] > seen[1][1]);
  }

  // The leftmost ink column in the band of a line.
  int leftInk(const Canvas& canvas, const TextBox& band) {
    for (int x = 0; x < canvas.width; ++x) {
      if (canvas.count({.x = static_cast<double>(x), .y = band.y, .width = 1.0, .height = band.height}, ink) > 0) {
        return x;
      }
    }
    return canvas.width;
  }

  void testJsonIndented() {
    TextTileLayout layout(document(R"({"key":{"inner":1}})", "a.json"), options());
    const Canvas canvas(layout);
    const auto top = leftInk(canvas, box(layout, {.block = 0, .line = 0, .column = 0}));
    const auto one = leftInk(canvas, box(layout, {.block = 0, .line = 1, .column = 0}));
    const auto two = leftInk(canvas, box(layout, {.block = 0, .line = 2, .column = 0}));
    const auto space = box(layout, {.block = 0, .line = 1, .column = 0}).width;
    TEST_CHECK(one - top >= static_cast<int>(1.5 * space));
    TEST_CHECK(two - one >= static_cast<int>(1.5 * space));
  }

  constexpr std::string_view kMarkdown = "# Head\n"
                                         "\n"
                                         "body text\n"
                                         "\n"
                                         "- item\n"
                                         "\n"
                                         "```\n"
                                         "code\n"
                                         "```\n"
                                         "\n"
                                         "| a | b |\n"
                                         "|---|---|\n"
                                         "| 1 | 2 |\n"
                                         "\n"
                                         "> quoted\n"
                                         "\n"
                                         "---\n";

  void testMarkdownBlocks() {
    TextTileLayout layout(document(kMarkdown, "a.md"), options());
    const Canvas canvas(layout);
    const auto heading = box(layout, {.block = 0, .line = 0, .column = 0});
    const auto body = box(layout, {.block = 1, .line = 0, .column = 0});
    // Headings stand out by weight, never by a size above the text's, so a preview stays below the list's size.
    TEST_CHECK(heading.height > 0 && heading.height <= body.height + 1.0);
    TEST_CHECK(heading.width > body.width);

    // The list item is indented and its marker drawn left of its text.
    const auto item = box(layout, {.block = 2, .line = 0, .column = 0});
    TEST_CHECK(item.x > body.x + body.width);
    TEST_CHECK(
        canvas.count({.x = body.x, .y = item.y, .width = item.x - body.x - 1.0, .height = item.height}, ink) > 0
    );

    // The code block sits on its background, beyond its text and around it.
    const auto code = span(layout, 3, 0, 0, 4);
    TEST_CHECK(
        canvas.at(static_cast<int>(code.x + code.width + 4.0), static_cast<int>(code.y + 2.0)) == kCodeBackground
    );
    TEST_CHECK(canvas.at(static_cast<int>(code.x), static_cast<int>(code.y) - 2) == kCodeBackground);
    TEST_CHECK(canvas.at(static_cast<int>(code.x) - 2, static_cast<int>(code.y + 2.0)) == kCodeBackground);

    // The table has a grid: a line left of the first cell's text and one between the header and the body.
    const auto header = box(layout, {.block = 4, .line = 0, .column = 0});
    const auto cell = box(layout, {.block = 4, .line = 2, .column = 0});
    const auto isOutline = [](const Pixel& p) { return p == kOutline; };
    TEST_CHECK(
        canvas.count({.x = body.x - 1.0, .y = header.y, .width = header.x - body.x, .height = header.height}, isOutline)
        >= static_cast<std::size_t>(header.height)
    );
    const TextBox between{
        .x = header.x, .y = header.y + header.height, .width = header.width, .height = cell.y - header.y - header.height
    };
    TEST_CHECK(between.height > 0.0);
    TEST_CHECK(canvas.count(between, isOutline) >= static_cast<std::size_t>(header.width));

    // The quote has a bar left of its text.
    const auto quoted = box(layout, {.block = 5, .line = 0, .column = 0});
    TEST_CHECK(canvas.count({.x = 0.0, .y = quoted.y, .width = quoted.x, .height = quoted.height}, isOutline) > 0);

    // The rule is a line below the quote, across most of the width.
    std::size_t widest = 0;
    for (int y = static_cast<int>(quoted.y + quoted.height); y < canvas.height; ++y) {
      widest = std::max(
          widest, canvas.count({.x = 0.0, .y = static_cast<double>(y), .width = 400.0, .height = 1.0}, isOutline)
      );
    }
    TEST_CHECK(widest > 300);
  }

  void testLongLineIsClipped() {
    const auto text = "int a;\n" + repeat("b", 3000);
    TextTileLayout layout(document(text, "a.c", {.columns = 5000}), options(300.0));
    TEST_CHECK(layout.width() == kMaxTextTileWidth);
    const Canvas canvas(layout);
    TEST_CHECK(canvas.tiles[0].width == kMaxTextTileWidth);
    // The mark, in the comment color, at the right end of the clipped line.
    const auto line = box(layout, {.block = 0, .line = 1, .column = 0});
    const TextBox end{.x = kMaxTextTileWidth - 40.0, .y = line.y, .width = 40.0, .height = line.height};
    TEST_CHECK(canvas.count(end, anyGreen) > 0);
    // A line cut by the column limit ends with the mark right after its text.
    TextTileLayout cut(document("abcdefghij", "a.c", {.columns = 4}), options());
    const Canvas cutCanvas(cut);
    const auto text4 = span(cut, 0, 0, 0, 4);
    const TextBox after{.x = text4.x + text4.width, .y = text4.y, .width = 20.0, .height = text4.height};
    TEST_CHECK(cutCanvas.count(after, anyGreen) > 0);
    TEST_CHECK(cutCanvas.count(text4, anyGreen) == 0);
    // A color code past the clip draws no swatch.
    TextTileLayout hidden(document("a { b: " + repeat("x", 700) + " #ff0000 }", "a.css"), options());
    TEST_CHECK(hidden.width() == kMaxTextTileWidth);
    TEST_CHECK(hidden.renderTile(0)->swatches.empty());
  }

  void testTablesFitAndAlign() {
    const auto row = std::string{"| "} + repeat("long cell text here | ", 5) + "\n";
    const auto wide = document(row + "|---|---|---|---|---|\n" + row, "a.md");
    TextTileLayout layout(wide, options(240.0));
    TEST_CHECK(layout.width() == 240);
    const Canvas canvas(layout);
    const auto first = box(layout, {.block = 0, .line = 0, .column = 0});
    const auto last = box(layout, {.block = 0, .line = 0, .column = 18});
    TEST_CHECK(last.y > first.y);
    TEST_CHECK(canvas.count({.x = 236.0, .y = 0.0, .width = 4.0, .height = 1.0 * layout.height()}, ink) == 0);

    TextTileLayout aligned(document("| left | right |\n|:--|--:|\n| x | y |\n", "a.md"), options());
    const auto leftHead = box(aligned, {.block = 0, .line = 0, .column = 0});
    const auto leftCell = box(aligned, {.block = 0, .line = 2, .column = 0});
    const auto rightHead = box(aligned, {.block = 0, .line = 1, .column = 0});
    const auto rightCell = box(aligned, {.block = 0, .line = 3, .column = 0});
    TEST_NEAR(leftCell.x, leftHead.x, 0.5);
    TEST_CHECK(rightCell.x > rightHead.x + rightHead.width);
  }

  void testWideTablesWiden() {
    std::string head = "|";
    std::string rule = "|";
    std::string row = "|";
    for (int c = 0; c < 30; ++c) {
      head += " h" + std::to_string(c) + " |";
      rule += "---|";
      row += " v" + std::to_string(c) + " |";
    }
    const auto doc = document(head + "\n" + rule + "\n" + row + "\n", "a.md");
    TextTileLayout layout(doc, options(320.0));
    TEST_CHECK(layout.width() > 320);
    TEST_CHECK(!layout.truncated());
    const auto last = span(layout, 0, 59, 0, 3);
    TEST_CHECK(last.x + last.width <= layout.width());
    const Canvas canvas(layout);
    TEST_CHECK(canvas.count(last, ink) > 0);

    // Too wide even at its narrowest: the layout stops at its widest and the table is cut at its edge.
    auto big = options(320.0);
    big.fonts.proseSize = 100.0;
    TextTileLayout cut(doc, big);
    TEST_CHECK(cut.width() == kMaxTextTileWidth);
    TEST_CHECK(cut.truncated());
    const Canvas cutCanvas(cut);
    TEST_CHECK(
        cutCanvas.count({.x = kMaxTextTileWidth - 1.0, .y = 0.0, .width = 1.0, .height = 1.0 * cut.height()}, ink) > 0
    );
  }

  // True when columns [from, to) of a line sit on one row of text.
  bool oneRow(TextTileLayout& layout, std::size_t block, std::size_t line, std::size_t from, std::size_t to) {
    const auto top = box(layout, {.block = block, .line = line, .column = from}).y;
    for (auto column = from + 1; column < to; ++column) {
      if (box(layout, {.block = block, .line = line, .column = column}).y != top) {
        return false;
      }
    }
    return true;
  }

  void testTableWordsStayWhole() {
    // A narrow column keeps its words while a column of long text has room to give.
    const auto notes = repeat("a longer note that wraps ", 8);
    const auto doc =
        document("| Mode | Shortcut | Notes |\n|:--|:--:|--:|\n| Workbench | Ctrl+4 | " + notes + "|\n", "a.md");
    for (const double width : {260.0, 320.0}) {
      TextTileLayout layout(doc, options(width));
      TEST_CHECK(layout.width() == static_cast<int>(width) && !layout.truncated());
      TEST_CHECK(oneRow(layout, 0, 1, 0, 8));
      TEST_CHECK(oneRow(layout, 0, 3, 0, 9));
      TEST_CHECK(!oneRow(layout, 0, 5, 0, notes.size() - 1));
    }

    // A word wider than the room breaks; the other column keeps its words and the table stays inside.
    const auto word = repeat("x", 120);
    const auto table = "| Key | Value |\n|---|---|\n| Name | " + word + " |\n";
    TextTileLayout narrow(document(table, "a.md"), options(320.0));
    TEST_CHECK(narrow.width() == 320 && !narrow.truncated());
    TEST_CHECK(oneRow(narrow, 0, 0, 0, 3) && oneRow(narrow, 0, 2, 0, 4));
    TEST_CHECK(!oneRow(narrow, 0, 3, 0, word.size()));
    const Canvas canvas(narrow);
    TEST_CHECK(canvas.count({.x = 313.0, .y = 0.0, .width = 7.0, .height = 1.0 * narrow.height()}, ink) == 0);

    // A layout already widened by code gives the word its room.
    const auto code = "```\n" + repeat("c", 90) + "\n```\n\n";
    const auto shortWord = repeat("x", 50);
    TextTileLayout wide(
        document(code + "| Key | Value |\n|---|---|\n| Name | " + shortWord + " |\n", "a.md"), options(320.0)
    );
    TEST_CHECK(wide.width() > 320 && !wide.truncated());
    TEST_CHECK(oneRow(wide, 1, 3, 0, shortWord.size()));
  }

  void testWideMarkersStayInside() {
    for (const auto* text : {"100. x\n", "123456. x\n"}) {
      TextTileLayout layout(document(text, "a.md"), options());
      const Canvas canvas(layout);
      TEST_CHECK(leftInk(canvas, box(layout, {.block = 0, .line = 0, .column = 0})) >= 4);
    }
    // Items of one list share the left edge of their text.
    TextTileLayout list(document("99. a\n100. b\n", "a.md"), options());
    const auto first = box(list, {.block = 0, .line = 0, .column = 0});
    TEST_NEAR(box(list, {.block = 1, .line = 0, .column = 0}).x, first.x, 0.5);
  }

  void testProseWraps() {
    const auto doc = document(repeat("words wrap here ", 40), "a.md");
    TextTileLayout wide(doc, options(600.0));
    TextTileLayout narrow(doc, options(200.0));
    TEST_CHECK(narrow.width() == 200);
    TEST_CHECK(wide.width() == 600);
    TEST_CHECK(narrow.height() > wide.height() * 2);
    const auto first = box(narrow, {.block = 0, .line = 0, .column = 0});
    const auto last = box(narrow, {.block = 0, .line = 0, .column = 600});
    TEST_CHECK(last.y > first.y + first.height);
    const Canvas canvas(narrow);
    TEST_CHECK(canvas.count({.x = 196.0, .y = 0.0, .width = 4.0, .height = 1.0 * narrow.height()}, ink) == 0);
  }

  // Plain text that is meant to be read wraps at word boundaries to the width, a word longer than the width breaks
  // inside itself, and every character stays inside the layout.
  void testPlainTextWraps() {
    std::string words;
    for (std::size_t k = 0; k < 30; ++k) {
      words += std::format("word{} lorem ipsum dolor ", k);
    }
    const auto text = words + "\n\n" + repeat("x", 300) + "\ntail";
    const auto doc = document(text, "notes.txt");
    TEST_CHECK(doc->blocks[0].kind == formats::BlockKind::Prose);
    TextTileLayout wide(doc, options(600.0));
    TextTileLayout narrow(doc, options(200.0));
    TEST_CHECK(wide.width() == 600);
    TEST_CHECK(narrow.width() == 200);
    TEST_CHECK(narrow.height() > wide.height() * 2);
    const Canvas canvas(narrow);
    TEST_CHECK(canvas.count({.x = 196.0, .y = 0.0, .width = 4.0, .height = 1.0 * narrow.height()}, ink) == 0);
    // Each word sits on one row: a break never falls inside a word that fits.
    const auto& line = joinedText(doc->blocks[0].lines[0]);
    std::size_t column = 0;
    std::size_t checked = 0;
    while (column < line.size()) {
      const auto end = std::min(line.find(' ', column), line.size());
      if (end > column) {
        TEST_CHECK(
            box(narrow, {.block = 0, .line = 0, .column = column}).y
            == box(narrow, {.block = 0, .line = 0, .column = end - 1}).y
        );
        ++checked;
      }
      column = end + 1;
    }
    TEST_CHECK(checked == 120);
    // The word wider than the layout breaks inside itself and its last character is inside the layout.
    const auto first = box(narrow, {.block = 0, .line = 2, .column = 0});
    const auto last = box(narrow, {.block = 0, .line = 2, .column = 299});
    TEST_CHECK(last.y > first.y + first.height);
    TEST_CHECK(last.x + last.width <= 200.0);
    // The blank line keeps its height, the line after it follows.
    TEST_CHECK(box(narrow, {.block = 0, .line = 3, .column = 0}).y > last.y);
    // The same text as code is not wrapped and widens the layout instead.
    TextTileLayout code(document(text, "notes.log"), options(200.0));
    TEST_CHECK(code.width() > 200);
  }

  // Processor time of this thread, in seconds.
  double threadSeconds() {
    timespec now{};
    TEST_CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return static_cast<double>(now.tv_sec) + (static_cast<double>(now.tv_nsec) / 1e9);
  }

  // A megabyte on one line and a hundred thousand short lines: the document is bounded by its limits, the whole
  // wrapped text is laid out and reachable, and the work stays within a processor time bound that a loaded machine
  // does not reach (a debug build takes about a tenth of it).
  void testProseCostBounds() {
    constexpr double kBound = 5.0; // seconds of processor time per case
    const auto oneLine = repeat("lorem ipsum dolor sit amet, ", 40000);
    const auto shortLines = repeat("short line\n", 100000);
    for (const double scale : {1.0, 2.0}) {
      auto start = threadSeconds();
      const auto doc = document(oneLine, "a.txt");
      TextTileLayout layout(doc, options(480.0, scale));
      for (std::size_t k = 0; k < std::min<std::size_t>(layout.tileCount(), 8); ++k) {
        TEST_CHECK(layout.renderTile(k).has_value());
      }
      TEST_CHECK(TEST_WITHIN_GUARD(threadSeconds() - start, kBound));
      TEST_CHECK(doc->bytesCapped);
      TEST_CHECK(layout.width() == static_cast<int>(480 * scale));
      TEST_CHECK(layout.tileCount() <= kMaxTextTiles);
      // Wrapped, not cut to one row: the text of 256 KiB fills far more than a screenful.
      TEST_CHECK(layout.height() > static_cast<int>(20000 * scale));
      const auto& lines = doc->blocks[0].lines;
      const auto lastLine = lines.size() - 1;
      const auto last = layout.characterBox({.block = 0, .line = lastLine, .column = 0});
      TEST_CHECK(last.has_value() || layout.truncated());
      if (last) {
        TEST_CHECK(last->x + last->width <= layout.width());
      }

      start = threadSeconds();
      const auto many = document(shortLines, "a.txt");
      TextTileLayout rows(many, options(480.0, scale));
      for (std::size_t k = 0; k < std::min<std::size_t>(rows.tileCount(), 8); ++k) {
        TEST_CHECK(rows.renderTile(k).has_value());
      }
      TEST_CHECK(TEST_WITHIN_GUARD(threadSeconds() - start, kBound));
      TEST_CHECK(many->linesCapped);
      TEST_CHECK(many->blocks[0].lines.size() == formats::kStyledTextLines);
    }
  }

  // The widest cases of wrapped text: a run with no space in a narrow column at the largest text size and scale, and
  // a Markdown paragraph of 1,000 source lines. Every row is inside the layout, so the end of the text can be reached.
  void testLongRunsStayReachable() {
    constexpr double kPangoLimit = 65000.0; // device pixels of one wrapped layout
    auto big = options(150.0, 2.0);
    big.fonts.proseSize = 13.0 * 3.0;
    const auto run = repeat("w", formats::kStyledProseSegment);
    const auto doc = document(run + run + "\n" + run, "a.txt");
    TextTileLayout narrow(doc, big);
    TEST_CHECK(!narrow.truncated());
    TEST_CHECK(narrow.height() < 2 * kPangoLimit);
    const auto& lines = doc->blocks[0].lines;
    TEST_CHECK(lines.size() == 3);
    for (std::size_t k = 0; k < lines.size(); ++k) {
      const auto first = narrow.characterBox({.block = 0, .line = k, .column = 0});
      const auto last = narrow.characterBox({.block = 0, .line = k, .column = formats::kStyledProseSegment - 1});
      TEST_CHECK(first.has_value() && last.has_value());
      if (first && last) {
        TEST_CHECK(last->y > first->y);
        TEST_CHECK(last->y + last->height - first->y < kPangoLimit);
        TEST_CHECK(last->x + last->width <= narrow.width());
      }
    }

    const auto paragraph = document(repeat("word word word word word word word word word\n", 1000), "a.md");
    TextTileLayout flowing(paragraph, options(300.0, 1.0));
    TEST_CHECK(paragraph->blocks[0].lines.size() > 30);
    const auto& tail = paragraph->blocks[0].lines;
    const auto end = flowing.characterBox({.block = 0, .line = tail.size() - 1, .column = 0});
    TEST_CHECK(end.has_value() && !flowing.truncated() && flowing.tileCount() > 4);
  }

  void testScalesAreProportional() {
    const auto doc = document(std::string{kMarkdown} + repeat("text that wraps across lines ", 20), "a.md");
    TextTileLayout one(doc, options(300.0, 1.0));
    TextTileLayout fractional(doc, options(300.0, 1.25));
    TextTileLayout two(doc, options(300.0, 2.0));
    TEST_CHECK(one.width() == 300);
    TEST_CHECK(fractional.width() == 375);
    TEST_CHECK(two.width() == 600);
    TEST_NEAR(fractional.height() / static_cast<double>(one.height()), 1.25, 0.08);
    TEST_NEAR(two.height() / static_cast<double>(one.height()), 2.0, 0.12);
    const auto tile = two.renderTile(0);
    TEST_CHECK(tile.has_value());
    TEST_CHECK(tile->width == 600);
    TEST_CHECK(tile->height == std::min(kTextTileHeight, two.height()));
    TEST_CHECK(tile->rgba.size() == static_cast<std::size_t>(tile->width) * tile->height * 4);
    const auto heading = box(two, {.block = 0, .line = 0, .column = 0}).height;
    TEST_NEAR(heading / box(one, {.block = 0, .line = 0, .column = 0}).height, 2.0, 0.15);
  }

  void testSameBytesTwice() {
    const auto doc = document(std::string{kMarkdown} + "\n```css\na { color: #80ff0080; }\n```\n", "a.md");
    TextTileLayout first(doc, options(320.0, 1.25));
    TextTileLayout second(doc, options(320.0, 1.25));
    TEST_CHECK(first.tileCount() == second.tileCount());
    for (std::size_t k = 0; k < first.tileCount(); ++k) {
      const auto a = first.renderTile(k);
      const auto b = second.renderTile(k);
      TEST_CHECK(a.has_value() && b.has_value());
      TEST_CHECK(a->rgba == b->rgba);
      const auto again = first.renderTile(k);
      TEST_CHECK(again->rgba == a->rgba);
    }
  }

  std::string sourceLines(std::size_t count) {
    std::string text;
    for (std::size_t k = 0; k < count; ++k) {
      text += "static int value" + std::to_string(k) + " = " + std::to_string(k * 7) + "; // line note\n";
    }
    return text;
  }

  void testCapsHold() {
    const auto doc = document(sourceLines(2000), "a.c");
    TEST_CHECK(doc->blocks[0].lines.size() == 2000);
    for (const double scale : {1.0, 2.0}) {
      TextTileLayout layout(doc, options(400.0, scale));
      TEST_CHECK(!layout.truncated());
      TEST_CHECK(layout.tileCount() <= kMaxTextTiles);
      TEST_CHECK(static_cast<std::uint64_t>(layout.width()) * layout.height() <= kMaxTextPixels);
      TEST_CHECK(box(layout, {.block = 0, .line = 1999, .column = 0}).y < layout.height());
    }

    auto big = options(400.0, 1.0);
    big.fonts.codeSize = 100.0;
    TextTileLayout tall(document(repeat("x;\n", 2000), "a.c"), big);
    TEST_CHECK(tall.truncated());
    TEST_CHECK(tall.tileCount() == kMaxTextTiles);
    TEST_CHECK(tall.height() == static_cast<int>(kMaxTextTiles) * kTextTileHeight);
    TEST_CHECK(tall.renderTile(kMaxTextTiles - 1).has_value());
    TEST_CHECK(!tall.renderTile(kMaxTextTiles).has_value());
    TEST_CHECK(!tall.characterBox({.block = 0, .line = 1999, .column = 0}).has_value());

    const auto wide = document(repeat("x", 200) + "\n" + sourceLines(2000), "a.c");
    TextTileLayout widest(wide, big);
    TEST_CHECK(widest.truncated());
    TEST_CHECK(widest.width() == kMaxTextTileWidth);
    TEST_CHECK(static_cast<std::uint64_t>(widest.width()) * widest.height() <= kMaxTextPixels);
    TEST_CHECK(widest.tileCount() * kTextTileHeight >= static_cast<std::size_t>(widest.height()));
  }

  void testHostileInput() {
    const std::string text{"a\0b\xFF\xFE\x01\n\xC3\x28 \xE2\x82\n\x1B[31m", 18};
    for (const auto* name : {"a.c", "a.md", "a.json", "a.txt"}) {
      TextTileLayout layout(document(text, name), options());
      TEST_CHECK(layout.height() > 0);
      const Canvas canvas(layout);
      TEST_CHECK(!canvas.tiles.empty());
    }
    TextTileLayout empty(std::make_shared<const formats::StyledDocument>(), options());
    TEST_CHECK(empty.tileCount() == 0);
    TEST_CHECK(!empty.renderTile(0).has_value());
    TEST_CHECK(!empty.positionAt(1.0, 1.0).has_value());
    TextTileLayout none(nullptr, options());
    TEST_CHECK(none.tileCount() == 0);
  }

  void testInvalidOptions() {
    const auto doc = document("int x;\nsome text here\n", "a.c");
    for (const double bad : {0.0, -5.0, std::nan(""), HUGE_VAL}) {
      TextTileLayout layout(doc, options(bad, bad));
      TEST_CHECK(layout.width() >= 1 && layout.width() <= kMaxTextTileWidth);
      TEST_CHECK(layout.height() > 0 && layout.tileCount() <= kMaxTextTiles);
      TEST_CHECK(layout.renderTile(0).has_value());
    }
    auto fonts = options();
    fonts.fonts.codeSize = std::nan("");
    fonts.fonts.proseSize = -1.0;
    TextTileLayout odd(doc, fonts);
    TEST_CHECK(odd.height() > 0);
  }

  void testHitTesting() {
    TextTileLayout layout(document(kMarkdown, "a.md"), options());
    for (const TextPosition position : std::initializer_list<TextPosition>{
             {.block = 0, .line = 0, .column = 2},
             {.block = 1, .line = 0, .column = 5},
             {.block = 3, .line = 0, .column = 1},
             {.block = 4, .line = 3, .column = 0},
         }) {
      const auto found = box(layout, position);
      TEST_CHECK(found.width > 0.0);
      const auto back = layout.positionAt(found.x + (found.width / 4.0), found.y + (found.height / 2.0));
      TEST_CHECK(back == position);
    }
    const auto end = box(layout, {.block = 1, .line = 0, .column = 9});
    TEST_CHECK(end.width <= 2.0);
    TEST_CHECK(!layout.characterBox({.block = 1, .line = 0, .column = 10}).has_value());
    TEST_CHECK(!layout.characterBox({.block = 1, .line = 1, .column = 0}).has_value());
    TEST_CHECK(!layout.characterBox({.block = 99, .line = 0, .column = 0}).has_value());
    // Past the end of a line, the line's end; between blocks, nothing.
    const auto body = box(layout, {.block = 1, .line = 0, .column = 0});
    TEST_CHECK((layout.positionAt(390.0, body.y + 2.0) == TextPosition{.block = 1, .line = 0, .column = 9}));
    const auto heading = box(layout, {.block = 0, .line = 0, .column = 0});
    TEST_CHECK(!layout.positionAt(10.0, (heading.y + heading.height + body.y) / 2.0).has_value());
    TEST_CHECK(!layout.positionAt(10.0, -1.0).has_value());
    TEST_CHECK(!layout.positionAt(10.0, layout.height() + 1.0).has_value());

    // A point on a swatch is the color code's column.
    TextTileLayout css(document("a { color: #12ab34; }", "a.css"), options());
    const auto swatch = css.renderTile(0)->swatches.at(0);
    TEST_CHECK((css.positionAt(swatch.x + 1.0, swatch.y + 1.0) == TextPosition{.block = 0, .line = 0, .column = 11}));
  }

  // The top of each line and table cell in document order, the row its text sits in; none past a cut layout's height.
  void testLineTops() {
    for (const double scale : {1.0, 2.0}) {
      const auto doc = document(std::string{kMarkdown} + repeat("Paragraph text that wraps. ", 40), "a.md");
      TextTileLayout layout(doc, options(300.0, scale));
      const auto tops = layout.lineTops();
      std::size_t k = 0;
      for (std::size_t b = 0; b < doc->blocks.size(); ++b) {
        for (std::size_t l = 0; l < doc->blocks[b].lines.size(); ++l, ++k) {
          TEST_CHECK(k < tops.size());
          const auto next = std::ranges::upper_bound(tops, tops[k]);
          const double bottom = next == tops.end() ? layout.height() : *next;
          const auto text = box(layout, {.block = b, .line = l, .column = 0});
          TEST_CHECK(tops[k] <= text.y + 0.5 && text.y < bottom);
        }
      }
      TEST_CHECK(k == tops.size() && k > 6);
    }
    auto big = options(400.0, 1.0);
    big.fonts.codeSize = 100.0;
    TextTileLayout tall(document(repeat("x;\n", 2000), "a.c"), big);
    const auto cut = tall.lineTops();
    TEST_CHECK(tall.truncated() && !cut.empty() && cut.size() < 2000 && cut.back() < tall.height());
    TEST_CHECK(TextTileLayout(std::make_shared<const formats::StyledDocument>(), options()).lineTops().empty());
  }

  std::vector<std::vector<std::uint8_t>> renderAll(std::shared_ptr<const formats::StyledDocument> doc, double scale) {
    TextTileLayout layout(std::move(doc), options(360.0, scale));
    std::vector<std::vector<std::uint8_t>> out;
    out.reserve(layout.tileCount());
    for (std::size_t k = 0; k < layout.tileCount(); ++k) {
      out.push_back(layout.renderTile(k).value().rgba);
    }
    return out;
  }

  void testTwoThreads() {
    const auto code = document(sourceLines(300), "a.c");
    const auto prose = document(std::string{kMarkdown} + repeat("Paragraph text that wraps. ", 200), "a.md");
    const auto codeAlone = renderAll(code, 1.0);
    const auto proseAlone = renderAll(prose, 1.25);
    for (int round = 0; round < 3; ++round) {
      std::latch start(2);
      std::vector<std::vector<std::uint8_t>> codeTiles;
      std::vector<std::vector<std::uint8_t>> proseTiles;
      std::thread first([&] {
        start.arrive_and_wait();
        codeTiles = renderAll(code, 1.0);
      });
      std::thread second([&] {
        start.arrive_and_wait();
        proseTiles = renderAll(prose, 1.25);
      });
      first.join();
      second.join();
      TEST_CHECK(codeTiles == codeAlone);
      TEST_CHECK(proseTiles == proseAlone);
    }
  }

  double millisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  // Prints layout and per-tile times; asserts nothing about speed.
  void reportTimings() {
    const auto source = sourceLines(2000);
    std::string markdown;
    for (std::size_t k = 0; markdown.size() < 200U * 1024U; ++k) {
      markdown += "## Section "
          + std::to_string(k)
          + "\n\nSome *styled* text with `code` and a [link](x). "
          + repeat("More words follow here. ", 6)
          + "\n\n- one\n- two\n\n```c\nint x = 1;\n```\n\n";
    }
    struct Sample {
      std::string_view name;
      std::string text;
      std::string_view file;
    };
    std::string oneLine;
    while (oneLine.size() < 1024U * 1024U) {
      oneLine += "lorem ipsum dolor sit amet, ";
    }
    const std::vector<Sample> samples{
        {.name = "2,000 line C", .text = source, .file = "a.c"},
        {.name = "200 KiB Markdown", .text = markdown, .file = "a.md"},
        {.name = "1 MiB one line of text", .text = oneLine, .file = "a.txt"},
        {.name = "100,000 short lines of text", .text = repeat("short line\n", 100000), .file = "a.txt"},
    };
    for (const auto& [name, text, file] : samples) {
      auto start = std::chrono::steady_clock::now();
      const auto doc = document(text, file);
      const double build = millisecondsSince(start);
      for (const double scale : {1.0, 2.0}) {
        start = std::chrono::steady_clock::now();
        TextTileLayout layout(doc, options(480.0, scale));
        const double laid = millisecondsSince(start);
        double slowest = 0.0;
        double total = 0.0;
        const auto tiles = std::min<std::size_t>(layout.tileCount(), 40);
        for (std::size_t k = 0; k < tiles; ++k) {
          start = std::chrono::steady_clock::now();
          TEST_CHECK(layout.renderTile(k).has_value());
          const double spent = millisecondsSince(start);
          slowest = std::max(slowest, spent);
          total += spent;
        }
        std::println(
            "{} at scale {}: document {:.1f} ms, layout {:.1f} ms, {}x{} px in {} tiles, tile mean {:.2f} ms, max "
            "{:.2f} ms over {} tiles",
            name, scale, build, laid, layout.width(), layout.height(), layout.tileCount(), total / tiles, slowest, tiles
        );
      }
    }
    rusage usage{};
    TEST_CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
    std::println("peak resident memory {} MiB", usage.ru_maxrss / 1024);
  }
} // namespace

int main() {
  testKeywordAndCommentColors();
  testOpaqueSwatch();
  testTranslucentSwatch();
  testJsonIndented();
  testMarkdownBlocks();
  testLongLineIsClipped();
  testTablesFitAndAlign();
  testWideTablesWiden();
  testTableWordsStayWhole();
  testWideMarkersStayInside();
  testProseWraps();
  testPlainTextWraps();
  testProseCostBounds();
  testLongRunsStayReachable();
  testScalesAreProportional();
  testSameBytesTwice();
  testCapsHold();
  testHostileInput();
  testInvalidOptions();
  testHitTesting();
  testLineTops();
  testTwoThreads();
  reportTimings();
  std::println("text tile renderer checks passed");
  return 0;
}
