#include "code_editor_rig.h"

#include <print>
#include <string>

namespace {
  using namespace luciole;
  using namespace luciole::test::editor;
  constexpr auto kCtrl = KeyMod::Ctrl;
  constexpr auto kShift = KeyMod::Shift;

  void testArrows(::test::SceneCapture& capture) {
    Rig rig(capture, "ab\ncd");
    rig.focus();
    rig.settle();
    rig.key(XKB_KEY_Right);
    TEST_CHECK(rig.selection() == at(0, 1));
    rig.key(XKB_KEY_Right);
    rig.key(XKB_KEY_Right);
    TEST_CHECK(rig.selection() == at(1, 0)); // from a line's end onto the next line's start
    rig.key(XKB_KEY_Left);
    TEST_CHECK(rig.selection() == at(0, 2));
    rig.key(XKB_KEY_Right, kShift);
    rig.key(XKB_KEY_Right, kShift);
    TEST_CHECK((rig.selection() == span({0, 2}, {1, 1})));
    rig.key(XKB_KEY_Left); // collapses to the selection's start
    TEST_CHECK(rig.selection() == at(0, 2));
    rig.key(XKB_KEY_Left, kShift);
    rig.key(XKB_KEY_Right); // collapses to its end
    TEST_CHECK(rig.selection() == at(0, 2));
    rig.key(XKB_KEY_Left, kShift);
    rig.key(XKB_KEY_Left, kShift);
    rig.key(XKB_KEY_Right, kShift);
    TEST_CHECK((rig.selection() == span({0, 2}, {0, 1})));
    TEST_CHECK(rig.edits == 0 && rig.buffer.text() == "ab\ncd");
  }

  void testClustersAndWords(::test::SceneCapture& capture) {
    // e and a combining acute accent are one cluster.
    Rig rig(capture, "foo bar_baz e\u0301x, qux");
    rig.focus();
    rig.settle();
    rig.key(XKB_KEY_Right, kCtrl);
    TEST_CHECK(rig.selection() == at(0, 3));
    rig.key(XKB_KEY_Right, kCtrl);
    TEST_CHECK(rig.selection() == at(0, 11));
    rig.key(XKB_KEY_Left, kCtrl);
    TEST_CHECK(rig.selection() == at(0, 4));
    rig.key(XKB_KEY_Right, kCtrl | kShift);
    TEST_CHECK((rig.selection() == span({0, 4}, {0, 11})));
    rig.key(XKB_KEY_Right);
    rig.key(XKB_KEY_Right);
    rig.key(XKB_KEY_Right); // over the whole cluster
    TEST_CHECK(rig.selection() == at(0, 15));
    rig.key(XKB_KEY_Left);
    TEST_CHECK(rig.selection() == at(0, 12));
  }

  void testHomeAndEnd(::test::SceneCapture& capture) {
    Rig rig(capture, "first\n    indented line\nlast");
    rig.focus();
    rig.settle();
    rig.key(XKB_KEY_Down);
    rig.key(XKB_KEY_End);
    TEST_CHECK(rig.selection() == at(1, 17));
    rig.key(XKB_KEY_Home);
    TEST_CHECK(rig.selection() == at(1, 4)); // the first non-blank
    rig.key(XKB_KEY_Home);
    TEST_CHECK(rig.selection() == at(1, 0));
    rig.key(XKB_KEY_Home);
    TEST_CHECK(rig.selection() == at(1, 4));
    rig.key(XKB_KEY_End, kShift);
    TEST_CHECK((rig.selection() == span({1, 4}, {1, 17})));
    rig.key(XKB_KEY_End, kCtrl);
    TEST_CHECK(rig.selection() == at(2, 4));
    rig.key(XKB_KEY_Home, kCtrl | kShift);
    TEST_CHECK((rig.selection() == span({2, 4}, {0, 0})));
    rig.key(XKB_KEY_Home, kCtrl);
    TEST_CHECK(rig.selection() == at(0, 0));
  }

