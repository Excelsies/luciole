#include "preview_edit_rig.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/workbench_view.h"
#include "workbench/pane_layout.h"

#include <string>
#include <unistd.h>

// Leaving a file edited in the column preview: the one question, its answers, saves that fail inside it, changes of
// target while it is open, the resting pointer, a hidden page and another view mode.
namespace {
  using namespace luciole::test::edit;
  using luciole::WorkbenchEntry;

  struct Folder {
    Files files;
    Folder() {
      files.write("a.txt", "alpha\n");
      files.write("b.txt", "beta\n");
      files.write("c.txt", "gamma\n");
    }
  };

  // Another file takes the cursor: the editor stays until the question is answered, then the preview shows it.
  void testLeaveAnsweredSave(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    rig.select("b.txt");
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(rig.asks == 1 && rig.question->name == "a.txt" && rig.question->sentence.empty());
    TEST_CHECK(rig.codeEditor() != nullptr && rig.slot().nameLabel().text() == "a.txt"); // held until answered
    TEST_CHECK(!rig.bar().saveAvailable); // the question has the Save now
    rig.answerSave();
    TEST_CHECK(folder.files.read("a.txt") == "1alpha\n");
    TEST_CHECK(!rig.editor->gate().editing() && rig.dismissals == 1 && !rig.question);
    rig.skew += luciole::WorkbenchPreview::kDwell;
    rig.frames([&] { return rig.offered(); });
    TEST_CHECK(rig.slot().nameLabel().text() == "b.txt");
  }

  void testLeaveAnsweredNo(::test::SceneCapture& capture) {
    const Folder folder;
    const auto before = revisionOf(folder.files.folder / "a.txt");
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    rig.select("b.txt");
    rig.frames([&] { return rig.question.has_value(); });
    rig.answerNo();
    TEST_CHECK(!rig.editor->gate().editing() && !rig.editor->unsaved());
    TEST_CHECK(revisionOf(folder.files.folder / "a.txt") == before);
    rig.skew += luciole::WorkbenchPreview::kDwell;
    rig.frames([&] { return rig.offered(); });
    TEST_CHECK(rig.slot().nameLabel().text() == "b.txt" && rig.asks == 1);
  }

  // A clean editor closes without a question when the target moves.
  void testCleanLeaveAsksNothing(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.select("b.txt");
    rig.skew += luciole::WorkbenchPreview::kDwell;
    rig.frames([&] { return rig.offered(); });
    TEST_CHECK(rig.asks == 0 && !rig.editor->gate().editing());
    TEST_CHECK(rig.slot().nameLabel().text() == "b.txt");
  }

  // Closing the tab, the pane or the window asks through the page seam and goes on after the answer.
  void testPageLeave(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    int resumed = 0;
    TEST_CHECK(rig.editor->leave([&] { ++resumed; })); // nothing edited: the page may go
    rig.edit("a.txt");
    TEST_CHECK(rig.editor->leave([&] { ++resumed; })); // a clean editor: the page may go
    rig.edit("a.txt");
    rig.type("1");
    TEST_CHECK(!rig.editor->leave([&] { ++resumed; }));
    rig.frames([&] { return rig.question.has_value(); });
    rig.answerSave();
    TEST_CHECK(resumed == 1 && !rig.editor->unsaved());
    TEST_CHECK(folder.files.read("a.txt") == "1alpha\n");

    rig.frames([&] { return rig.offered(); });
    rig.edit("a.txt");
    rig.type("2");
    TEST_CHECK(!rig.editor->leave([&] { ++resumed; }));
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(resumed == 1); // nothing goes on before the answer
    rig.answerNo();
    TEST_CHECK(resumed == 2 && !rig.editor->unsaved());
    TEST_CHECK(folder.files.read("a.txt") == "1alpha\n");
  }

