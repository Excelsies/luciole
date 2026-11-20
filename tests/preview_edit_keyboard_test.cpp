#include "app/page/preview_edit_keys.h"
#include "preview_edit_rig.h"
#include "thumbnails/sheet_preview.h"
#include "ui/controls/pane_divider.h"
#include "ui/controls/table_view.h"

#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

// The keyboard while a file is edited in the column preview: which page shortcuts leave the file, which keys stay
// with the editor, the keyboard staying where it was through a click anywhere in the pane that is no text field, and
// the facts line describing the saved file.
namespace {
  using namespace luciole::test::edit;
  using luciole::TableEdit;
  using luciole::TableView;

  TableView* table(const Rig& rig) { return dynamic_cast<TableView*>(rig.slot().content()); }

  void editTable(Rig& rig, std::string_view name) {
    rig.select(name);
    rig.frame();
    rig.skew += luciole::WorkbenchPreview::kDwell;
    rig.frames([&] {
      const auto* shown = table(rig);
      return shown != nullptr && shown->table() != nullptr && !shown->editable() && rig.bar().enableAvailable;
    });
    rig.click(rig.slot().enableEditingButton());
    rig.frames([&] {
      const auto* shown = table(rig);
      return shown != nullptr && shown->editable() && rig.editor->textFocused();
    });
  }

  // A press and a release at a scene point, with the pointer moved there first.
  void clickAt(Rig& rig, float x, float y) {
    rig.dispatcher.pointerEnter(x, y, ++rig.serial);
    (void)rig.dispatcher.pointerButton(x, y, BTN_LEFT, true, ++rig.serial, 0, false);
    (void)rig.dispatcher.pointerButton(x, y, BTN_LEFT, false, ++rig.serial, 0, false);
    rig.frame();
  }

  void testLeavingKeys() {
    struct Chord {
      std::uint32_t sym;
      std::uint32_t modifiers;
      bool leaves;
    };
    constexpr auto kCtrl = KeyMod::Ctrl;
    const std::vector<Chord> chords{
        // The view modes and the keys that go to another folder leave the file.
        {'1', kCtrl, true},
        {'2', kCtrl, true},
        {'3', kCtrl, true},
        {'4', kCtrl, true},
        {XKB_KEY_Left, KeyMod::Alt, true},
        {XKB_KEY_Right, KeyMod::Alt, true},
        {XKB_KEY_Up, KeyMod::Alt, true},
        {XKB_KEY_Home, KeyMod::Alt, true},
        // The keys that edit, move or save stay with the editor.
        {'z', kCtrl, false},
        {'z', kCtrl | KeyMod::Shift, false},
        {'y', kCtrl, false},
        {'c', kCtrl, false},
        {'x', kCtrl, false},
        {'v', kCtrl, false},
        {'a', kCtrl, false},
        {'s', kCtrl, false},
        {'h', kCtrl, false},
        {XKB_KEY_Delete, 0, false},
        {XKB_KEY_BackSpace, 0, false},
        {XKB_KEY_Return, 0, false},
        {XKB_KEY_Escape, 0, false},
        {XKB_KEY_Left, 0, false},
        {XKB_KEY_Home, 0, false},
        {XKB_KEY_Left, kCtrl, false},
        {XKB_KEY_F2, 0, false},
        {XKB_KEY_F5, 0, false},
        {' ', 0, false},
        {'a', 0, false},
        {'2', 0, false},
        {'2', KeyMod::Alt, false},
    };
    for (const auto& chord : chords) {
      TEST_CHECK(luciole::leavesEditedFile(chord.sym, chord.modifiers) == chord.leaves);
      // The page's shortcuts: an editor with the keyboard keeps every key that does not leave the file.
      TEST_CHECK(luciole::textHasKey(false, true, chord.sym, chord.modifiers) == !chord.leaves);
      // The location field keeps every key, the ones that leave the file included.
      TEST_CHECK(luciole::textHasKey(true, false, chord.sym, chord.modifiers));
      TEST_CHECK(luciole::textHasKey(true, true, chord.sym, chord.modifiers));
      // With no text field focused no key is kept from the page.
      TEST_CHECK(!luciole::textHasKey(false, false, chord.sym, chord.modifiers));
    }
  }

