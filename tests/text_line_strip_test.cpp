#include "font_coverage.h"
#include "formats/styled_text.h"
#include "imaging/text_line_strip.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <print>
#include <string>
#include <string_view>

namespace {
  using namespace luciole;
  using formats::StyleRole;

  TextLineStripOptions options(double scale = 1.0) {
    TextLineStripOptions out;
    out.scale = scale;
    out.theme.roles.fill(rgbHex(0xFFFFFF));
    out.theme.roles[static_cast<std::size_t>(StyleRole::Keyword)] = rgbHex(0xFF0000);
    out.theme.outline = rgbHex(0x808080);
    return out;
  }

  formats::StyledLine plain(std::string_view text) {
    formats::StyledLine line;
    line.runs.push_back({.text = std::string(text)});
    return line;
  }

  // The first line of a file of the kind, colored as a text preview colors it.
  formats::StyledLine styled(std::string_view text, std::string_view name) {
    auto document = formats::styledText(text, formats::fileKind(name), {.bytes = 65536, .lines = 4, .columns = 65536});
    TEST_CHECK(document.blocks.size() == 1);
    TEST_CHECK(!document.blocks[0].lines.empty());
    return document.blocks[0].lines[0];
  }

  void testHeightIsShared() {
    TextLineStrip strip(options());
    const int height = strip.lineHeight();
    TEST_CHECK(height >= 12 && height <= 24);
    TEST_CHECK(strip.render(plain("int x;"), 0, 50)->height == height);
    TEST_CHECK(strip.render(plain(""), 0, 50)->height == height);
    TextLineStrip twice(options(2.0));
    TEST_CHECK(std::abs(twice.lineHeight() - (2 * height)) <= 1);
  }

  void testCaretGrowsAndRoundTrips() {
    TextLineStrip strip(options());
    const auto line = styled("int value = 42; // note", "a.c");
    const std::size_t end = 23;
    TEST_CHECK(strip.caretX(line, 0) == 0.0);
    for (std::size_t c = 0; c < end; ++c) {
      const double left = strip.caretX(line, c);
      const double right = strip.caretX(line, c + 1);
      TEST_CHECK(right > left + 3.0);
      TEST_CHECK(strip.columnAt(line, left) == c);
      TEST_CHECK(strip.columnAt(line, left + 1.0) == c);
      TEST_CHECK(strip.columnAt(line, right - 1.0) == c + 1);
    }
    TEST_CHECK(std::abs(strip.caretX(line, end) - strip.lineWidth(line)) <= 1.0);
    TEST_CHECK(strip.columnAt(line, strip.lineWidth(line) + 50.0) == end);
  }

  // Two color codes take a gap each before them, and a point in a gap belongs to the code after it.
  void testSwatchGaps() {
    TextLineStrip strip(options());
    const std::string text = "a { color: #12ab34; border: #fff; }";
    const auto colored = styled(text, "a.css");
    TEST_CHECK(colored.swatches.size() == 2);
    TEST_CHECK(colored.swatches[0].column == 11 && colored.swatches[1].column == 28);
    const auto bare = plain(text);
    const double advance = strip.caretX(bare, 1);
    const double em = TextTileFonts{}.codeSize;
    const double first = strip.caretX(colored, 11) - strip.caretX(bare, 11);
    TEST_CHECK(strip.caretX(colored, 10) == strip.caretX(bare, 10));
    TEST_CHECK(first >= 0.8 * em && first <= 1.5 * em);
    TEST_NEAR(strip.caretX(colored, 28) - strip.caretX(bare, 28), 2.0 * first, 0.01);
    TEST_NEAR(strip.lineWidth(colored) - strip.lineWidth(bare), 2.0 * first, 1.0);
    // The gap sits between the space at column 10 and the code at 11.
    const double gapLeft = strip.caretX(colored, 10) + advance;
    const double gapRight = strip.caretX(colored, 11);
    for (double x = gapLeft + 0.5; x < gapRight; x += 1.0) {
      TEST_CHECK(strip.columnAt(colored, x) == 11);
    }
    for (std::size_t c = 0; c < text.size(); ++c) {
      TEST_CHECK(strip.columnAt(colored, strip.caretX(colored, c)) == c);
    }
  }

