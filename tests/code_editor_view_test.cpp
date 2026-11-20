#include "code_editor_rig.h"

#include <algorithm>
#include <ctime>
#include <format>
#include <print>
#include <string>
#include <vector>

namespace {
  using namespace luciole;
  using namespace luciole::test::editor;
  constexpr auto kCtrl = KeyMod::Ctrl;

  // Processor time this thread has used, in milliseconds.
  double cpuMs() {
    timespec now{};
    TEST_CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return (static_cast<double>(now.tv_sec) * 1000.0) + (static_cast<double>(now.tv_nsec) / 1.0e6);
  }

  // Lines a viewport of the rig's height shows, and those kept beside them.
  std::size_t linesKept(Rig& rig) {
    const auto shown = static_cast<std::size_t>(static_cast<float>(rig.height) / rig.view->caretRect().height) + 2;
    return shown + (2 * CodeEditorView::kLinesBeside);
  }

  // Strips drawn by the update after an action.
  struct Drawn {
    Rig& rig;
    std::uint64_t seen = rig.view->drawnStrips();

    std::uint64_t since() {
      rig.settle();
      const auto count = rig.view->drawnStrips() - seen;
      seen = rig.view->drawnStrips();
      return count;
    }
  };

  // A text shorter than the view, so no line enters or leaves the strips kept beside it.
  void testStripsRedrawnPerEdit(::test::SceneCapture& capture) {
    Rig rig(capture, numberedLines(8), "main.c");
    rig.settle();
    TEST_CHECK(rig.view->residentStrips() == 8);
    rig.caretAt({3, 6});
    Drawn drawn{rig};
    TEST_CHECK(drawn.since() == 0); // moving the caret draws nothing
    rig.type("x");
    const auto typed = drawn.since();
    rig.key(XKB_KEY_Return);
    const auto enter = drawn.since();
    rig.key(XKB_KEY_BackSpace);
    const auto joined = drawn.since();
    rig.key('z', kCtrl);
    const auto undone = drawn.since();
    rig.key(XKB_KEY_Up, KeyMod::Shift);
    const auto selected = drawn.since();
    std::println(
        "strips drawn: typed {}, Enter {}, Backspace {}, undo {}, selection {}", typed, enter, joined, undone, selected
    );
    TEST_CHECK(typed == 1 && enter == 2 && joined == 1 && undone == 2 && selected == 0);
    TEST_CHECK(rig.view->residentStrips() == 9);
  }

  // In a long text only the lines in view and beside it are drawn.
  void testStripsRedrawnInView(::test::SceneCapture& capture) {
    Rig rig(capture, numberedLines(80), "main.c");
    rig.settle();
    TEST_CHECK(rig.view->residentStrips() > 10 && rig.view->residentStrips() <= linesKept(rig));
    rig.caretAt({3, 6});
    Drawn drawn{rig};
    rig.clipboard.text = numberedLines(100);
    rig.key('v', kCtrl);
    const auto pasted = drawn.since();
    rig.key(XKB_KEY_Home, kCtrl);
    (void)drawn.since();
    rig.type("/*"); // every following line turns into a comment
    const auto opened = drawn.since();
    std::println("strips drawn: paste of 100 lines {}, comment opened {}", pasted, opened);
    TEST_CHECK(pasted > 10 && pasted <= linesKept(rig));
    TEST_CHECK(opened > 10 && opened <= linesKept(rig) + 1);
    TEST_CHECK(rig.view->residentStrips() <= linesKept(rig));
  }

  // With no budget each update draws one strip, the caret's line first.
  void testCaretLineDrawnFirst(::test::SceneCapture& capture) {
    Rig rig(capture, numberedLines(100), "notes.txt", {}, std::chrono::microseconds{0});
    rig.settle();
    rig.caretAt({10, 0});
    rig.dispatcher.setFocus(nullptr); // no caret drawn
    rig.view->setTextSize(13);
    const auto drawn = rig.view->drawnStrips();
    const auto pixels = rig.frame();
    TEST_CHECK(rig.view->drawnStrips() == drawn + 1 && !rig.view->settled());
    const auto caret = rig.view->caretRect();
    const auto top = static_cast<int>(caret.y);
    const auto bottom = static_cast<int>(caret.y + caret.height);
    const int columns = rig.width - 20; // clear of the scrollbar
    TEST_CHECK(luciole::test::tiles::inkIn(pixels, 0, columns, 0, top - 1) == 0);
    TEST_CHECK(luciole::test::tiles::inkIn(pixels, 0, columns, top, bottom) > 20);
    TEST_CHECK(luciole::test::tiles::inkIn(pixels, 0, columns, bottom + 1, rig.height) == 0);
    rig.frame();
    TEST_CHECK(rig.view->drawnStrips() == drawn + 2);
    rig.settle();
    TEST_CHECK(rig.view->residentStrips() > 10);
  }