  // A save that fails inside the question keeps it open with the sentence; nothing goes on until another answer.
  void testFailedSaveInsideTheQuestion(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    int resumed = 0;
    rig.edit("a.txt");
    rig.type("1");
    TEST_CHECK(!rig.editor->leave([&] { ++resumed; }));
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(::chmod(folder.files.folder.c_str(), 0555) == 0);
    rig.answerSave();
    TEST_CHECK(::chmod(folder.files.folder.c_str(), 0755) == 0);
    TEST_CHECK(rig.question.has_value() && rig.told.size() == 1 && rig.told.back().contains("read-only"));
    TEST_CHECK(rig.question && rig.question->sentence == rig.told.back());
    TEST_CHECK(rig.editor->unsaved() && resumed == 0 && rig.asks == 1);
    TEST_CHECK(rig.bar().notice.empty()); // the sentence is in the question, not the bar
    rig.answerSave();                     // the folder is writable again
    TEST_CHECK(resumed == 1 && !rig.question && folder.files.read("a.txt") == "1alpha\n");

    // A file that changed on disk, then a hard link, each refused inside the question.
    rig.frames([&] { return rig.offered(); });
    rig.edit("b.txt");
    rig.type("2");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    folder.files.write("b.txt", "written elsewhere\n");
    rig.select("c.txt");
    rig.frames([&] { return rig.question.has_value(); });
    rig.answerSave();
    TEST_CHECK(rig.question && rig.question->sentence.contains("changed on disk"));
    TEST_CHECK(folder.files.read("b.txt") == "written elsewhere\n");
    rig.answerNo();
    rig.skew += luciole::WorkbenchPreview::kDwell;
    rig.frames([&] { return rig.offered(); });
    TEST_CHECK(rig.slot().nameLabel().text() == "c.txt");

    rig.edit("c.txt");
    rig.type("3");
    fs::create_hard_link(folder.files.folder / "c.txt", folder.files.folder / "c-link.txt");
    rig.key(XKB_KEY_Escape);
    rig.frames([&] { return rig.question.has_value(); });
    rig.answerSave();
    TEST_CHECK(rig.question && rig.question->sentence.contains("hard links"));
    rig.answerNo();
    TEST_CHECK(folder.files.read("c.txt") == "gamma\n" && !rig.editor->gate().editing());
  }

  // Further changes of target while the question is open ask nothing more; the answer shows the latest target.
  void testTwoTargetChangesDuringOneQuestion(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    rig.select("b.txt");
    rig.frames([&] { return rig.question.has_value(); });
    rig.select("c.txt");
    for (int i = 0; i < 5; ++i) {
      rig.frame();
    }
    rig.select("b.txt");
    for (int i = 0; i < 5; ++i) {
      rig.frame();
    }
    rig.select("c.txt");
    rig.frame();
    TEST_CHECK(rig.asks == 1 && rig.codeEditor() != nullptr);
    rig.answerNo();
    rig.skew += luciole::WorkbenchPreview::kDwell;
    rig.frames([&] { return rig.offered(); });
    TEST_CHECK(rig.slot().nameLabel().text() == "c.txt" && rig.asks == 1);
  }

  // Escape and Stop Editing take the same path and end on the same file, read-only and freshly loaded, with the
  // keyboard back in the list. A save that replaced the file leaves the list's cursor on it.
  void testStopEditing(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    const auto cursorOn = [&](std::string_view name) {
      const auto cursor = luciole::workbenchFeed(rig.browser).cursor;
      return cursor && cursor->location.basename() == name;
    };
    rig.edit("a.txt");
    rig.key(XKB_KEY_Escape); // nothing unsaved: the editor closes at once
    rig.frames([&] { return rig.offered(); });
    TEST_CHECK(rig.asks == 0 && rig.dispatcher.focusedArea() == rig.listFocus);

    rig.edit("a.txt");
    rig.type("1");
    rig.key(XKB_KEY_Escape);
    rig.frames([&] { return rig.question.has_value(); });
    rig.answerSave();
    TEST_CHECK(rig.dispatcher.focusedArea() == rig.listFocus);
    rig.skew += luciole::WorkbenchPreview::kDwell;
    const auto rows = rig.browser.model().snapshot().generation;
    // the listing sees the save after the editor went
    rig.frames([&] { return rig.offered() && rig.browser.model().snapshot().generation != rows; });
    TEST_CHECK(rig.slot().nameLabel().text() == "a.txt" && folder.files.read("a.txt") == "1alpha\n");
    TEST_CHECK(cursorOn("a.txt"));
    rig.edit("a.txt");
    rig.type("2");
    rig.click(rig.slot().stopEditingButton());
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(rig.asks == 2);
    rig.answerNo();
    TEST_CHECK(rig.dispatcher.focusedArea() == rig.listFocus);
    rig.skew += luciole::WorkbenchPreview::kDwell;
    rig.frames([&] { return rig.offered(); });
    TEST_CHECK(rig.slot().nameLabel().text() == "a.txt" && folder.files.read("a.txt") == "1alpha\n");
    TEST_CHECK(rig.tiles() != nullptr && rig.preview->loadsStarted() >= 4);
    TEST_CHECK(rig.reveals == 0 && cursorOn("a.txt"));

    // Once editing ended, a cursor that goes away takes the target with it.
    TEST_CHECK(rig.browser.model().setFocus(std::nullopt));
    rig.frames([&] { return !rig.session->state().target(); });
  }