  // Tabs stop every eight spaces from the line's start.
  void testTabs() {
    TextLineStrip strip(options());
    const auto spaces = plain("                x");
    const double eight = strip.caretX(spaces, 8);
    TEST_NEAR(strip.caretX(plain("\tx"), 1), eight, 0.01);
    TEST_NEAR(strip.caretX(plain("ab\tx"), 3), eight, 0.01);
    TEST_NEAR(strip.caretX(plain("abcdefg\tx"), 8), eight, 0.01);
    TEST_NEAR(strip.caretX(plain("abcdefgh\tx"), 9), strip.caretX(spaces, 16), 0.01);
    const auto tabbed = plain("\tx");
    TEST_CHECK(strip.columnAt(tabbed, eight - 1.0) == 1);
    TEST_CHECK(strip.columnAt(tabbed, 1.0) == 0);
    TEST_CHECK(strip.columnAt(tabbed, eight + 1.0) == 1);
  }

  // "e" and a combining acute accent are one cluster of two columns: no caret lands between them.
  void testCombiningSequence() {
    TextLineStrip strip(options());
    const auto line = plain("aéx");
    const double e = strip.caretX(line, 1);
    TEST_CHECK(e > 0.0);
    TEST_CHECK(strip.caretX(line, 2) == e);
    TEST_CHECK(strip.caretX(line, 3) > e);
    TEST_NEAR(strip.caretX(line, 3) - e, e, 0.5);
    for (double x = -2.0; x < strip.lineWidth(line) + 4.0; x += 0.25) {
      TEST_CHECK(strip.columnAt(line, x) != 2);
    }
    TEST_CHECK(strip.columnAt(line, e + 1.0) == 1);
    TEST_CHECK(strip.columnAt(line, strip.caretX(line, 3) - 1.0) == 3);
  }

  // Each CJK character is clearly wider than a monospace cell; the fallback face sets its width.
  void testWideCharacters() {
    if (!luciole::test::installedFontCovers(0x6F22)) { // no font to fall back to for a CJK character
      std::println("skipped wide characters: no installed font has CJK characters");
      return;
    }
    TextLineStrip strip(options());
    const auto line = plain("ab漢字cd");
    const double cell = strip.caretX(line, 1);
    for (std::size_t c = 0; c < 6; ++c) {
      const double left = strip.caretX(line, c);
      const double right = strip.caretX(line, c + 1);
      if (c == 2 || c == 3) {
        TEST_CHECK(right - left > 1.4 * cell && right - left < 2.5 * cell);
      } else {
        TEST_NEAR(right - left, cell, 0.01);
      }
      TEST_CHECK(strip.columnAt(line, left) == c);
      TEST_CHECK(strip.columnAt(line, left + (0.3 * (right - left))) == c);
      TEST_CHECK(strip.columnAt(line, right - (0.3 * (right - left))) == c + 1);
    }
  }

  void testHostileInput() {
    TextLineStrip strip(options());
    const auto line = plain("int x;");
    const auto none = strip.render(line, 0, 0);
    TEST_CHECK(none.has_value() && none->width == 0 && none->rgba.empty());
    const auto negative = strip.render(line, 0, -40);
    TEST_CHECK(negative.has_value() && negative->width == 0 && negative->rgba.empty());
    const auto capped = strip.render(line, 0, std::numeric_limits<int>::max());
    TEST_CHECK(capped.has_value() && capped->width == kMaxTextTileWidth);
    TEST_CHECK(capped->rgba.size() == static_cast<std::size_t>(kMaxTextTileWidth) * capped->height * 4);
    for (const int x0 : {std::numeric_limits<int>::max(), std::numeric_limits<int>::min(), 5000, -5000}) {
      const auto far = strip.render(line, x0, 64);
      TEST_CHECK(far.has_value() && far->width == 64 && far->x0 == x0);
      TEST_CHECK(std::ranges::all_of(far->rgba, [](std::uint8_t byte) { return byte == 0; }));
    }
    const auto empty = plain("");
    TEST_CHECK(strip.lineWidth(empty) == 0);
    TEST_CHECK(strip.caretX(empty, 0) == 0.0);
    TEST_CHECK(strip.caretX(empty, 9) == 0.0);
    TEST_CHECK(strip.columnAt(empty, 30.0) == 0);
    const auto blank = strip.render(empty, 0, 32);
    TEST_CHECK(blank.has_value());
    TEST_CHECK(std::ranges::all_of(blank->rgba, [](std::uint8_t byte) { return byte == 0; }));
    TEST_CHECK(strip.caretX(line, 1000) == strip.caretX(line, 6));
    TEST_CHECK(strip.columnAt(line, std::numeric_limits<double>::quiet_NaN()) == 0);
    TEST_CHECK(strip.columnAt(line, -std::numeric_limits<double>::infinity()) == 0);
    TEST_CHECK(strip.columnAt(line, std::numeric_limits<double>::infinity()) == 6);
    TEST_CHECK(strip.columnAt(line, 1e300) == 6);
    // A swatch past the line's end and an empty composing range draw nothing out of place.
    auto stray = line;
    stray.swatches.push_back({.color = {.r = 255, .g = 0, .b = 0, .a = 255}, .column = 400});
    TEST_CHECK(strip.render(stray, 0, 100, {.begin = 5, .end = 2}).has_value());
    TEST_CHECK(strip.render(line, 0, 100, {.begin = 3, .end = 900}).has_value());
    // Out-of-range options are clamped.
    TextLineStripOptions odd = options(std::numeric_limits<double>::quiet_NaN());
    odd.fonts.codeSize = std::numeric_limits<double>::infinity();
    TextLineStrip clamped(odd);
    TEST_CHECK(clamped.lineHeight() > 0 && clamped.lineHeight() < 1000);
    TEST_CHECK(clamped.render(line, 0, 300).has_value());
  }

