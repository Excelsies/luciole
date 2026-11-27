#include "text_tile_view_rig.h"

#include <chrono>
#include <cstdint>
#include <print>
#include <string>
#include <thread>

// A text tile view whose layout is made again: a changed width, scale, text size or palette, and the rows of a hex
// dump. The old tiles stay on screen until the new layout covers the viewport.
namespace {
  using namespace luciole;
  using namespace luciole::test::tiles;

  // Frames until the view settles, every one with text in rows [top, bottom] of its first `columns` columns.
  ::test::GlPixels settleInked(Rig& rig, int columns, int top, int bottom) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    for (;;) {
      auto pixels = rig.frame();
      TEST_CHECK(rig.view->residentTiles() > 0 && inkIn(pixels, 0, columns, top, bottom) > 0);
      if (rig.view->settled()) {
        return pixels;
      }
      TEST_CHECK(Clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // A hex dump takes the most bytes per row, of 16, 12, 8 and 4, that fit the view, and lays its rows out again after
  // the width changes, keeping the old rows on screen meanwhile and the same bytes in view.
  void testHexRowsFitTheWidth(::test::SceneCapture& capture) {
    Rig rig(capture);
    rig.view->setTextSize(13);
    std::string bytes;
    for (int i = 0; i < 4000; ++i) {
      bytes += static_cast<char>((i * 37) % 256);
    }
    rig.width = 450;
    rig.height = 300;
    rig.view->show(textOf(bytes, "blob.bin"));
    auto pixels = rig.settle();
    keep(pixels, "hex-450", rig.scale);
    TEST_CHECK(rig.view->message().contains("as hex"));
    const auto narrow = rig.view->geometry();
    TEST_CHECK(narrow.width <= 450 && rig.view->maxScrollX() == 0 && !rig.view->horizontalBar().Node::visible());
    rig.view->scrollTo(0, 2000);
    rig.settle(); // the rows at the new position arrive from the worker, so the old rows are on screen to be kept
    TEST_CHECK(rig.view->residentTiles() > 0);
    const auto narrowRevision = rig.view->layoutRevision();
    rig.width = 1200;
    rig.frame();
    rig.frame();
    TEST_CHECK(rig.view->layoutRevision() == narrowRevision); // the rows wait for the width to stay
    pixels = settleInked(rig, 200, 30, 290);
    keep(pixels, "hex-1200", rig.scale);
    const auto wide = rig.view->geometry();
    TEST_CHECK(wide.width > 450 && wide.width <= 1200 && rig.view->maxScrollX() == 0);
    TEST_CHECK(wide.height * 3 < narrow.height * 2); // 16 bytes a row against 8: half the rows
    TEST_CHECK(rig.view->scrollY() > 800 && rig.view->scrollY() < 1200);
    std::println("hex rows: {} px wide at 450, {} px at 1200", narrow.width, wide.width);
    // A little narrower than sixteen bytes a row takes twelve.
    rig.width = wide.width - 20;
    pixels = settleInked(rig, 200, 30, 290);
    keep(pixels, "hex-twelve", rig.scale);
    const auto middle = rig.view->geometry();
    TEST_CHECK(middle.width <= rig.width && rig.view->maxScrollX() == 0);
    TEST_CHECK(middle.height > wide.height && middle.height < narrow.height);
    // The width the rows were chosen for stays: nothing is laid out again while it does not change.
    const auto revision = rig.view->layoutRevision();
    rig.idle(std::chrono::milliseconds(200));
    TEST_CHECK(rig.view->layoutRevision() == revision && rig.view->settled());
    // A width that comes back within the delay makes no rows.
    const int middleWidth = rig.width;
    rig.width = 450;
    rig.frame();
    rig.width = middleWidth;
    rig.idle(std::chrono::milliseconds(300));
    TEST_CHECK(rig.view->layoutRevision() == revision && rig.view->settled());
    TEST_CHECK(rig.view->geometry() == middle);
  }

  void testRelayoutOnChange(::test::SceneCapture& capture) {
    Rig rig(capture);
    rig.view->show(
        textOf("# Title\n\nProse that wraps to the width of the view, line after line after line.\n", "a.md")
    );
    rig.settle();
    TEST_CHECK(rig.view->layoutRevision() == 1);
    const int narrow = rig.view->geometry().width;
    rig.width = 600;
    const auto waiting = rig.frame();
    TEST_CHECK(rig.view->layoutRevision() == 1 && !rig.view->settled()); // debounced
    // The old tiles stay on screen until the new layout covers the viewport.
    TEST_CHECK(rig.view->residentTiles() > 0 && inkIn(waiting, 0, narrow, 0, 40) > 0);
    rig.settle();
    TEST_CHECK(rig.view->layoutRevision() == 2 && rig.view->geometry().width > narrow);
    rig.scale = 1.5F;
    rig.settle();
    TEST_CHECK(rig.view->layoutRevision() == 3 && rig.view->geometry().width >= 880);
    rig.view->setTextSize(10);
    rig.settle();
    TEST_CHECK(rig.view->layoutRevision() == 4);
    auto other = testPalette();
    other.onSurface = rgbHex(0xFFFFFF);
    setPalette(other);
    rig.settle();
    TEST_CHECK(rig.view->layoutRevision() == 5);
    setPalette(testPalette());
    rig.settle();
    // Code sets its own width: a narrower view lays nothing out again.
    rig.view->show(textOf(cssLines(20), "style.css"));
    rig.settle();
    const auto revision = rig.view->layoutRevision();
    rig.width = 300;
    rig.idle(std::chrono::milliseconds(200));
    TEST_CHECK(rig.view->layoutRevision() == revision && rig.view->settled());
  }

  // The width changes and the rows of the dump are made again while the tiles of a scrolled position are still on the
  // worker. The old layout's tiles must still reach the viewport, which then stays covered while the new rows take
  // over; the first covered frame shows the old layout.
  void testReflowKeepsAScrolledViewportCovered(::test::SceneCapture& capture) {
    Rig rig(capture);
    rig.view->setTextSize(13);
    std::string bytes;
    for (int i = 0; i < 4000; ++i) {
      bytes += static_cast<char>((i * 37) % 256);
    }
    rig.width = 450;
    rig.height = 300;
    rig.view->show(textOf(bytes, "blob.bin"));
    rig.settle();
    const auto narrowRevision = rig.view->layoutRevision();
    rig.width = 1200;
    rig.frame();
    rig.frame(); // the new width is seen and the relayout timer runs
    const auto wait = [](std::chrono::milliseconds span) { std::this_thread::sleep_for(span); };
    wait(TextTileView::kRelayoutDelay + std::chrono::milliseconds(80)); // nothing is pumped meanwhile
    TimerManager::instance().tick();                                    // the rows are made on a worker
    wait(std::chrono::milliseconds(200));                               // done; its answer waits for the main context
    rig.view->scrollTo(0, 2500);
    rig.view->update();                   // asks for the tiles of that place in the old layout
    wait(std::chrono::milliseconds(200)); // drawn; they wait for the main context too
    // Both answers are handed over, the rows first: they wait for the tiles of the scrolled place, which are not in
    // yet.
    while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
    }
    TEST_CHECK(rig.view->hexAnswerHeld() && !rig.view->settled());
    TEST_CHECK(rig.view->layoutRevision() == narrowRevision);
    std::uint64_t revisionWhenCovered = 0;
    bool covered = false;
    int blankAfterCovered = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    for (;;) {
      const auto pixels = rig.frame();
      const bool inked = inkIn(pixels, 0, 200, 30, 290) > 0;
      if (inked && !covered) {
        covered = true;
        revisionWhenCovered = rig.view->layoutRevision();
      }
      blankAfterCovered += covered && !inked ? 1 : 0;
      if (rig.view->settled()) {
        break;
      }
      TEST_CHECK(Clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(covered && inkIn(rig.frame(), 0, 200, 30, 290) > 0);
    TEST_CHECK(revisionWhenCovered == narrowRevision); // the old layout's tiles covered it, not the new layout
    TEST_CHECK(blankAfterCovered == 0);
    TEST_CHECK(!rig.view->hexAnswerHeld());
    TEST_CHECK(rig.view->layoutRevision() > narrowRevision && rig.view->geometry().width > 450);
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
  testHexRowsFitTheWidth(*capture);
  testReflowKeepsAScrolledViewportCovered(*capture);
  testRelayoutOnChange(*capture);
  std::println("text-tile-view-reflow: all checks passed");
  return 0;
}