  // Up and Down keep the caret's place in pixels across shorter lines and tabs.
  void testVerticalKeepsPixels(::test::SceneCapture& capture) {
    Rig rig(capture, "abcdefghij\nab\nabcdefghij\n        abc\n\tabc");
    rig.focus();
    rig.settle();
    rig.key(XKB_KEY_End);
    rig.key(XKB_KEY_Left);
    rig.key(XKB_KEY_Left);
    TEST_CHECK(rig.selection() == at(0, 8));
    rig.key(XKB_KEY_Down);
    TEST_CHECK(rig.selection() == at(1, 2));
    rig.key(XKB_KEY_Down);
    TEST_CHECK(rig.selection() == at(2, 8));
    rig.key(XKB_KEY_Down);
    TEST_CHECK(rig.selection() == at(3, 8)); // before the a, after eight spaces
    rig.key(XKB_KEY_Down);
    TEST_CHECK(rig.selection() == at(4, 1)); // after the tab, which reaches as far
    rig.key(XKB_KEY_Up);
    rig.key(XKB_KEY_Up);
    rig.key(XKB_KEY_Up, kShift);
    TEST_CHECK((rig.selection() == span({2, 8}, {1, 2})));
    rig.key(XKB_KEY_Up);
    TEST_CHECK(rig.selection() == at(0, 8));
    rig.key(XKB_KEY_Up); // the first line's start
    TEST_CHECK(rig.selection() == at(0, 0));
    rig.key(XKB_KEY_End, kCtrl);
    rig.key(XKB_KEY_Home);
    rig.key(XKB_KEY_Down); // the last line's end
    TEST_CHECK(rig.selection() == at(4, 4));
    // Any other key forgets the place aimed at.
    rig.key(XKB_KEY_Home, kCtrl);
    rig.key(XKB_KEY_End);
    rig.key(XKB_KEY_Down);
    rig.key(XKB_KEY_Right);
    TEST_CHECK(rig.selection() == at(2, 0));
    rig.key(XKB_KEY_Up);
    TEST_CHECK(rig.selection() == at(1, 0));
  }

  void testPageKeys(::test::SceneCapture& capture) {
    Rig rig(capture, numberedLines(200));
    rig.focus();
    rig.settle();
    const float lineHeight = rig.view->caretRect().height;
    const auto page = static_cast<std::size_t>(rig.height / lineHeight);
    std::println("page of {} lines of {} px", page, lineHeight);
    rig.key(XKB_KEY_Right);
    rig.key(XKB_KEY_Right);
    rig.key(XKB_KEY_Page_Down);
    TEST_CHECK(rig.selection() == at(page, 2));
    TEST_CHECK(rig.view->scrollY() > 0);
    const auto top = rig.view->caretRect().y;
    rig.key(XKB_KEY_Page_Down);
    TEST_CHECK(rig.selection() == at(2 * page, 2));
    TEST_NEAR(rig.view->caretRect().y, top, 1.0); // the caret keeps its row while the text moves
    rig.key(XKB_KEY_Page_Up, kShift);
    TEST_CHECK((rig.selection() == span({2 * page, 2}, {page, 2})));
    rig.key(XKB_KEY_Page_Up);
    rig.key(XKB_KEY_Page_Up);
    TEST_CHECK(rig.selection() == at(0, 0));
    rig.key(XKB_KEY_Page_Down, kShift); // still aiming at the third column
    TEST_CHECK((rig.selection() == span({0, 0}, {page, 2})));
  }

  void testSelectAllAndTyping(::test::SceneCapture& capture) {
    Rig rig(capture, "one\ntwo");
    rig.focus();
    rig.settle();
    rig.key('a', kCtrl);
    TEST_CHECK((rig.selection() == span({0, 0}, {1, 3})));
    rig.type("x");
    TEST_CHECK(rig.buffer.text() == "x" && rig.selection() == at(0, 1));
    rig.key(XKB_KEY_eacute, 0, 0xE9);
    rig.key(XKB_KEY_A, kShift, 'A');
    TEST_CHECK(rig.buffer.text() == "x\u00E9A" && rig.selection() == at(0, 4));
    rig.key(XKB_KEY_Left, kShift);
    rig.key(XKB_KEY_Left, kShift);
    rig.type("yz");
    TEST_CHECK(rig.buffer.text() == "xyz" && rig.edits == 5);
  }

  void testErasing(::test::SceneCapture& capture) {
    Rig rig(capture, "alpha beta e\u0301\ngamma");
    rig.focus();
    rig.settle();
    rig.key(XKB_KEY_End);
    rig.key(XKB_KEY_BackSpace); // the whole cluster
    TEST_CHECK(rig.buffer.text() == "alpha beta \ngamma");
    rig.key(XKB_KEY_BackSpace, kCtrl);
    TEST_CHECK(rig.buffer.text() == "alpha \ngamma");
    rig.key(XKB_KEY_Delete); // the line break
    TEST_CHECK(rig.buffer.text() == "alpha gamma" && rig.selection() == at(0, 6));
    rig.key(XKB_KEY_Home);
    rig.key(XKB_KEY_Delete, kCtrl);
    TEST_CHECK(rig.buffer.text() == " gamma");
    rig.key(XKB_KEY_Delete);
    rig.key(XKB_KEY_End, kShift);
    rig.key(XKB_KEY_BackSpace);
    TEST_CHECK(rig.buffer.text().empty() && rig.edits == 6);
    rig.key(XKB_KEY_BackSpace); // nothing before the caret: no edit
    TEST_CHECK(rig.edits == 6);
  }