  // A slot the layout hides, as in a narrower window, is no leave: the editor keeps its text and shows again with the
  // slot. The keyboard goes to the list meanwhile and comes back to the editor.
  void testNarrowWindowKeepsTheEditor(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    rig.width = 480;
    rig.frames([&] { return !rig.session->shows(rig.slot()); });
    for (int i = 0; i < 5; ++i) {
      rig.frame();
    }
    TEST_CHECK(rig.asks == 0 && !rig.editor->gate().asking() && rig.editor->unsaved());
    TEST_CHECK(rig.dispatcher.focusedArea() == rig.listFocus);
    rig.width = kWidth;
    rig.frames([&] { return rig.session->shows(rig.slot()) && rig.codeEditor() != nullptr; });
    rig.frames([&] { return rig.editor->textFocused() && rig.codeEditor()->settled(); });
    TEST_CHECK(rig.asks == 0 && rig.editor->unsaved());
    rig.type("2");
    rig.saveKeys();
    TEST_CHECK(folder.files.read("a.txt") == "12alpha\n" && rig.asks == 0);
  }

  // The expanded instrument covers the slot: that leaves the file.
  void testExpandedInstrumentLeaves(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    auto& shell = rig.session->view().shell();
    shell.setState(luciole::workbench::withExpanded(shell.state(), true));
    rig.frames([&] { return rig.question.has_value(); });
    rig.answerNo();
    TEST_CHECK(!rig.editor->gate().editing() && folder.files.read("a.txt") == "alpha\n");
  }

  // A save that fails after the view went asks for the frame that shows the question.
  void testDetachedSaveFailureAsksForAFrame(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    folder.files.write("a.txt", "written elsewhere\n");
    // Ctrl+S, then another view mode before the main context delivers the save's answer.
    TEST_CHECK(
        rig.editor->key({.sym = 's', .utf32 = 's', .modifiers = KeyMod::Ctrl, .pressed = true, .preedit = false})
    );
    TEST_CHECK(rig.editor->saving());
    rig.unmount();
    const int before = rig.frameRequests;
    rig.until([&] { return !rig.editor->saving(); });
    TEST_CHECK(rig.editor->gate().asking() && !rig.question && rig.frameRequests > before);
    rig.frame();
    TEST_CHECK(rig.question && rig.question->sentence.contains("changed on disk"));
    rig.answerNo();
    TEST_CHECK(folder.files.read("a.txt") == "written elsewhere\n");
    rig.mount();
  }

  // While a file is edited, a resting pointer does not retarget the columns.
  void testFrozenPointer(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    auto& state = rig.session->state();
    const WorkbenchEntry other{.location = luciole::Location::fromPath((folder.files.folder / "c.txt").native())};
    const auto now = luciole::WorkbenchState::Clock::now();
    state.pointerInList(other, 10, 40, now);
    TEST_CHECK(!state.deadline());
    state.advance(now + std::chrono::seconds(1));
    state.pointerLeft(now + std::chrono::seconds(1));
    state.advance(now + std::chrono::seconds(2));
    for (int i = 0; i < 5; ++i) {
      rig.frame();
    }
    TEST_CHECK(rig.asks == 0 && rig.codeEditor() != nullptr && rig.editor->unsaved());
    TEST_CHECK(state.target() && state.target()->location.basename() == "a.txt");
    rig.key(XKB_KEY_Escape);
    rig.frames([&] { return rig.question.has_value(); });
    rig.answerNo();
    rig.frame();
    state.pointerInList(other, 10, 40, now + std::chrono::seconds(3));
    TEST_CHECK(state.deadline().has_value()); // the pointer rests again once the editor is gone
  }