  void testHiddenHoldsNothing(::test::SceneCapture& capture) {
    Rig rig(capture, numberedLines(100));
    rig.settle();
    rig.view->scrollTo(0, 200);
    const auto before = rig.settle();
    TEST_CHECK(rig.view->textureBytes() > 0 && rig.view->scrollY() == 200);
    rig.focus();
    TEST_CHECK(rig.view->blinking());
    rig.view->setShown(false);
    TEST_CHECK(!rig.view->visible() && !rig.view->blinking());
    TEST_CHECK(rig.view->residentStrips() == 0 && rig.view->textureBytes() == 0);
    const auto asked = rig.framesAsked;
    const auto drawn = rig.view->drawnStrips();
    // Keys that still reach it, the wheel and an input method change nothing and are left to the page.
    const auto selection = rig.selection();
    TEST_CHECK(rig.view->inputArea().focused());
    TEST_CHECK(!rig.offer('x', 0, 'x') && !rig.offer(XKB_KEY_Down) && !rig.offer(XKB_KEY_Next));
    TEST_CHECK(!rig.offer('z', kCtrl) && !rig.offer(XKB_KEY_s, kCtrl) && !rig.offer(XKB_KEY_Escape));
    rig.type("y");
    rig.key(XKB_KEY_Down);
    TEST_CHECK(!rig.wheel(100, 100, true, 3));
    TextInputEdit commit;
    commit.commitText = "ime";
    commit.hasCommitText = true;
    rig.view->textInputApplyEdit(commit);
    TEST_CHECK(rig.buffer.text() == numberedLines(100) && rig.selection() == selection && rig.edits == 0);
    TEST_CHECK(rig.saves == 0 && rig.escapes == 0 && rig.view->scrollY() == 200);
    for (int i = 0; i < 20; ++i) {
      rig.frame();
    }
    TEST_CHECK(rig.framesAsked == asked && rig.view->drawnStrips() == drawn && rig.view->residentStrips() == 0);
    TEST_CHECK(!rig.view->blinking());
    // Shown again, it draws the same place.
    rig.dispatcher.setFocus(nullptr);
    rig.view->setShown(true);
    const auto after = rig.settle();
    TEST_CHECK(rig.view->scrollY() == 200 && rig.view->textureBytes() > 0);
    std::size_t differing = 0;
    for (std::size_t i = 0; i < before.rgba.size() && i < after.rgba.size(); ++i) {
      differing += before.rgba[i] != after.rgba[i] ? 1 : 0;
    }
    TEST_CHECK(before.rgba.size() == after.rgba.size() && differing == 0);
  }

  // Idle, an unfocused view asks for nothing and draws nothing; a focused one runs only the blink.
  void testIdleCostsNothing(::test::SceneCapture& capture) {
    Rig rig(capture, numberedLines(100));
    rig.settle();
    const auto asked = rig.framesAsked;
    const auto drawn = rig.view->drawnStrips();
    const double start = cpuMs();
    for (int i = 0; i < 50; ++i) {
      rig.view->update();
    }
    const double perUpdate = (cpuMs() - start) / 50;
    std::println("an idle update takes {:.4f} ms", perUpdate);
    TEST_CHECK(perUpdate < 0.5);
    TEST_CHECK(rig.framesAsked == asked && rig.view->drawnStrips() == drawn && !rig.view->blinking());
    rig.focus();
    TEST_CHECK(rig.view->blinking() && rig.framesAsked == asked);
    rig.dispatcher.setFocus(nullptr);
    TEST_CHECK(!rig.view->blinking());
  }

