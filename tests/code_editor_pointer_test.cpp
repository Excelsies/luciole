#include "code_editor_rig.h"
#include "cursor-shape-v1-client-protocol.h"
#include "ui/controls/scrollbar.h"
#include "ui/text_input_client.h"

#include <print>
#include <string>

namespace {
  using namespace luciole;
  using namespace luciole::test::editor;
  constexpr auto kCtrl = KeyMod::Ctrl;
  constexpr auto kShift = KeyMod::Shift;

  // The middle of a line's row at a logical x.
  float rowY(Rig& rig, std::size_t line) {
    const float height = rig.view->caretRect().height;
    return 8.0F - rig.view->scrollY() + ((static_cast<float>(line) + 0.5F) * height);
  }

  void testClickAndShiftClick(::test::SceneCapture& capture) {
    Rig rig(capture, "alpha beta\ngamma delta\nepsilon");
    rig.settle();
    const auto beta = rig.caretAt({1, 6});
    rig.key(XKB_KEY_Home, kCtrl);
    rig.click(beta.x + 1, beta.y + (beta.height / 2));
    TEST_CHECK(rig.selection() == at(1, 6));
    TEST_CHECK(rig.dispatcher.focusedArea() == &rig.view->inputArea()); // a press takes the keyboard
    TEST_CHECK(rig.view->inputArea().cursorShape() == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
    rig.held = kShift;
    rig.click(380, rowY(rig, 2)); // past the line's end
    TEST_CHECK((rig.selection() == span({1, 6}, {2, 7})));
    rig.held = 0;
    rig.click(2, rowY(rig, 0));
    TEST_CHECK(rig.selection() == at(0, 0));
    rig.click(200, 290); // below the last line
    TEST_CHECK(rig.selection() == at(2, 7));
    TEST_CHECK(rig.edits == 0);
  }

  void testDoubleAndTripleClick(::test::SceneCapture& capture) {
    Rig rig(capture, "alpha beta_gamma, delta\nnext line");
    rig.settle();
    const auto inside = rig.caretAt({0, 9});
    const float x = inside.x + 1;
    const float y = inside.y + (inside.height / 2);
    rig.click(x, y);
    rig.click(x, y);
    TEST_CHECK((rig.selection() == span({0, 6}, {0, 16})));
    rig.click(x, y);
    TEST_CHECK((rig.selection() == span({0, 0}, {1, 0}))); // the line and its break
    rig.click(x + 40, y);                                  // too far away to count as a fourth click
    TEST_CHECK(rig.selection().empty());
    // A double click then a drag extends by words.
    const auto delta = rig.caretAt({0, 20});
    rig.click(x, y);
    rig.press(x, y);
    rig.dispatcher.pointerMotion(delta.x + 1, y, ++rig.serial);
    TEST_CHECK((rig.selection() == span({0, 6}, {0, 23})));
    rig.release(delta.x + 1, y);
  }

  void testDragSelectsAndScrolls(::test::SceneCapture& capture) {
    Rig rig(capture, numberedLines(100));
    rig.settle();
    rig.press(4, rowY(rig, 2));
    rig.dispatcher.pointerMotion(380, rowY(rig, 4), ++rig.serial);
    TEST_CHECK((rig.selection() == span({2, 0}, {4, 6})));
    TEST_CHECK(rig.view->scrollY() == 0);
    // Past the bottom edge the selection reaches lines out of view and the view follows.
    rig.dispatcher.pointerMotion(40, static_cast<float>(rig.height) + 60, ++rig.serial);
    const auto cursor = rig.selection().cursor;
    TEST_CHECK(rig.view->scrollY() > 0 && cursor.line > 20);
    const auto caret = rig.view->caretRect();
    TEST_CHECK(caret.y >= 0 && caret.y + caret.height <= static_cast<float>(rig.height));
    rig.release(40, static_cast<float>(rig.height) + 60);
    TEST_CHECK(rig.selection().anchor == (editing::TextPosition{2, 0}));
    // Back above the top edge.
    rig.press(4, rowY(rig, cursor.line));
    rig.dispatcher.pointerMotion(4, -40, ++rig.serial);
    TEST_CHECK(rig.selection().cursor.line < cursor.line - 2);
    rig.release(4, -40);
    rig.settle();
  }

  void testWheel(::test::SceneCapture& capture) {
    // Lines are measured as they are drawn, so the wide one is in view.
    Rig rig(capture, std::string(400, 'w') + "\n" + numberedLines(100));
    rig.settle();
    TEST_CHECK(rig.view->maxScrollX() > 0 && rig.view->maxScrollY() > 0);
    TEST_CHECK(rig.wheel(100, 100, true, 3));
    TEST_CHECK(rig.view->scrollY() > 0 && rig.view->scrollX() == 0);
    rig.held = kShift;
    TEST_CHECK(rig.wheel(100, 100, true, 2));
    TEST_CHECK(rig.view->scrollX() > 0);
    rig.held = 0;
    const float x = rig.view->scrollX();
    TEST_CHECK(rig.wheel(100, 100, false, -1));
    TEST_CHECK(rig.view->scrollX() < x);
    TEST_CHECK(rig.selection() == at(0, 0)); // the wheel leaves the caret
    rig.settle();
  }

  // The bars show on a long, wide text, and dragging a thumb scrolls without moving the caret.
  void testScrollbars(::test::SceneCapture& capture) {
    Rig rig(capture, std::string(300, 'w') + "\n" + numberedLines(200));
    rig.focus();
    rig.settle();
    const auto& vertical = rig.view->verticalBar();
    const auto& horizontal = rig.view->horizontalBar();
    TEST_CHECK(vertical.visible() && horizontal.visible());
    TEST_CHECK(vertical.thumbTravel() > 0 && horizontal.thumbTravel() > 0);
    const float right = static_cast<float>(rig.width) - 4;
    const float bottom = static_cast<float>(rig.height) - 4;
    rig.dispatcher.pointerMotion(right, 10, ++rig.serial);
    (void)rig.dispatcher.pointerButton(right, 10, BTN_LEFT, true, ++rig.serial, rig.time += 10, false);
    rig.dispatcher.pointerMotion(right, 150, ++rig.serial);
    (void)rig.dispatcher.pointerButton(right, 150, BTN_LEFT, false, ++rig.serial, rig.time += 10, false);
    const float down = rig.view->scrollY();
    std::println("vertical thumb dragged 140 px: scrolled {:.1f} of {:.1f}", down, rig.view->maxScrollY());
    TEST_CHECK(down > 0.3F * rig.view->maxScrollY() && down < rig.view->maxScrollY());
    TEST_CHECK(rig.view->scrollX() == 0 && rig.selection() == at(0, 0));
    rig.dispatcher.pointerMotion(10, bottom, ++rig.serial);
    (void)rig.dispatcher.pointerButton(10, bottom, BTN_LEFT, true, ++rig.serial, rig.time += 10, false);
    rig.dispatcher.pointerMotion(200, bottom, ++rig.serial);
    (void)rig.dispatcher.pointerButton(200, bottom, BTN_LEFT, false, ++rig.serial, rig.time += 10, false);
    TEST_CHECK(rig.view->scrollX() > 0 && rig.view->scrollY() == down && rig.selection() == at(0, 0));
    TEST_CHECK(rig.edits == 0);
    rig.settle();
  }

  void testClipboard(::test::SceneCapture& capture) {
    Rig rig(capture, "one two\nthree");
    rig.focus();
    rig.settle();
    rig.key(XKB_KEY_Right, kCtrl | kShift);
    rig.key('c', kCtrl);
    TEST_CHECK(rig.clipboard.text == "one" && rig.buffer.text() == "one two\nthree");
    rig.key(XKB_KEY_End, kCtrl);
    rig.key('v', kCtrl);
    TEST_CHECK(rig.buffer.text() == "one two\nthreeone" && rig.selection() == at(1, 8));
    rig.key(XKB_KEY_Home, kCtrl);
    rig.key(XKB_KEY_Down, kShift);
    rig.key('x', kCtrl);
    TEST_CHECK(rig.clipboard.text == "one two\n" && rig.buffer.text() == "threeone");
    rig.key(XKB_KEY_Insert, kShift);
    TEST_CHECK(rig.buffer.text() == "one two\nthreeone");
    rig.key(XKB_KEY_Left, kShift);
    rig.key(XKB_KEY_Insert, kCtrl);
    TEST_CHECK(rig.clipboard.text == "\n");
    rig.key(XKB_KEY_Delete, kShift);
    TEST_CHECK(rig.buffer.text() == "one twothreeone" && rig.clipboard.text == "\n");
    rig.key(XKB_KEY_Delete, kShift); // nothing selected: an ordinary Delete
    TEST_CHECK(rig.buffer.text() == "one twohreeone" && rig.clipboard.text == "\n");
    // Copying nothing leaves the clipboard alone, and the keys stay the editor's.
    TEST_CHECK(rig.offer('c', kCtrl) && rig.clipboard.text == "\n");
    TEST_CHECK(rig.edits == 5);
  }

  void testPasteAnswersLate(::test::SceneCapture& capture) {
    Rig rig(capture, "abc");
    rig.focus();
    rig.settle();
    rig.clipboard.text = "XY";
    rig.clipboard.hold = true;
    rig.key('v', kCtrl);
    TEST_CHECK(rig.clipboard.waiting() == 1 && rig.buffer.text() == "abc");
    rig.clipboard.answer();
    TEST_CHECK(rig.buffer.text() == "XYabc");
    // The text changed meanwhile: the answer is dropped.
    rig.key('v', kCtrl);
    rig.type("q");
    rig.clipboard.answer();
    TEST_CHECK(rig.buffer.text() == "XYqabc");
    // The view lost the keyboard meanwhile, even if it took it back.
    rig.key('v', kCtrl);
    rig.dispatcher.setFocus(nullptr);
    rig.focus();
    rig.clipboard.answer();
    TEST_CHECK(rig.buffer.text() == "XYqabc");
    // A selection moved but not the text: the answer replaces the selection.
    rig.key('v', kCtrl);
    rig.key(XKB_KEY_Home, kShift);
    rig.clipboard.answer();
    TEST_CHECK(rig.buffer.text() == "XYabc" && rig.edits == 3);
  }

  // A paste that arrives while an input method composes drops the composition, as typing does.
  void testPasteWhileComposing(::test::SceneCapture& capture) {
    Rig rig(capture, "ab\ncd");
    rig.focus();
    rig.settle();
    rig.key(XKB_KEY_Right);
    rig.clipboard.text = "X\nY";
    rig.clipboard.hold = true;
    rig.key('v', kCtrl);
    TextInputEdit edit;
    edit.preeditText = "ka";
    edit.preeditCursorBegin = -1;
    edit.hasPreedit = true;
    rig.view->textInputApplyEdit(edit);
    TEST_CHECK(rig.view->composing() == "ka");
    rig.clipboard.answer();
    TEST_CHECK(rig.buffer.text() == "aX\nYb\ncd" && rig.selection() == at(1, 1));
    TEST_CHECK(rig.view->composing().empty());
    rig.settle();
  }

  void testRefusedEdits(::test::SceneCapture& capture) {
    const std::string longest(editing::kEditLineBytes, 'a');
    Rig rig(capture, longest + "\nshort");
    rig.focus();
    rig.settle();
    rig.type("b");
    TEST_CHECK(rig.refusals.size() == 1 && !rig.refusals.front().empty());
    TEST_CHECK(rig.buffer.text() == longest + "\nshort" && rig.edits == 0 && rig.selection() == at(0, 0));
    std::println("refused: {}", rig.refusals.front());
    rig.clipboard.text = std::string("nul\0byte", 8);
    rig.key(XKB_KEY_Down);
    rig.key('v', kCtrl);
    TEST_CHECK(rig.refusals.size() == 2 && rig.refusals.back().contains("NUL"));
    TEST_CHECK(rig.buffer.text() == longest + "\nshort" && rig.edits == 0);
    // Joining the lines would pass the line limit too.
    rig.key(XKB_KEY_BackSpace);
    TEST_CHECK(rig.refusals.size() == 3 && rig.buffer.lineCount() == 2 && rig.selection() == at(1, 0));
  }

  TextInputEdit composing(std::string text) {
    TextInputEdit edit;
    edit.preeditText = std::move(text);
    edit.preeditCursorBegin = -1;
    edit.preeditCursorEnd = -1;
    edit.hasPreedit = true;
    return edit;
  }

  TextInputEdit committing(std::string text) {
    TextInputEdit edit;
    edit.commitText = std::move(text);
    edit.hasCommitText = true;
    edit.hasPreedit = true; // nothing composed after it
    return edit;
  }

  TextInputEdit deleting(std::uint32_t before, std::uint32_t after) {
    TextInputEdit edit;
    edit.deleteBeforeLength = before;
    edit.deleteAfterLength = after;
    edit.hasDelete = true;
    return edit;
  }

  void testInputMethod(::test::SceneCapture& capture) {
    Rig rig(capture, "ab");
    rig.focus();
    rig.settle();
    rig.key(XKB_KEY_Right);
    const auto drawn = rig.view->drawnStrips();
    const auto before = rig.view->caretRect();
    rig.view->textInputApplyEdit(composing("ka"));
    TEST_CHECK(rig.view->composing() == "ka" && rig.buffer.text() == "ab" && rig.edits == 0);
    rig.settle();
    TEST_CHECK(rig.view->drawnStrips() == drawn + 1);   // the caret's line, with the text underlined in it
    TEST_CHECK(rig.view->caretRect().x > before.x + 4); // after the composed text
    // The state an input method reads: the caret's line and the caret in it.
    const auto state = rig.view->textInputState();
    TEST_CHECK(state.surroundingText == "ab" && state.cursor == 1 && state.anchor == 1);
    TEST_CHECK(state.cursorRectHeight > 4);
    rig.view->textInputApplyEdit(committing("\u304B"));
    TEST_CHECK(rig.view->composing().empty() && rig.buffer.text() == "a\u304Bb" && rig.selection() == at(0, 4));
    TEST_CHECK(rig.edits == 1);
    // A composition that is cancelled leaves the text as it was.
    rig.view->textInputApplyEdit(composing("xyz"));
    rig.settle();
    rig.view->textInputApplyEdit(composing({}));
    TEST_CHECK(rig.view->composing().empty() && rig.buffer.text() == "a\u304Bb" && rig.edits == 1);
    rig.view->textInputApplyEdit(composing("q"));
    rig.view->textInputResetPreedit();
    TEST_CHECK(rig.view->composing().empty());
    rig.view->textInputApplyEdit(composing("q"));
    rig.dispatcher.setFocus(nullptr); // losing the keyboard drops it too
    TEST_CHECK(rig.view->composing().empty() && rig.buffer.text() == "a\u304Bb");
    rig.focus();
    // Deleting around the caret, in bytes.
    rig.view->textInputApplyEdit(deleting(3, 1));
    TEST_CHECK(rig.buffer.text() == "a" && rig.selection() == at(0, 1) && rig.edits == 2);
    // A dead key's preview shows like a composition; the key that follows replaces it.
    TEST_CHECK(rig.view->key({.sym = XKB_KEY_dead_acute, .utf32 = 0xB4, .pressed = true, .preedit = true}));
    TEST_CHECK(rig.view->composing() == "\u00B4" && rig.buffer.text() == "a");
    rig.key(XKB_KEY_eacute, 0, 0xE9);
    TEST_CHECK(rig.view->composing().empty() && rig.buffer.text() == "a\u00E9");
    rig.settle();
  }

  // The caret stays in view after every move and edit.
  void testCaretKeptInView(::test::SceneCapture& capture) {
    std::string text = numberedLines(300);
    text += "\n" + std::string(300, 'x') + " end";
    Rig rig(capture, text);
    rig.focus();
    rig.settle();
    const auto inView = [&rig] {
      const auto caret = rig.view->caretRect();
      return caret.x >= 0
          && caret.y >= 0
          && caret.x + caret.width <= static_cast<float>(rig.width)
          && caret.y + caret.height <= static_cast<float>(rig.height);
    };
    rig.key(XKB_KEY_End, kCtrl);
    TEST_CHECK(inView() && rig.view->scrollY() > 0 && rig.view->scrollX() > 0);
    TEST_CHECK(rig.view->scrollY() == rig.view->maxScrollY());
    rig.key(XKB_KEY_Home);
    TEST_CHECK(inView() && rig.view->scrollX() == 0);
    rig.key(XKB_KEY_Up);
    rig.key(XKB_KEY_Return);
    rig.type("typed");
    TEST_CHECK(inView());
    rig.view->scrollTo(0, 0);
    rig.type("more"); // typing brings the caret back
    TEST_CHECK(inView() && rig.view->scrollY() > 0);
    rig.key(XKB_KEY_Home, kCtrl);
    TEST_CHECK(inView() && rig.view->scrollY() == 0);
    rig.settle();
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
  testClickAndShiftClick(*capture);
  testDoubleAndTripleClick(*capture);
  testDragSelectsAndScrolls(*capture);
  testWheel(*capture);
  testScrollbars(*capture);
  testClipboard(*capture);
  testPasteAnswersLate(*capture);
  testPasteWhileComposing(*capture);
  testRefusedEdits(*capture);
  testInputMethod(*capture);
  testCaretKeptInView(*capture);
  std::println("code-editor-pointer: all checks passed");
  return 0;
}