  // A hidden page keeps its editor and its text; the editor's textures go while hidden and come back after.
  void testHideAndShow(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("abc");
    auto* view = rig.codeEditor();
    const auto caret = view->caretRect();
    TEST_CHECK(view->residentStrips() > 0);
    rig.dispatcher.setFocus(nullptr); // the page drops its keyboard focus as it goes out of sight
    rig.preview->hide();
    rig.pump();
    TEST_CHECK(view->residentStrips() == 0 && view->textureBytes() == 0 && !view->blinking());
    TEST_CHECK(rig.asks == 0 && rig.editor->unsaved());
    rig.frames([&] { return rig.codeEditor() == view && view->settled() && view->residentStrips() > 0; });
    TEST_CHECK(rig.asks == 0 && rig.editor->unsaved());
    TEST_CHECK(rig.editor->textFocused()); // the keyboard comes back to the editor
    TEST_CHECK(view->caretRect().x == caret.x && view->caretRect().y == caret.y);
    rig.type("d");
    rig.saveKeys();
    TEST_CHECK(folder.files.read("a.txt") == "abcdalpha\n");
  }

  // Another view mode: the view goes at once and the question asks from the model; Save still writes.
  void testAnotherViewMode(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    rig.unmount();
    rig.frame();
    TEST_CHECK(rig.question.has_value() && rig.asks == 1 && rig.question->name == "a.txt");
    TEST_CHECK(rig.editor->unsaved() && !rig.editor->textFocused());
    rig.answerSave();
    TEST_CHECK(folder.files.read("a.txt") == "1alpha\n" && !rig.editor->gate().editing());

    rig.mount();
    rig.edit("b.txt");
    rig.type("2");
    rig.mayAsk = false; // the page cannot show its question yet
    rig.unmount();
    rig.frame();
    TEST_CHECK(!rig.question.has_value());
    rig.mayAsk = true;
    rig.frame();
    TEST_CHECK(rig.question.has_value());
    rig.answerNo();
    TEST_CHECK(folder.files.read("b.txt") == "beta\n" && !rig.editor->unsaved());
    rig.mount();
  }

  // The page went inactive while its question was open: the question is kept and shown again, and the popup's
  // close meanwhile is no answer.
  void testKeptQuestion(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.edit("a.txt");
    rig.type("1");
    rig.select("b.txt");
    rig.frames([&] { return rig.question.has_value(); });
    rig.editor->keepQuestion();
    rig.mayAsk = false;
    if (auto shown = std::exchange(rig.question, std::nullopt)) {
      shown->closed(); // the page closes its popup on deactivation
    }
    rig.frame();
    TEST_CHECK(rig.editor->unsaved() && rig.editor->gate().asking() && rig.asks == 1);
    rig.mayAsk = true;
    rig.frame();
    TEST_CHECK(rig.question.has_value() && rig.asks == 2);
    rig.answerSave();
    TEST_CHECK(folder.files.read("a.txt") == "1alpha\n");
  }
} // namespace

int main() {
  if (::geteuid() == 0) {
    return 77; // root writes into read-only folders, so the refused save cannot be shown
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  testLeaveAnsweredSave(*capture);
  testLeaveAnsweredNo(*capture);
  testCleanLeaveAsksNothing(*capture);
  testPageLeave(*capture);
  testFailedSaveInsideTheQuestion(*capture);
  testTwoTargetChangesDuringOneQuestion(*capture);
  testStopEditing(*capture);
  testNarrowWindowKeepsTheEditor(*capture);
  testExpandedInstrumentLeaves(*capture);
  testDetachedSaveFailureAsksForAFrame(*capture);
  testFrozenPointer(*capture);
  testHideAndShow(*capture);
  testAnotherViewMode(*capture);
  testKeptQuestion(*capture);
  std::println("preview-edit-leave: all checks passed");
  return 0;
}