  // The editor passes the keys that leave the file on to the page, and keeps the ones that edit.
  void testEditorPassesLeavingKeys(::test::SceneCapture& capture) {
    const Files files;
    files.write("a.txt", "alpha\n");
    files.write("t.csv", "name,size\nalpha,1\nbeta,2\n");
    Rig rig(files, capture, files.folder);
    rig.edit("a.txt");
    rig.type("x");
    for (const char view : {'1', '2', '3', '4'}) {
      TEST_CHECK(!rig.key(static_cast<std::uint32_t>(view), KeyMod::Ctrl));
    }
    TEST_CHECK(!rig.key(XKB_KEY_Left, KeyMod::Alt) && !rig.key(XKB_KEY_Up, KeyMod::Alt));
    TEST_CHECK(rig.key('z', KeyMod::Ctrl) && !rig.editor->unsaved());
    TEST_CHECK(rig.key('a', KeyMod::Ctrl) && rig.key(XKB_KEY_Delete) && rig.editor->unsaved());
    TEST_CHECK(rig.key('z', KeyMod::Ctrl) && !rig.editor->unsaved());
    rig.key(XKB_KEY_Escape);
    rig.frames([&] { return rig.offered(); });

    // The grid, and the cell field, which takes every key but Ctrl+S.
    editTable(rig, "t.csv");
    auto* view = table(rig);
    TEST_CHECK(!rig.key('2', KeyMod::Ctrl) && !rig.key(XKB_KEY_Left, KeyMod::Alt));
    TEST_CHECK(rig.key('q', 0, 'q') && view->editingCell());
    TEST_CHECK(!rig.key('2', KeyMod::Ctrl) && !rig.key(XKB_KEY_Home, KeyMod::Alt));
    TEST_CHECK(view->editingCell());
    TEST_CHECK(rig.key('z', KeyMod::Ctrl) && rig.key('a', KeyMod::Ctrl) && rig.key('r', 0, 'r'));
    TEST_CHECK(view->editingCell());
  }

  // After a click, typing still edits the text: the click leaves the keyboard with the editor.
  void expectTyping(Rig& rig, std::string_view what) {
    if (!rig.editor->textFocused()) {
      std::println("lost the keyboard: {}", what);
    }
    TEST_CHECK(rig.editor->textFocused());
    TEST_CHECK(!rig.editor->unsaved());
    rig.type("W");
    if (!rig.editor->unsaved()) {
      std::println("typing did nothing: {}", what);
    }
    TEST_CHECK(rig.editor->unsaved());
    TEST_CHECK(rig.key('z', KeyMod::Ctrl) && !rig.editor->unsaved());
  }

  void testClicksKeepKeyboardInText(::test::SceneCapture& capture) {
    const Files files;
    files.write("mixed.txt", "a\r\nb\r\nc\nd\r\n");
    Rig rig(files, capture, files.folder);
    rig.edit("mixed.txt");
    TEST_CHECK(!rig.bar().notice.empty());
    auto& slot = rig.slot();
    const std::vector<std::pair<const Node*, std::string_view>> targets{
        {&slot.smallerButton(), "smaller"}, {&slot.sizeButton(), "size step"},   {&slot.largerButton(), "larger"},
        {&slot.openButton(), "open"},       {&slot.nameLabel(), "name"},         {&slot.kindLabel(), "kind"},
        {&slot.factsLabel(), "facts"},      {&slot.saveButton(), "dimmed save"}, {&slot.noticeLabel(), "notice"},
    };
    for (const auto& [node, what] : targets) {
      rig.click(*node);
      expectTyping(rig, what);
    }
    // The padding left of the header, where no divider is.
    float x = 0;
    float y = 0;
    Node::absolutePosition(&slot, x, y);
    y += slot.headerHeight() / 2;
    float padding = -1;
    for (float inset = 1; inset <= 7 && padding < 0; ++inset) {
      const auto* hit = Node::hitTest(&rig.session->view(), x + inset, y);
      if (dynamic_cast<const PaneDivider*>(hit) == nullptr && hit != nullptr && hit->parent() != nullptr) {
        padding = inset;
      }
    }
    TEST_CHECK(padding > 0);
    clickAt(rig, x + padding, y);
    expectTyping(rig, "left padding");
    // The unsaved mark, shown while text is unsaved.
    rig.type("W");
    rig.click(slot.unsavedLabel());
    TEST_CHECK(rig.editor->textFocused() && rig.editor->unsaved());
    // The enabled Save writes and keeps the editor.
    rig.click(slot.saveButton());
    rig.frames([&] { return !rig.editor->saving(); });
    TEST_CHECK(files.read("mixed.txt") == "Wa\r\nb\r\nc\r\nd\r\n");
    expectTyping(rig, "save");
    // A click outside the preview still takes the keyboard away from the editor.
    clickAt(rig, 3, 3);
    TEST_CHECK(!rig.editor->textFocused());
  }