  // At the largest size a long line of tabs passes what the shaper can place: it stops at a column, still in order.
  void testLineTooWideForTheShaper() {
    TextLineStripOptions large = options(4.0);
    large.fonts.codeSize = 200.0;
    TextLineStrip strip(large);
    const auto line = plain(std::string(16384, '\t') + "x");
    const double end = strip.caretX(line, 16385);
    TEST_CHECK(end > 0.0 && end < 2.2e6);
    TEST_CHECK(strip.caretX(line, 10) > strip.caretX(line, 9));
    TEST_CHECK(strip.columnAt(line, 1e9) < 16385);
    TEST_CHECK(strip.lineWidth(line) > 0);
  }

  // Glyphs over four em wide pass what the shaper can place well before a long line ends: the line stops at a column,
  // its carets stay in order and in reach, and points past its end give that column.
  void testWideGlyphsPastTheShaper() {
    TextLineStrip strip(options());
    const std::string wide = "﷽";
    const double em = TextTileFonts{}.codeSize;
    TEST_CHECK(strip.caretX(plain(wide), 1) > 4.0 * em);
    constexpr std::size_t kCount = 43690;
    std::string text;
    for (std::size_t k = 0; k < kCount; ++k) {
      text += wide;
    }
    const auto line = plain(text);
    double previous = 0.0;
    std::size_t decreasing = 0;
    // Every 89th column.
    for (std::size_t c = 0; c < kCount; c += 89) {
      const double x = strip.caretX(line, c);
      decreasing += x < previous ? 1 : 0;
      previous = x;
    }
    TEST_CHECK(decreasing == 0);
    const double end = strip.caretX(line, kCount);
    TEST_CHECK(end >= previous);
    TEST_CHECK(end > 1.0e6 && end < 1.1e6);
    const auto reached = strip.columnAt(line, 2.0e6);
    TEST_CHECK(reached > 10000 && reached < kCount);
    TEST_CHECK(strip.caretX(line, reached) == end);
    TEST_CHECK(std::abs(strip.lineWidth(line) - end) <= em);
    const auto last = strip.render(line, strip.lineWidth(line) - 400, 400);
    TEST_CHECK(last.has_value() && std::ranges::any_of(last->rgba, [](std::uint8_t byte) { return byte != 0; }));
  }

  // In right-to-left text a caret sits on its character's leading edge, the right one. Clicking where a caret is drawn
  // gives that caret back; where the direction changes, one place stands for two columns.
  void testRightToLeft() {
    TextLineStrip strip(options());
    const auto line = plain("x = \"שלום\";");
    constexpr std::size_t kEnd = 11;
    for (std::size_t c = 6; c <= 8; ++c) {
      TEST_CHECK(strip.caretX(line, c) < strip.caretX(line, c - 1) - 3.0);
    }
    TEST_CHECK(strip.caretX(line, 5) == strip.caretX(line, 9));
    for (std::size_t c = 0; c <= kEnd; ++c) {
      const double x = strip.caretX(line, c);
      const auto back = strip.columnAt(line, x);
      TEST_CHECK(back == c || (c == 5 && back == 9));
      TEST_CHECK(strip.caretX(line, back) == x);
    }
  }

  // Processor time of this thread in milliseconds, so other work on the machine does not count.
  double cpuMilliseconds() {
    timespec now{};
    TEST_CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return (static_cast<double>(now.tv_sec) * 1000.0) + (static_cast<double>(now.tv_nsec) / 1e6);
  }