  void testEnterKeepsIndent(::test::SceneCapture& capture) {
    Rig rig(capture, "    body();");
    rig.focus();
    rig.settle();
    rig.key(XKB_KEY_End);
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.buffer.text() == "    body();\n    " && rig.selection() == at(1, 4));
    rig.key(XKB_KEY_KP_Enter);
    TEST_CHECK(rig.buffer.lineCount() == 3);
    TEST_CHECK(!rig.offer(XKB_KEY_Return, kCtrl) && rig.buffer.lineCount() == 3);
  }

  // The dispatcher moves focus on Tab before an area sees it, so the page offers Tab to key() first, as here.
  void testIndentKeys(::test::SceneCapture& capture) {
    Rig rig(capture, "x\ny\nz", "notes.txt", {.tab = false, .spaces = 2});
    rig.focus();
    rig.settle();
    TEST_CHECK(rig.offer(XKB_KEY_Tab));
    TEST_CHECK(rig.buffer.text() == "  x\ny\nz" && rig.selection() == at(0, 2));
    TEST_CHECK(rig.offer(XKB_KEY_ISO_Left_Tab, kShift));
    TEST_CHECK(rig.buffer.text() == "x\ny\nz");
    rig.key('a', kCtrl);
    TEST_CHECK(rig.offer(XKB_KEY_Tab));
    TEST_CHECK(rig.buffer.text() == "  x\n  y\n  z");
    TEST_CHECK(rig.offer(XKB_KEY_Tab, kShift));
    TEST_CHECK(rig.buffer.text() == "x\ny\nz" && rig.edits == 4);
    TEST_CHECK(!rig.offer(XKB_KEY_Tab, kCtrl));
  }

  void testUndoAndRedo(::test::SceneCapture& capture) {
    Rig rig(capture, "");
    rig.focus();
    rig.settle();
    rig.type("abc");
    rig.key(XKB_KEY_Return);
    rig.type("d");
    rig.key('z', kCtrl);
    TEST_CHECK(rig.buffer.text() == "abc\n");
    rig.key('z', kCtrl);
    rig.key('z', kCtrl);
    TEST_CHECK(rig.buffer.text().empty());
    rig.key('Z', kCtrl | kShift);
    TEST_CHECK(rig.buffer.text() == "abc");
    rig.key('y', kCtrl);
    rig.key('y', kCtrl);
    TEST_CHECK(rig.buffer.text() == "abc\nd" && rig.selection() == at(1, 1));
    const auto edits = rig.edits;
    TEST_CHECK(rig.offer('y', kCtrl)); // nothing left to redo, and still not the page's
    TEST_CHECK(rig.edits == edits);
  }

  void testSaveEscapeAndOtherKeys(::test::SceneCapture& capture) {
    Rig rig(capture, "text");
    rig.focus();
    rig.settle();
    rig.key('s', kCtrl);
    rig.key(XKB_KEY_Escape);
    TEST_CHECK(rig.saves == 1 && rig.escapes == 1 && rig.buffer.text() == "text");
    TEST_CHECK(!rig.offer(XKB_KEY_F5));
    TEST_CHECK(!rig.offer(XKB_KEY_Left, KeyMod::Alt));
    TEST_CHECK(!rig.offer(XKB_KEY_Tab, kCtrl));
    TEST_CHECK(!rig.offer('S', kCtrl | kShift));
    TEST_CHECK(!rig.offer('q', kCtrl));
    TEST_CHECK(!rig.offer('x', KeyMod::Alt, 'x'));
    TEST_CHECK(!rig.offer(XKB_KEY_Control_L, kCtrl));
    TEST_CHECK(!rig.view->key({.sym = 'a', .utf32 = 'a', .modifiers = 0, .pressed = false}));
    TEST_CHECK(rig.offer(XKB_KEY_Up, kShift));
    TEST_CHECK(rig.buffer.text() == "text" && rig.saves == 1 && rig.escapes == 1 && rig.edits == 0);
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
  testArrows(*capture);
  testClustersAndWords(*capture);
  testHomeAndEnd(*capture);
  testVerticalKeepsPixels(*capture);
  testPageKeys(*capture);
  testSelectAllAndTyping(*capture);
  testErasing(*capture);
  testEnterKeepsIndent(*capture);
  testIndentKeys(*capture);
  testUndoAndRedo(*capture);
  testSaveEscapeAndOtherKeys(*capture);
  std::println("code-editor-keys: all checks passed");
  return 0;
}