  void testClicksKeepKeyboardInTable(::test::SceneCapture& capture) {
    const Files files;
    files.write("t.csv", "name,size\nalpha,1\nbeta,2\n");
    Rig rig(files, capture, files.folder);
    editTable(rig, "t.csv");
    auto* view = table(rig);
    auto& bar = view->editBar();
    const auto cellIsTable = [&](std::string_view what) {
      if (!rig.editor->textFocused() || rig.dispatcher.focusedArea() != &view->canvas()) {
        std::println("lost the grid's keyboard: {}", what);
      }
      TEST_CHECK(rig.editor->textFocused() && rig.dispatcher.focusedArea() == &view->canvas());
    };
    // Nothing to undo or redo yet: both are dimmed.
    rig.click(bar.button(TableEdit::Undo));
    cellIsTable("dimmed undo");
    float x = 0;
    float y = 0;
    Node::absolutePosition(&bar, x, y);
    clickAt(rig, x + (bar.width() * 3 / 4), y + (bar.height() / 2));
    cellIsTable("bar beside its buttons");
    for (const Node* node : std::initializer_list<const Node*>{
             &rig.slot().smallerButton(), &rig.slot().largerButton(), &rig.slot().factsLabel()
         }) {
      rig.click(*node);
      cellIsTable("header");
    }

    // A cell edit, then a click on the dimmed Redo: Ctrl+Z undoes the cell, not a file operation of the page.
    view->select({.row = 0, .column = 1});
    rig.frame();
    rig.type("9");
    TEST_CHECK(view->editingCell());
    TEST_CHECK(rig.key(XKB_KEY_Return) && rig.editor->unsaved());
    TEST_CHECK(!bar.available(TableEdit::Redo));
    rig.click(bar.button(TableEdit::Redo));
    cellIsTable("dimmed redo");
    TEST_CHECK(rig.key('z', KeyMod::Ctrl)); // the table took it: the page's own Undo never saw it
    TEST_CHECK(!rig.editor->unsaved() && bar.available(TableEdit::Redo));
    TEST_CHECK(files.read("t.csv") == "name,size\nalpha,1\nbeta,2\n");
    // An open cell field keeps the keyboard through a click on a dimmed button too.
    view->select({.row = 1, .column = 0});
    rig.frame();
    rig.type("z");
    TEST_CHECK(view->editingCell());
    rig.click(rig.slot().factsLabel());
    TEST_CHECK(view->editingCell() && rig.editor->textFocused());
    TEST_CHECK(rig.dispatcher.focusedArea() == &view->canvas().editor().fieldArea());
    TEST_CHECK(rig.key(XKB_KEY_Return) && rig.editor->unsaved());
  }

  // The facts line describes the file as saved: the lines or the rows and columns follow every save.
  void testFactsFollowTheSave(::test::SceneCapture& capture) {
    const Files files;
    files.write("a.txt", "a\nb\nc\n");
    files.write("t.csv", "name,size\nalpha,1\nbeta,2\n");
    Rig rig(files, capture, files.folder);
    rig.edit("a.txt");
    TEST_CHECK(rig.slot().factsLabel().text().contains("3 lines"));
    rig.type("x");
    (void)rig.key(XKB_KEY_Return);
    (void)rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.slot().factsLabel().text().contains("3 lines")); // unsaved text is not the file
    rig.saveKeys();
    TEST_CHECK(files.read("a.txt") == "x\n\na\nb\nc\n");
    TEST_CHECK(rig.slot().factsLabel().text().contains("5 lines"));
    const auto generation = rig.browser.model().snapshot().generation;
    rig.frames([&] { return rig.browser.model().snapshot().generation != generation; }); // the listing sees it
    rig.idle();
    TEST_CHECK(rig.slot().factsLabel().text().contains("5 lines"));
    rig.type("y");
    (void)rig.key(XKB_KEY_Return);
    rig.saveKeys();
    TEST_CHECK(files.read("a.txt") == "x\n\ny\na\nb\nc\n");
    TEST_CHECK(rig.slot().factsLabel().text().contains("6 lines"));
    rig.click(rig.slot().stopEditingButton());
    rig.frames([&] { return rig.offered(); });
    TEST_CHECK(rig.slot().factsLabel().text().contains("6 lines"));

