#include "formats/styled_text.h"
#include "imaging/text_line_strip.h"
#include "imaging/text_tile_renderer.h"
#include "png_writer.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole;
  using formats::StyleRole;
  using Pixel = std::array<std::uint8_t, 4>;

  constexpr std::string_view kSource = "a { color: #12ab34; }\n"
                                       "\tborder: 1px solid rgba(10, 20, 30, 0.5); /* half */\n"
                                       "/* note */ .card > p::after { content: \"\u00bb\"; }\n"
                                       "@media (min-width: 40em) { .x { background: #ff8800 } }\n";

  // A transparent background, so a tile and a strip draw on the same nothing.
  TextTileTheme theme() {
    TextTileTheme out;
    out.roles.fill(rgbHex(0xE0E0E0));
    out.roles[static_cast<std::size_t>(StyleRole::Keyword)] = rgbHex(0xFF5050);
    out.roles[static_cast<std::size_t>(StyleRole::Comment)] = rgbHex(0x60D060);
    out.roles[static_cast<std::size_t>(StyleRole::String)] = rgbHex(0x6090FF);
    out.roles[static_cast<std::size_t>(StyleRole::Number)] = rgbHex(0xFFC040);
    out.roles[static_cast<std::size_t>(StyleRole::Punctuation)] = rgbHex(0xA0A0A0);
    out.background = Color{0.0F, 0.0F, 0.0F, 0.0F};
    out.outline = rgbHex(0x808080);
    out.checkerLight = rgbHex(0xC0C0C0);
    out.checkerDark = rgbHex(0x404040);
    return out;
  }

  TextLineStripOptions stripOptions(double scale) {
    TextLineStripOptions out;
    out.scale = scale;
    out.theme = theme();
    return out;
  }

  std::shared_ptr<const formats::StyledDocument> document(std::string_view text) {
    return std::make_shared<const formats::StyledDocument>(
        formats::styledText(text, formats::fileKind("a.css"), {.bytes = 65536, .lines = 16, .columns = 65536})
    );
  }

  Pixel pixelAt(const std::vector<std::uint8_t>& rgba, int width, int x, int y) {
    const auto index = ((static_cast<std::size_t>(y) * width) + x) * 4;
    return {rgba[index], rgba[index + 1], rgba[index + 2], rgba[index + 3]};
  }

  int difference(const Pixel& a, const Pixel& b) {
    int most = 0;
    for (std::size_t k = 0; k < 4; ++k) {
      most = std::max(most, std::abs(a[k] - b[k]));
    }
    return most;
  }

  bool drawn(const TextStrip& strip) {
    return std::ranges::any_of(strip.rgba, [](std::uint8_t byte) { return byte != 0; });
  }

  // The strip over a dark backdrop, as an opaque image to look at.
  ::test::GlPixels onBackdrop(const std::vector<const TextStrip*>& rows) {
    ::test::GlPixels out;
    for (const auto* row : rows) {
      out.width = std::max(out.width, row->width);
      out.height += row->height + 2;
    }
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    for (std::size_t k = 0; k < out.rgba.size(); k += 4) {
      out.rgba[k] = 0x20;
      out.rgba[k + 1] = 0x22;
      out.rgba[k + 2] = 0x28;
      out.rgba[k + 3] = 255;
    }
    int top = 0;
    for (const auto* row : rows) {
      for (int y = 0; y < row->height; ++y) {
        for (int x = 0; x < row->width; ++x) {
          const auto source = pixelAt(row->rgba, row->width, x, y);
          const auto index = ((static_cast<std::size_t>(top + y) * out.width) + x) * 4;
          for (std::size_t c = 0; c < 3; ++c) {
            out.rgba[index + c] =
                static_cast<std::uint8_t>(source[c] + (out.rgba[index + c] * (255 - source[3]) / 255));
          }
        }
      }
      top += row->height + 2;
    }
    return out;
  }

  // Writes the image when LUCIOLE_CAPTURE_DIR names a folder.
  void capture(const ::test::GlPixels& image, std::string_view name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || *dir == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(::test::writePng(image, path));
    std::println("wrote {}", path.string());
  }

  // Each line drawn as a strip equals that line cut out of a tile of the one-block code document.
  void testStripsMatchTheTile(double scale) {
    const auto doc = document(kSource);
    TEST_CHECK(doc->blocks.size() == 1 && doc->blocks[0].kind == formats::BlockKind::Code);
    const auto& lines = doc->blocks[0].lines;
    TextTileLayout layout(doc, {.width = 900.0, .scale = scale, .theme = theme(), .fonts = {}});
    TEST_CHECK(layout.tileCount() == 1);
    const auto tile = layout.renderTile(0);
    TEST_CHECK(tile.has_value());
    TextLineStrip strip(stripOptions(scale));
    std::vector<TextStrip> captured;
    for (std::size_t k = 0; k < lines.size(); ++k) {
      const auto origin = layout.characterBox({.block = 0, .line = k, .column = 0});
      TEST_CHECK(origin.has_value());
      const int left = static_cast<int>(origin->x);
      const int top = static_cast<int>(origin->y);
      TEST_CHECK(left == origin->x && top == origin->y);
      if (k > 0) {
        const auto above = layout.characterBox({.block = 0, .line = k - 1, .column = 0});
        TEST_CHECK(top - static_cast<int>(above->y) == strip.lineHeight());
      }
      const int width = strip.lineWidth(lines[k]) + 2;
      TEST_CHECK(left + width < tile->width);
      for (const auto& [x0, span] :
           {std::pair{0, width}, std::pair{0, 90}, std::pair{37, 90}, std::pair{37, width - 37},
            std::pair{width - 40, 80}}) {
        const auto cut = strip.render(lines[k], x0, span);
        TEST_CHECK(cut.has_value() && cut->height == strip.lineHeight());
        TEST_CHECK(drawn(*cut) || span != width);
        int worst = 0;
        for (int y = 0; y < cut->height; ++y) {
          for (int x = 0; x < cut->width; ++x) {
            const int tileX = left + x0 + x;
            const auto expected = tileX < tile->width ? pixelAt(tile->rgba, tile->width, tileX, top + y) : Pixel{};
            worst = std::max(worst, difference(pixelAt(cut->rgba, cut->width, x, y), expected));
          }
        }
        TEST_CHECK(worst <= 1);
      }
      captured.push_back(*strip.render(lines[k], 0, width));
      TextStrip fromTile{.x0 = 0, .width = width, .height = strip.lineHeight(), .rgba = {}};
      for (int y = 0; y < fromTile.height; ++y) {
        for (int x = 0; x < width; ++x) {
          const auto p = pixelAt(tile->rgba, tile->width, left + x, top + y);
          fromTile.rgba.insert(fromTile.rgba.end(), p.begin(), p.end());
        }
      }
      captured.push_back(std::move(fromTile));
    }
    std::vector<const TextStrip*> rows;
    rows.reserve(captured.size());
    for (const auto& c : captured) {
      rows.push_back(&c);
    }
    capture(onBackdrop(rows), std::format("strip-vs-tile-{}x.png", scale));
  }

  // A glyph from a taller fallback face does not move the line's baseline: the text before it draws as it does alone.
  void testSharedBaseline() {
    TextLineStrip strip(stripOptions(2.0));
    const auto mixed = document("ab \xE6\xBC\xA2\xE5\xAD\x97 cd")->blocks[0].lines[0];
    const auto alone = document("ab")->blocks[0].lines[0];
    TEST_CHECK(strip.caretX(alone, 2) == strip.caretX(mixed, 2));
    const int width = static_cast<int>(std::ceil(strip.caretX(mixed, 2)));
    const auto withWide = strip.render(mixed, 0, width);
    const auto without = strip.render(alone, 0, width);
    TEST_CHECK(withWide && without && drawn(*without));
    int worst = 0;
    for (int y = 0; y < without->height; ++y) {
      for (int x = 0; x < without->width; ++x) {
        worst = std::max(worst, difference(pixelAt(withWide->rgba, width, x, y), pixelAt(without->rgba, width, x, y)));
      }
    }
    TEST_CHECK(worst == 0);
  }

  // A glyph over four em wide draws whole in narrow windows that start deep inside it, as in one window of the line.
  void testWideGlyphInWindows() {
    const auto doc = document("ab﷽﷽cd");
    const auto& line = doc->blocks[0].lines[0];
    TextLineStrip strip(stripOptions(1.0));
    TEST_CHECK(strip.caretX(line, 3) - strip.caretX(line, 2) > 4.0 * TextTileFonts{}.codeSize);
    const int width = strip.lineWidth(line) + 4;
    const auto whole = strip.render(line, 0, width);
    TEST_CHECK(whole.has_value() && drawn(*whole));
    constexpr int kSpan = 3;
    TextStrip joined{.x0 = 0, .width = width, .height = strip.lineHeight(), .rgba = {}};
    joined.rgba.assign(static_cast<std::size_t>(width) * joined.height * 4, 0);
    int worst = 0;
    for (int x0 = 0; x0 < width; ++x0) {
      const auto cut = strip.render(line, x0, kSpan);
      TEST_CHECK(cut.has_value());
      for (int y = 0; y < cut->height; ++y) {
        for (int x = 0; x < kSpan && x0 + x < width; ++x) {
          const auto p = pixelAt(cut->rgba, kSpan, x, y);
          worst = std::max(worst, difference(p, pixelAt(whole->rgba, width, x0 + x, y)));
          if (x0 % kSpan == 0) {
            const auto index = ((static_cast<std::size_t>(y) * width) + x0 + x) * 4;
            std::ranges::copy(p, joined.rgba.begin() + static_cast<std::ptrdiff_t>(index));
          }
        }
      }
    }
    TEST_CHECK(worst == 0);
    capture(onBackdrop({&*whole, &joined}), "wide-glyph-windows.png");
  }

  // The swatch square sits in the gap before its color code, in the code's color with the outline around it.
  void testSwatchInItsGap() {
    const auto doc = document("a { color: #12ab34; }");
    const auto& line = doc->blocks[0].lines[0];
    TextLineStrip strip(stripOptions(2.0));
    const auto cut = strip.render(line, 0, strip.lineWidth(line));
    TEST_CHECK(cut.has_value());
    const double gapLeft = strip.caretX(line, 10) + strip.caretX(line, 1);
    const double gapRight = strip.caretX(line, 11);
    const Pixel swatch{0x12, 0xab, 0x34, 255};
    const Pixel outline{0x80, 0x80, 0x80, 255};
    int inside = 0;
    int border = 0;
    for (int y = 0; y < cut->height; ++y) {
      for (int x = 0; x < cut->width; ++x) {
        const auto p = pixelAt(cut->rgba, cut->width, x, y);
        const bool inGap = x >= gapLeft && x < gapRight;
        inside += p == swatch ? 1 : 0;
        border += p == outline ? 1 : 0;
        if (p == swatch || p == outline) {
          TEST_CHECK(inGap);
        }
      }
    }
    TEST_CHECK(inside >= 100);
    TEST_CHECK(border >= 20);
  }

  // Windows of a long line put side by side draw what one window over the same pixels draws.
  void testWindowsJoin() {
    std::string text;
    for (int k = 0; text.size() < 16384U - 24U; ++k) {
      text += "x" + std::to_string(k) + ": #a0b0c0; /* c */ ";
    }
    const auto doc = document(text);
    const auto& line = doc->blocks[0].lines[0];
    TextLineStrip strip(stripOptions(1.0));
    const int width = strip.lineWidth(line);
    TEST_CHECK(width > 3 * kMaxTextTileWidth);
    constexpr int kStep = 700;
    std::vector<TextStrip> windows;
    for (int x0 = 0; x0 < 4 * kStep; x0 += kStep) {
      windows.push_back(*strip.render(line, x0, kStep));
    }
    // Windows that straddle the seams of the first set.
    for (int x0 = kStep / 2; x0 + kStep <= 4 * kStep; x0 += kStep) {
      const auto across = strip.render(line, x0, kStep);
      TEST_CHECK(across.has_value() && drawn(*across));
      int worst = 0;
      for (int y = 0; y < across->height; ++y) {
        for (int x = 0; x < kStep; ++x) {
          const int at = x0 + x;
          const auto& joined = windows[static_cast<std::size_t>(at / kStep)];
          worst = std::max(
              worst, difference(pixelAt(across->rgba, kStep, x, y), pixelAt(joined.rgba, kStep, at % kStep, y))
          );
        }
      }
      TEST_CHECK(worst == 0);
    }
    // The seam between the widest windows, far along the line.
    const int seam = 3 * kMaxTextTileWidth;
    const auto before = strip.render(line, seam - kMaxTextTileWidth, kMaxTextTileWidth);
    const auto after = strip.render(line, seam, kMaxTextTileWidth);
    const auto across = strip.render(line, seam - 64, 128);
    TEST_CHECK(before && after && across && drawn(*across));
    for (int y = 0; y < across->height; ++y) {
      for (int x = 0; x < 128; ++x) {
        const auto expected = x < 64 ? pixelAt(before->rgba, kMaxTextTileWidth, kMaxTextTileWidth - 64 + x, y)
                                     : pixelAt(after->rgba, kMaxTextTileWidth, x - 64, y);
        TEST_CHECK(pixelAt(across->rgba, 128, x, y) == expected);
      }
    }
    const auto past = strip.render(line, width + 1, 300);
    TEST_CHECK(past.has_value() && !drawn(*past));
    std::vector<const TextStrip*> rows;
    rows.reserve(windows.size());
    for (const auto& w : windows) {
      rows.push_back(&w);
    }
    capture(onBackdrop(rows), "long-line-windows.png");
  }

  // Composing text is underlined across its columns and nowhere else.
  void testComposingUnderline() {
    const auto doc = document("p { margin: 0 auto; }");
    const auto& line = doc->blocks[0].lines[0];
    TextLineStrip strip(stripOptions(2.0));
    const int width = strip.lineWidth(line) + 8;
    const auto bare = strip.render(line, 0, width);
    const auto composing = strip.render(line, 0, width, {.begin = 4, .end = 10});
    TEST_CHECK(bare && composing);
    const double left = strip.caretX(line, 4);
    const double right = strip.caretX(line, 10);
    int changed = 0;
    int lowest = composing->height;
    for (int y = 0; y < composing->height; ++y) {
      for (int x = 0; x < width; ++x) {
        if (pixelAt(bare->rgba, width, x, y) != pixelAt(composing->rgba, width, x, y)) {
          ++changed;
          lowest = std::min(lowest, y);
          TEST_CHECK(x >= std::floor(left) - 1 && x <= std::ceil(right) + 1);
        }
      }
    }
    // The underline crosses the descender of "g", where the pixels were already inked.
    TEST_CHECK(changed >= static_cast<int>((right - left) / 2.0));
    TEST_CHECK(lowest > composing->height / 2);
    capture(onBackdrop({&*bare, &*composing}), "composing.png");
  }

  // Carets of a line with a tab, a swatch, a wide character and a combining sequence, marked as bars on the strip.
  void captureCarets() {
    const auto doc = document("a\tb: #12ab34 \xE6\xBC\xA2\xE5\xAD\x97 e\xCC\x81x;");
    const auto& line = doc->blocks[0].lines[0];
    TextLineStrip strip(stripOptions(2.0));
    auto marked = *strip.render(line, 0, strip.lineWidth(line) + 8);
    for (std::size_t c = 0; c <= 20; ++c) {
      const int x = static_cast<int>(std::lround(strip.caretX(line, c)));
      for (int y = 0; y < marked.height && x < marked.width; y += 2) {
        const auto index = ((static_cast<std::size_t>(y) * marked.width) + x) * 4;
        marked.rgba[index] = 255;
        marked.rgba[index + 1] = 0;
        marked.rgba[index + 2] = 255;
        marked.rgba[index + 3] = 255;
      }
    }
    capture(onBackdrop({&marked}), "carets.png");
  }
} // namespace

int main() {
  testStripsMatchTheTile(1.0);
  testStripsMatchTheTile(2.0);
  testStripsMatchTheTile(1.5);
  testSharedBaseline();
  testWideGlyphInWindows();
  testSwatchInItsGap();
  testWindowsJoin();
  testComposingUnderline();
  captureCarets();
  std::println("text line strip pixel checks passed");
  return 0;
}