  void testDestroyedLeavesNothing(::test::SceneCapture& capture) {
    Rig rig(capture, "abc");
    rig.focus();
    rig.settle();
    TEST_CHECK(rig.view->blinking() && TimerManager::instance().pollTimeoutMs() >= 0);
    rig.clipboard.hold = true;
    rig.clipboard.text = "late";
    rig.key('v', kCtrl);
    TEST_CHECK(rig.clipboard.waiting() == 1);
    TextInputEdit composing;
    composing.preeditText = "ka";
    composing.preeditCursorBegin = -1;
    composing.hasPreedit = true;
    rig.view->textInputApplyEdit(composing);
    TEST_CHECK(rig.view->composing() == "ka");
    rig.dispatcher.setSceneRoot(nullptr);
    const auto asked = rig.framesAsked;
    rig.view.reset();
    TEST_CHECK(rig.framesAsked == asked);                       // going away asks the owner for nothing
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() == -1); // no timer left behind
    rig.clipboard.answer();                                     // an answer after the view is gone changes nothing
    TEST_CHECK(rig.buffer.text() == "abc" && rig.edits == 0);
  }

  // A viewport wider than one texture over lines wider than one texture settles and then draws nothing.
  void testWiderThanOneTexture(::test::SceneCapture& capture) {
    std::string text;
    for (int i = 0; i < 12; ++i) {
      text += std::string(1200, 'w') + "\n";
    }
    Rig rig(capture, text + "short");
    rig.width = 4300;
    rig.height = 200;
    rig.settle();
    TEST_CHECK(rig.view->maxScrollX() > 0); // the lines are wider than the viewport
    const auto asked = rig.framesAsked;
    const auto drawn = rig.view->drawnStrips();
    for (int i = 0; i < 30; ++i) {
      rig.frame();
    }
    TEST_CHECK(rig.view->settled() && rig.view->drawnStrips() == drawn && rig.framesAsked == asked);
    // Scrolled sideways, the lines are drawn again around the new place and settle again.
    rig.view->scrollTo(rig.view->maxScrollX(), 0);
    rig.settle();
    TEST_CHECK(rig.view->drawnStrips() > drawn);
  }

  // A new text size keeps the caret in view when it was, and otherwise the line at the top of the view.
  void testTextSizeKeepsPlace(::test::SceneCapture& capture) {
    Rig rig(capture, numberedLines(300));
    rig.focus();
    rig.settle();
    rig.key(XKB_KEY_End, kCtrl);
    rig.settle();
    rig.view->setTextSize(30);
    rig.settle();
    const auto caret = rig.view->caretRect();
    TEST_CHECK(caret.height > 30 && caret.y >= 0 && caret.y + caret.height <= static_cast<float>(rig.height));
    rig.view->setTextSize(12);
    rig.settle();
    const auto small = rig.view->caretRect();
    TEST_CHECK(small.y >= 0 && small.y + small.height <= static_cast<float>(rig.height));
    // The caret out of view: the line at the top stays there.
    rig.key(XKB_KEY_Home, kCtrl);
    rig.view->scrollTo(0, 8.0F + (100.0F * small.height));
    rig.settle();
    rig.view->setTextSize(20);
    rig.settle();
    const float line = rig.view->caretRect().height;
    const float top = (rig.view->scrollY() - 8.0F) / line;
    std::println("text size 20: top line {:.2f}", top);
    TEST_CHECK(line > small.height && top > 99.5F && top < 100.5F);
    TEST_CHECK(rig.selection() == at(0, 0));
  }

  // Measures processor time of the view's own work: the key, then the update that draws and uploads the strips.
  void testCosts(::test::SceneCapture& capture) {
    std::string text;
    for (int i = 0; i < 20000; ++i) {
      text += std::format("int value_{} = {} * 7; // line {}\n", i, i, i);
    }
    Rig rig(capture, text, "main.c");
    rig.focus();
    rig.settle();
    rig.key(XKB_KEY_End, kCtrl);
    rig.key(XKB_KEY_Up);
    rig.key(XKB_KEY_Up);
    rig.settle();
    TEST_CHECK(rig.selection().cursor.line == 19998);
    const auto measure = [&](auto&& action) {
      const double start = cpuMs();
      action();
      rig.view->update();
      return cpuMs() - start;
    };
    // Frames until settled, timing only the updates.
    const auto drawRest = [&] {
      double total = 0;
      for (int frames = 0; frames < 200 && !rig.view->settled(); ++frames) {
        rig.capture.capture(*rig.view, rig.width, rig.height, rig.scale);
        const double start = cpuMs();
        rig.view->update();
        total += cpuMs() - start;
      }
      rig.capture.capture(*rig.view, rig.width, rig.height, rig.scale);
      TEST_CHECK(rig.view->settled());
      return total;
    };
    std::vector<double> typed;
    for (int i = 0; i < 21; ++i) {
      typed.push_back(measure([&] { rig.type("x"); }));
      rig.capture.capture(*rig.view, rig.width, rig.height, rig.scale);
    }
    std::ranges::sort(typed);
    const double enter = measure([&] { rig.key(XKB_KEY_Return); });
    drawRest();
    rig.clipboard.text = numberedLines(100);
    const double pasteKey = measure([&] { rig.key('v', kCtrl); });
    const double pasteRest = drawRest();
    rig.key(XKB_KEY_Home, kCtrl);
    drawRest();
    const double pageKey = measure([&] { rig.key(XKB_KEY_Page_Down); });
    const double pageRest = drawRest();
    std::println(
        "20,000 lines, processor ms: typed character median {:.2f} worst {:.2f}; Enter {:.2f}; paste of 100 lines "
        "{:.2f} then {:.2f} more to draw; page down {:.2f} then {:.2f} more to draw",
        typed[typed.size() / 2], typed.back(), enter, pasteKey, pasteRest, pageKey, pageRest
    );
    // Bounds well above the measured cost, low enough to catch work that grows with the document.
    TEST_CHECK(typed[typed.size() / 2] < 10.0);
    TEST_CHECK(enter < 20.0);
    TEST_CHECK(pasteKey < 40.0);
    TEST_CHECK(pageKey < 40.0);
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
  testStripsRedrawnPerEdit(*capture);
  testStripsRedrawnInView(*capture);
  testCaretLineDrawnFirst(*capture);
  testHiddenHoldsNothing(*capture);
  testIdleCostsNothing(*capture);
  testDestroyedLeavesNothing(*capture);
  testWiderThanOneTexture(*capture);
  testTextSizeKeepsPlace(*capture);
  testCosts(*capture);
  std::println("code-editor-view: all checks passed");
  return 0;
}