  // Mean processor milliseconds of `rounds` calls.
  template <typename Call> double meanCost(int rounds, Call call) {
    const double start = cpuMilliseconds();
    for (int k = 0; k < rounds; ++k) {
      call(k);
    }
    return (cpuMilliseconds() - start) / rounds;
  }

  // A 120 column CSS line with two swatches; `variant` changes one character so the strip cannot reuse its layout.
  formats::StyledLine ordinaryLine(int variant) {
    std::string text = "  box-shadow: 0 0 4px #12ab34, inset 0 1px 2px #ff8800aa; /* raised card edge, ";
    text += static_cast<char>('a' + (variant % 2));
    text.resize(118, 'x');
    text += "*/";
    auto line = styled(text, "a.css");
    TEST_CHECK(line.swatches.size() == 2);
    return line;
  }

  // The kept layout makes questions about the caret's line cheap, and an ordinary line costs a small part of the
  // editor's 2 ms per keystroke. Bounds are loose; the numbers are printed.
  void testCost() {
    TextLineStrip strip(options());
    const std::array lines{ordinaryLine(0), ordinaryLine(1)};
    TEST_CHECK(strip.lineWidth(lines[0]) > 600);
    const double renderCold = meanCost(100, [&](int k) { TEST_CHECK(strip.render(lines[k % 2], 0, 1000)); });
    const double renderWarm = meanCost(100, [&](int) { TEST_CHECK(strip.render(lines[0], 0, 1000)); });
    double sink = 0.0;
    const double caretCold = meanCost(200, [&](int k) { sink += strip.caretX(lines[k % 2], 60); });
    const double caretWarm = meanCost(2000, [&](int k) { sink += strip.caretX(lines[0], k % 120); });
    const double columnCold = meanCost(200, [&](int k) { sink += strip.columnAt(lines[k % 2], 400.0); });
    const double columnWarm = meanCost(2000, [&](int k) { sink += strip.columnAt(lines[0], k % 800); });
    TEST_CHECK(sink > 0.0);
    std::println(
        "120 column line, processor ms: render {:.3f} shaped anew, {:.3f} kept; caretX {:.4f} anew, {:.4f} kept; "
        "columnAt {:.4f} anew, {:.4f} kept",
        renderCold, renderWarm, caretCold, caretWarm, columnCold, columnWarm
    );
    TEST_CHECK(caretWarm * 4.0 < caretCold);
    TEST_CHECK(columnWarm * 4.0 < columnCold);
    TEST_CHECK(caretWarm < 0.05 && columnWarm < 0.05);
    TEST_CHECK(renderCold < 1.0);

    std::string text;
    for (int k = 0; text.size() < 16384U - 16U; ++k) {
      text += "x" + std::to_string(k) + " = #a0b0c0; ";
    }
    text.resize(16384, ' ');
    const auto longLine = styled(text, "a.css");
    double shaped = cpuMilliseconds();
    const int width = strip.lineWidth(longLine);
    shaped = cpuMilliseconds() - shaped;
    TEST_CHECK(width > 100000);
    int windows = 0;
    const double perWindow = meanCost((width + kMaxTextTileWidth - 1) / kMaxTextTileWidth, [&](int k) {
      TEST_CHECK(strip.render(longLine, k * kMaxTextTileWidth, kMaxTextTileWidth));
      ++windows;
    });
    const double longCaret = meanCost(200, [&](int k) { sink += strip.caretX(longLine, 16000 - k); });
    const double longColumn = meanCost(200, [&](int k) { sink += strip.columnAt(longLine, width - (k * 10.0)); });
    std::println(
        "16 KiB line with {} swatches, {} px: shaping {:.2f} ms, {} windows of {} px at {:.2f} ms each, caretX {:.3f} "
        "ms, columnAt {:.3f} ms",
        longLine.swatches.size(), width, shaped, windows, kMaxTextTileWidth, perWindow, longCaret, longColumn
    );
    TEST_CHECK(perWindow < 50.0);
  }
} // namespace

int main() {
  testHeightIsShared();
  testCaretGrowsAndRoundTrips();
  testSwatchGaps();
  testTabs();
  testCombiningSequence();
  testWideCharacters();
  testHostileInput();
  testLineTooWideForTheShaper();
  testWideGlyphsPastTheShaper();
  testRightToLeft();
  testCost();
  std::println("text line strip checks passed");
  return 0;
}