    editTable(rig, "t.csv");
    TEST_CHECK(rig.slot().factsLabel().text().contains("2 rows, 2 columns"));
    table(rig)->select({.row = 0, .column = 0});
    rig.click(table(rig)->editBar().button(TableEdit::InsertRowBelow));
    rig.click(table(rig)->editBar().button(TableEdit::InsertColumnRight));
    TEST_CHECK(rig.slot().factsLabel().text().contains("2 rows, 2 columns"));
    rig.saveKeys();
    rig.frames([&] { return rig.slot().factsLabel().text().contains("3 rows, 3 columns"); });
  }
  // "Unsaved changes" shows whole in the column, under the buttons when it does not fit beside them.
  void testUnsavedMarkShowsWhole(::test::SceneCapture& capture) {
    const Files files;
    files.write("a.txt", "alpha\nbeta\n");
    Rig rig(files, capture, files.folder);
    rig.edit("a.txt");
    rig.type("x");
    rig.idle();
    auto& slot = rig.slot();
    TEST_CHECK(slot.unsavedLabel().visible() && slot.unsavedLabel().text() == "Unsaved changes");
    float labelX = 0;
    float labelY = 0;
    Node::absolutePosition(&slot.unsavedLabel(), labelX, labelY);
    float slotX = 0;
    float slotY = 0;
    Node::absolutePosition(&slot, slotX, slotY);
    TEST_CHECK(labelX + slot.unsavedLabel().width() <= slotX + slot.width() + 0.5F);
    TEST_CHECK(slot.unsavedLabel().width() >= 60);
    keepForViewing(rig.pixels, "preview-edit-unsaved.png");
  }

  float contentTop(const Rig& rig) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(rig.slot().content(), x, y);
    return y;
  }

  // Nothing below the preview header moves when the unsaved mark appears or goes, for the text editor and the table
  // editor, in a slot of any width.
  void testUnsavedMarkMovesNothing(::test::SceneCapture& capture) {
    const Files files;
    files.write("a.txt", "alpha\nbeta\n");
    files.write("t.csv", "name,size\nalpha,1\nbeta,2\n");
    for (const int width : {1600, 1400, 1100}) {
      Rig rig(files, capture, files.folder);
      rig.width = width;
      rig.edit("a.txt");
      rig.idle();
      TEST_CHECK(!rig.editor->unsaved());
      const float clean = contentTop(rig);
      rig.type("x");
      rig.idle();
      TEST_CHECK(rig.editor->unsaved() && rig.slot().unsavedLabel().opacity() > 0.0F);
      TEST_NEAR(contentTop(rig), clean, 0.01F);
      rig.saveKeys();
      rig.idle();
      TEST_CHECK(!rig.editor->unsaved());
      TEST_NEAR(contentTop(rig), clean, 0.01F);
      rig.click(rig.slot().stopEditingButton());
      rig.frames([&] { return rig.offered(); });

      editTable(rig, "t.csv");
      rig.idle();
      const float tableClean = contentTop(rig);
      table(rig)->select({.row = 0, .column = 0});
      rig.click(table(rig)->editBar().button(TableEdit::InsertRowBelow));
      rig.idle();
      TEST_CHECK(rig.editor->unsaved() && rig.slot().unsavedLabel().opacity() > 0.0F);
      TEST_NEAR(contentTop(rig), tableClean, 0.01F);
      rig.saveKeys();
      rig.idle();
      TEST_CHECK(!rig.editor->unsaved());
      TEST_NEAR(contentTop(rig), tableClean, 0.01F);
      rig.click(rig.slot().stopEditingButton());
      rig.frames([&] { return table(rig) != nullptr && !table(rig)->editable() && rig.bar().enableAvailable; });
    }
  }
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (!args.empty() && args.front() == "--sheet-preview-worker") {
    return luciole::runSheetPreviewWorker(std::span(args).subspan(1));
  }
  if (::geteuid() == 0) {
    return 77;
  }
  testLeavingKeys();
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  testEditorPassesLeavingKeys(*capture);
  testClicksKeepKeyboardInText(*capture);
  testClicksKeepKeyboardInTable(*capture);
  testFactsFollowTheSave(*capture);
  testUnsavedMarkShowsWhole(*capture);
  testUnsavedMarkMovesNothing(*capture);
  std::println("preview-edit-keyboard: all checks passed");
  return 0;
}
