#include "text_tile_view_rig.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <print>
#include <set>
#include <string>
#include <variant>

namespace {
  using namespace luciole;
  using namespace luciole::test::tiles;

  // Code lines wide enough to take the layout to its widest, kMaxTextTileWidth.
  std::string wideLines(int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
      out += std::format("int value{} = {};{}\n", i, i, std::string(700, ' ') + "// far to the right");
    }
    return out;
  }

  // Bands of `band` device rows in [0, bottom) with no ink in their first `columns` columns.
  int bandsWithoutInk(const ::test::GlPixels& pixels, int columns, int bottom, int band) {
    int empty = 0;
    for (int top = 0; top + band <= bottom; top += band) {
      empty += inkIn(pixels, 0, columns, top, top + band - 1) == 0 ? 1 : 0;
    }
    return empty;
  }

  // The lowest device row with ink in its first `columns` columns; -1 for none.
  int lowestInk(const ::test::GlPixels& pixels, int columns) {
    for (int y = pixels.height - 1; y >= 0; --y) {
      if (inkIn(pixels, 0, columns, y, y) > 0) {
        return y;
      }
    }
    return -1;
  }

  TextDocument refused(TextDocumentState state, std::string message = {}) {
    TextDocument out;
    out.state = state;
    out.message = std::move(message);
    return out;
  }

  std::string tooltipAt(Rig& rig, float x, float y) {
    rig.dispatcher.pointerMotion(x, y, ++rig.serial);
    const auto content = rig.view->inputArea().tooltipContent();
    const auto* text = std::get_if<std::string>(&content);
    return text != nullptr ? *text : std::string{};
  }

  void testStatesInWords(::test::SceneCapture& capture) {
    Rig rig(capture);
    rig.view->showLoading();
    rig.frame();
    TEST_CHECK(rig.view->message() == "Loading…");
    rig.view->show(refused(TextDocumentState::PrivateKey));
    rig.idle(std::chrono::milliseconds(20));
    TEST_CHECK(rig.view->message().contains("private key") && rig.view->textureBytes() == 0);
    rig.view->show(refused(TextDocumentState::Unreadable, "Permission denied"));
    rig.frame();
    TEST_CHECK(rig.view->message() == "This file could not be read: Permission denied");
    rig.view->show(refused(TextDocumentState::Changed));
    rig.frame();
    TEST_CHECK(rig.view->message().contains("changed while it was read"));
    rig.view->show(textOf("", "empty.txt"));
    rig.settle();
    TEST_CHECK(rig.view->message() == "Empty file");
    rig.view->show(textOf(cssLines(5), "style.css"));
    rig.settle();
    TEST_CHECK(rig.view->message().empty() && rig.view->textureBytes() > 0);
    const auto head = cssLines(5);
    const UserFileIdentity identity{.size = 3U << 20U};
    rig.view->show(textDocument(UserFileContent{.bytes = head, .truncated = true, .identity = identity}, "a.css", {}));
    rig.settle();
    // Sizes are written as GLib writes them, with a no-break space before a unit prefix.
    TEST_CHECK(rig.view->message() == std::format("Showing the first {} bytes of 3.0\u00A0MiB", head.size()));
    rig.view->show(textOf(
        std::string(
            "\x7F"
            "ELF\x02\x01\x01\x00",
            8
        ),
        "program"
    ));
    rig.settle();
    TEST_CHECK(rig.view->message().contains("as hex") && rig.view->residentTiles() == 1);
  }

  void testOnlyNearTilesHoldTextures(::test::SceneCapture& capture) {
    Rig rig(capture);
    rig.view->show(textOf(cssLines(600), "style.css"));
    rig.settle();
    const auto count = rig.view->geometry().tileCount;
    TEST_CHECK(count >= 20);
    // 300 px of a 256 px tile grid touch at most three tiles, and one more each way may be kept.
    TEST_CHECK(rig.view->residentTiles() >= 2 && rig.view->residentTiles() <= 4);
    const auto atTop = rig.view->textureBytes();
    std::println("at rest: {} tiles, {} bytes of textures", rig.view->residentTiles(), atTop);
    TEST_CHECK(atTop <= TextTileView::kDefaultTextureBudget);
    TEST_CHECK(rig.wheel(200, 150, true, 3));
    TEST_CHECK(rig.view->scrollY() > 0);
    rig.view->scrollTo(0, rig.view->maxScrollY() / 2);
    rig.settle();
    TEST_CHECK(rig.view->residentTiles() <= 5);
    // Scroll the way a user does and time the view's own per-frame work, then layout and drawing together.
    double worst = 0;
    double total = 0;
    double drawing = 0;
    constexpr int kSteps = 40;
    for (int step = 0; step < kSteps; ++step) {
      TEST_CHECK(rig.wheel(200, 150, true, 1));
      while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
      }
      const auto start = std::clock();
      rig.view->update();
      const auto updated = std::clock();
      rig.capture.capture(*rig.view, rig.width, rig.height, rig.scale);
      const auto drawn = std::clock();
      const double ms = 1000.0 * static_cast<double>(updated - start) / CLOCKS_PER_SEC;
      worst = std::max(worst, ms);
      total += ms;
      drawing += 1000.0 * static_cast<double>(drawn - updated) / CLOCKS_PER_SEC;
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    std::println(
        "scrolling: update {:.3f} ms mean, {:.3f} ms worst; layout and offscreen drawing {:.3f} ms mean (processor "
        "time)",
        total / kSteps, worst, drawing / kSteps
    );
    rig.settle();
    TEST_CHECK(rig.view->residentTiles() <= 5 && rig.view->textureBytes() <= TextTileView::kDefaultTextureBudget);
  }

  void testBudgetBoundsTextures(::test::SceneCapture& capture) {
    // One tile of the widest layout: the viewport needs three.
    constexpr std::size_t kBudget = std::size_t{kMaxTextTileWidth} * kTextTileHeight * 4;
    Rig rig(capture, kBudget);
    rig.height = 600;
    rig.view->show(textOf(wideLines(300), "a.c")); // under the builder's byte limit, so nothing is cut
    const auto pixels = rig.settle();
    TEST_CHECK(rig.view->message().empty());
    keep(pixels, "budget", rig.scale);
    const auto& geometry = rig.view->geometry();
    TEST_CHECK(geometry.width == kMaxTextTileWidth && geometry.tileCount > 4);
    // Rows 0 to 599 cross tiles 0, 1 and 2; tile 3 below would go past the budget.
    TEST_CHECK(rig.view->residentTiles() == 3);
    TEST_CHECK(rig.view->textureBytes() == 3 * kBudget);
    // Every line has ink at its start, so every band of the viewport above the horizontal bar has some.
    TEST_CHECK(bandsWithoutInk(pixels, 200, rig.height - 16, 32) == 0);
    // Code keeps its lines: the long ones scroll sideways.
    TEST_CHECK(rig.view->maxScrollX() > 100);
    TEST_CHECK(rig.wheel(200, 150, false, 2));
    TEST_CHECK(rig.view->scrollX() > 0);
    rig.view->scrollTo(1e9F, 1e9F);
    TEST_CHECK(rig.view->scrollX() == rig.view->maxScrollX() && rig.view->scrollY() == rig.view->maxScrollY());
    rig.settle();
    // At the end only the tiles crossing the viewport hold textures: three, or four when it straddles one more.
    TEST_CHECK(rig.view->residentTiles() >= 3 && rig.view->residentTiles() <= 4);
  }

  // While a new layout waits to cover the viewport, the tiles that arrived for it are kept only near the viewport.
  void testWaitingTilesFollowTheViewport(::test::SceneCapture& capture) {
    Rig rig(capture);
    rig.view->show(textOf(cssLines(1990), "style.css"));
    rig.settle();
    // The smaller text gives tiles no wider than these.
    const auto tile = static_cast<std::size_t>(rig.view->geometry().width) * kTextTileHeight * 4;
    rig.view->setTextSize(11);
    // Each frame jumps 1,100 px, past the viewport and the tile kept each way, so the new layout never covers it.
    std::size_t worst = 0;
    std::size_t total = 0;
    constexpr int kFrames = 60;
    for (int step = 0; step < kFrames; ++step) {
      const auto y = static_cast<float>((step * 1100) % 9900);
      rig.view->scrollTo(0, y);
      TEST_CHECK(rig.view->scrollY() == y);
      rig.frame();
      worst = std::max(worst, rig.view->waitingBytes());
      const auto end = Clock::now() + std::chrono::milliseconds(30);
      while (Clock::now() < end) {
        while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      total = std::max(total, rig.view->waitingBytes());
    }
    std::println("waiting tiles: at most {} after a frame, {} between frames", worst / tile, total / tile);
    TEST_CHECK(total > 0); // tiles of the new layout did arrive
    TEST_CHECK(worst <= 5 * tile);
    rig.settle();
    TEST_CHECK(rig.view->layoutRevision() == 2);
  }

  // A bar shows only when there is text out of sight: the empty margin after the last column and line scrolls only
  // along with text that does not fit.
  void testBarsOnlyForHiddenText(::test::SceneCapture& capture) {
    Rig rig(capture);
    rig.view->setTextSize(13);
    rig.width = 1200;
    rig.height = 600;
    rig.view->show(textOf(
        "/* palette */\n:root {\n  --accent: #3584e4;\n  --short: #e01b24;\n}\n"
        "button.primary { color: #ffffff; background: #9141ac; }\n",
        "theme.css"
    ));
    rig.settle();
    const int wide = rig.view->geometry().width;
    const int tall = rig.view->geometry().height;
    TEST_CHECK(wide > 300 && wide < 1200 && tall > 60 && tall < 600);
    // As wide and as tall as the text, not its margin: nothing to scroll either way.
    rig.width = wide - 6;
    rig.height = tall - 6;
    auto pixels = rig.settle();
    keep(pixels, "margin-only", rig.scale);
    // The text starts after the margin the view counts on.
    TEST_CHECK(inkIn(pixels, 0, 7, 0, 40) == 0 && inkIn(pixels, 8, 20, 0, 40) > 0);
    TEST_CHECK(rig.view->maxScrollX() == 0 && rig.view->maxScrollY() == 0);
    TEST_CHECK(!rig.view->horizontalBar().Node::visible() && !rig.view->verticalBar().Node::visible());
    TEST_CHECK(!rig.wheel(40, 20, false, 2) && rig.view->scrollX() == 0);
    const int lastInk = lowestInk(pixels, 200);
    TEST_CHECK(lastInk >= rig.height - static_cast<int>(Style::scrollbarWidth)); // where a horizontal bar lies
    // Narrower than the text by a little: the bar shows and scrolls to the margin's end. It lies over the last
    // line's foot, so that line scrolls up above it.
    rig.width = wide - 14;
    pixels = rig.settle();
    keep(pixels, "text-past-edge", rig.scale);
    TEST_CHECK(rig.view->horizontalBar().Node::visible() && rig.view->verticalBar().Node::visible());
    TEST_NEAR(rig.view->maxScrollX(), 14.0F, 0.01F);
    TEST_NEAR(rig.view->maxScrollY(), 6.0F, 0.01F);
    TEST_CHECK(rig.wheel(40, 20, false, 2) && rig.view->scrollX() > 0);
    rig.view->scrollTo(0, rig.view->maxScrollY());
    pixels = rig.settle();
    keep(pixels, "text-past-edge-bottom", rig.scale);
    const int footRow = lastInk - static_cast<int>(rig.view->scrollY());
    TEST_CHECK(footRow < rig.height - static_cast<int>(Style::scrollbarWidth));
    TEST_CHECK(inkIn(pixels, 0, 200, footRow, footRow) > 0);
  }

  void testScrollbarDrag(::test::SceneCapture& capture) {
    Rig rig(capture);
    rig.view->show(textOf(cssLines(300), "style.css"));
    rig.settle();
    const auto& bar = rig.view->verticalBar();
    TEST_CHECK(bar.Node::visible());
    float x = 0;
    float y = 0;
    Node::absolutePosition(&bar, x, y);
    const float grabX = x + bar.width() / 2;
    const float grabY = y + 4;
    rig.dispatcher.pointerEnter(grabX, grabY, ++rig.serial);
    rig.dispatcher.pointerButton(grabX, grabY, BTN_LEFT, true, ++rig.serial, 1000, false);
    rig.dispatcher.pointerMotion(grabX, grabY + 100, ++rig.serial);
    rig.dispatcher.pointerButton(grabX, grabY + 100, BTN_LEFT, false, ++rig.serial, 1100, false);
    TEST_CHECK(rig.view->scrollY() > 100);
  }

  void testSwatchTooltip(::test::SceneCapture& capture) {
    Rig rig(capture);
    rig.view->show(textOf(".a { color: #ff8000; }\n.b { color: #0000ff80; }\n", "style.css"));
    rig.settle();
    std::set<std::string> seen;
    for (float y = 0; y < 60; y += 1) {
      for (float x = 0; x < 200; x += 1) {
        if (auto tip = tooltipAt(rig, x, y); !tip.empty()) {
          seen.insert(std::move(tip));
        }
      }
    }
    TEST_CHECK((seen == std::set<std::string>{"#ff8000", "#0000ff80"}));
    TEST_CHECK(tooltipAt(rig, 290, 8).empty());
  }

  void testHiddenHoldsNothing(::test::SceneCapture& capture) {
    Rig rig(capture);
    rig.view->show(textOf(cssLines(100), "style.css"));
    rig.settle();
    rig.view->scrollTo(0, 400);
    const auto before = rig.settle();
    TEST_CHECK(rig.view->textureBytes() > 0 && rig.view->scrollY() == 400);
    rig.view->setShown(false);
    TEST_CHECK(rig.view->textureBytes() == 0 && rig.view->residentTiles() == 0 && !rig.view->visible());
    TEST_CHECK(rig.view->waitingBytes() == 0);
    const auto asked = rig.framesAsked;
    rig.idle(std::chrono::milliseconds(150));
    TEST_CHECK(rig.view->textureBytes() == 0 && rig.framesAsked == asked);
    // Shown again, it keeps its place in the text.
    rig.view->setShown(true);
    const auto after = rig.settle();
    TEST_CHECK(rig.view->textureBytes() > 0 && rig.view->scrollY() == 400);
    std::size_t differing = 0;
    for (std::size_t i = 0; i < before.rgba.size() && i < after.rgba.size(); ++i) {
      differing += before.rgba[i] != after.rgba[i] ? 1 : 0;
    }
    TEST_CHECK(before.rgba.size() == after.rgba.size() && differing == 0);
  }

  // Larger or smaller text keeps the line at the top: line 120 of 400 stays the first one shown.
  void testTextSizeKeepsTheTopLine(::test::SceneCapture& capture) {
    constexpr int kLines = 400;
    constexpr float kMargin = 8; // the tile renderer's margin around a document
    Rig rig(capture);
    rig.view->show(textOf(cssLines(kLines), "style.css"));
    rig.settle();
    const auto lineHeight = [&] {
      return ((static_cast<float>(rig.view->geometry().height) / rig.scale) - (2 * kMargin)) / kLines;
    };
    rig.view->scrollTo(0, kMargin + (120 * lineHeight()));
    rig.settle();
    for (const float size : {8.4F, 12.0F, 18.0F, 36.0F}) {
      rig.view->setTextSize(size);
      TEST_NEAR(rig.view->textSize(), size, 0.0F);
      const auto pixels = rig.settle();
      keep(pixels, std::format("size-{}", static_cast<int>(std::lround(size * 100 / 12))), rig.scale);
      const float top = (rig.view->scrollY() - kMargin) / lineHeight();
      std::println("text at {}: line {:.2f} at the top, {} px tall", size, top, rig.view->geometry().height);
      TEST_NEAR(top, 120, 0.05F);
    }
  }

  // Short lines then long paragraphs, which grow faster than the short lines as the text grows: the paragraph at the
  // top stays there. Where it lies at each size comes from a document of only the blocks above it: its height less the
  // bottom margin, then the gap of half a line before a paragraph.
  void testWrappedTextKeepsTheTopLine(::test::SceneCapture& capture) {
    constexpr float kMargin = 8;
    constexpr int kShort = 60;
    constexpr int kAbove = 20; // paragraphs above the one at the top
    const auto blocks = [](int shortLines, int paragraphs) {
      std::string out;
      for (int i = 0; i < shortLines; ++i) {
        out += std::format("Line {}\n\n", i);
      }
      for (int i = 0; i < paragraphs; ++i) {
        out += std::format("Paragraph {} ", i);
        for (int w = 0; w < 60; ++w) {
          out += "word ";
        }
        out += "\n\n";
      }
      return out;
    };
    const auto topAt = [&](float size) {
      Rig above(capture);
      above.view->setTextSize(size);
      above.view->show(textOf(blocks(kShort, kAbove), "notes.md"));
      (void)above.settle();
      const float height = static_cast<float>(above.view->geometry().height) / above.scale;
      return height - kMargin + std::round(size / 2);
    };
    Rig rig(capture);
    rig.view->show(textOf(blocks(kShort, 60), "notes.md"));
    (void)rig.settle();
    rig.view->scrollTo(0, topAt(12));
    (void)rig.settle();
    TEST_NEAR(rig.view->scrollY(), topAt(12), 0.0F);
    for (const float size : {8.4F, 18.0F, 36.0F}) {
      rig.view->setTextSize(size);
      (void)rig.settle();
      const float expected = topAt(size);
      std::println(
          "wrapped text at {}: top {:.1f}, paragraph {} at {:.1f}", size, rig.view->scrollY(), kAbove, expected
      );
      TEST_NEAR(rig.view->scrollY(), expected, 1.0F);
    }
  }
  // Plain text wraps to the view: a paragraph is a line of the file. The position is the paragraph at the top and
  // how far into its height the top lies; a new view width or text size lays the text out again and keeps both.
  void testPlainTextKeepsItsPositionWhenReflowed(::test::SceneCapture& capture) {
    constexpr std::size_t kAt = 20;
    std::string text;
    for (int i = 0; i < 60; ++i) {
      text += std::format("Paragraph {} ", i);
      for (int w = 0; w < 40 + (i % 7); ++w) {
        text += "word ";
      }
      text += '\n';
    }
    Rig rig(capture);
    rig.width = 420;
    rig.height = 240;
    rig.view->show(textOf(text, "notes.txt"));
    const auto first = rig.settle();
    keep(first, "plain-wide", rig.scale);
    TEST_CHECK(rig.view->message().empty());
    // Wrapped to the width: the text is as wide as the view and nothing scrolls sideways.
    TEST_CHECK(rig.view->geometry().width == rig.width);
    TEST_CHECK(rig.view->maxScrollX() == 0 && !rig.view->horizontalBar().Node::visible());
    TEST_CHECK(rig.view->verticalBar().Node::visible());
    const auto top = [&](std::size_t line) {
      return static_cast<float>(rig.view->geometry().lineTops[line]) / rig.scale;
    };
    const auto bottom = [&](std::size_t line) {
      const auto& tops = rig.view->geometry().lineTops;
      return (line + 1 < tops.size() ? static_cast<float>(tops[line + 1])
                                     : static_cast<float>(rig.view->geometry().height))
          / rig.scale;
    };
    // Exactly at a paragraph's top, and a third of the way into the next one.
    for (const float into : {0.0F, 0.33F}) {
      rig.width = 420;
      rig.view->setTextSize(12);
      (void)rig.settle();
      rig.view->scrollTo(0, top(kAt) + (into * (bottom(kAt) - top(kAt))));
      (void)rig.settle();
      const float before = rig.view->scrollY();
      TEST_NEAR(before, top(kAt) + (into * (bottom(kAt) - top(kAt))), 0.5F);
      const auto heightWide = rig.view->geometry().height;
      for (const int width : {300, 600, 420}) {
        rig.width = width;
        const auto pixels = rig.settle();
        keep(pixels, std::format("plain-{}-{}", width, static_cast<int>(into * 100)), rig.scale);
        TEST_CHECK(rig.view->geometry().width == width);
        const float expected = top(kAt) + (into * (bottom(kAt) - top(kAt)));
        std::println("plain width {} into {}: top {:.1f}, expected {:.1f}", width, into, rig.view->scrollY(), expected);
        TEST_NEAR(rig.view->scrollY(), expected, 1.0F);
        TEST_CHECK((width < 420) == (rig.view->geometry().height > heightWide));
      }
      for (const float size : {9.0F, 18.0F, 12.0F}) {
        rig.view->setTextSize(size);
        (void)rig.settle();
        const float expected = top(kAt) + (into * (bottom(kAt) - top(kAt)));
        std::println("plain size {} into {}: top {:.1f}, expected {:.1f}", size, into, rig.view->scrollY(), expected);
        TEST_NEAR(rig.view->scrollY(), expected, 1.0F);
      }
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
  testStatesInWords(*capture);
  testPlainTextKeepsItsPositionWhenReflowed(*capture);
  testOnlyNearTilesHoldTextures(*capture);
  testBudgetBoundsTextures(*capture);
  testWaitingTilesFollowTheViewport(*capture);
  testBarsOnlyForHiddenText(*capture);
  testScrollbarDrag(*capture);
  testSwatchTooltip(*capture);
  testHiddenHoldsNothing(*capture);
  testTextSizeKeepsTheTopLine(*capture);
  testWrappedTextKeepsTheTopLine(*capture);
  std::println("text-tile-view: all checks passed");
  return 0;
}
