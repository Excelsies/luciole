#include "editing/table_session.h"
#include "preview_edit_rig.h"
#include "ui/controls/table_view.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/workbench_view.h"
#include "workbench/pane_layout.h"
#include "workbench/preview_home.h"

#include <string>
#include <string_view>
#include <unistd.h>

// Editing in the preview while it is homed in the Jump ahead pane, and while the setting or the window size moves it
// between the pane, its column and nowhere: the text and the cells stay, a save writes them, and leaving them unsaved
// asks the one question.
namespace {
  using namespace luciole::test::edit;
  using luciole::TableEdit;
  using luciole::TableView;
  using luciole::WorkbenchPreviewPlace;
  using luciole::workbench::PreviewHome;

  // The view sizes of real windows (see workbench_preview_home_test.cpp): too small for a column, and roomy.
  constexpr int kSmallWidth = 883;
  constexpr int kSmallHeight = 606;
  constexpr int kRoomyWidth = 1184;
  constexpr int kRoomyHeight = 806;

  struct Folder {
    Files files;
    Folder() {
      files.write("a.txt", "alpha\n");
      files.write("b.txt", "beta\n");
      files.write("t.csv", "name,size\nalpha,1\nbeta,2\n");
    }
  };

  void small(Rig& rig) {
    rig.width = kSmallWidth;
    rig.height = kSmallHeight;
  }

  void roomy(Rig& rig) {
    rig.width = kRoomyWidth;
    rig.height = kRoomyHeight;
  }

  PreviewHome home(const Rig& rig) { return rig.session->view().previewHome(); }

  bool inJump(const Rig& rig) {
    const Node* jump = rig.session->view().shell().content(luciole::PaneRegion::Jump);
    for (const Node* at = &rig.slot(); at != nullptr; at = at->parent()) {
      if (at == jump) {
        return true;
      }
    }
    return false;
  }

  TableView* table(const Rig& rig) { return dynamic_cast<TableView*>(rig.slot().content()); }

  void frames(Rig& rig, int count) {
    for (int i = 0; i < count; ++i) {
      rig.frame();
    }
  }

  // The text editor works in the Jump ahead pane: typing, Save, the bytes.
  void textEditsAndSavesInTheJumpAheadPane(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    small(rig);
    rig.edit("a.txt");
    TEST_CHECK(home(rig) == PreviewHome::JumpAhead && inJump(rig));
    rig.type("12");
    TEST_CHECK(rig.bar().unsaved && rig.editor->unsaved());
    TEST_CHECK(folder.files.read("a.txt") == "alpha\n"); // only Save writes
    rig.saveKeys();
    TEST_CHECK(folder.files.read("a.txt") == "12alpha\n");
    TEST_CHECK(!rig.editor->unsaved() && rig.asks == 0);
  }

  // The table editor in the pane: a cell, a row, undo and the bytes of Save.
  void tableEditsAndSavesInTheJumpAheadPane(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    small(rig);
    rig.select("t.csv");
    rig.frame();
    rig.skew += luciole::WorkbenchPreview::kDwell;
    rig.frames([&] { return table(rig) != nullptr && table(rig)->table() != nullptr && rig.bar().enableAvailable; });
    TEST_CHECK(home(rig) == PreviewHome::JumpAhead && inJump(rig));
    rig.click(rig.slot().enableEditingButton());
    rig.frames([&] { return table(rig) != nullptr && table(rig)->editable() && rig.editor->textFocused(); });
    table(rig)->select({.row = 0, .column = 1});
    rig.frame();
    rig.type("9");
    TEST_CHECK(rig.key(XKB_KEY_Return));
    TEST_CHECK(rig.bar().unsaved);
    rig.click(table(rig)->editBar().button(TableEdit::InsertRowBelow));
    TEST_CHECK(table(rig)->table()->rowCount == 3);
    rig.saveKeys();
    TEST_CHECK(folder.files.read("t.csv") == "name,size\nalpha,9\nbeta,2\n,\n");
    TEST_CHECK(!rig.editor->unsaved() && rig.asks == 0);
  }

  // An edit in progress when the setting is switched live: out of sight under "hide when it does not fit", still
  // edited when the pane returns, saved with its text.
  void switchingTheSettingKeepsTheEdit(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    small(rig);
    rig.edit("a.txt");
    rig.type("1");
    rig.choices.previewPlace = WorkbenchPreviewPlace::HiddenWhenNarrow;
    rig.frames([&] { return !rig.session->shows(rig.slot()); });
    frames(rig, 5);
    TEST_CHECK(home(rig) == PreviewHome::None);
    TEST_CHECK(rig.asks == 0 && !rig.editor->gate().asking() && rig.editor->unsaved());
    TEST_CHECK(folder.files.read("a.txt") == "alpha\n");
    rig.choices.previewPlace = WorkbenchPreviewPlace::JumpAheadWhenNarrow;
    rig.frames([&] { return rig.session->shows(rig.slot()) && rig.codeEditor() != nullptr; });
    rig.frames([&] { return rig.editor->textFocused() && rig.codeEditor()->settled(); });
    TEST_CHECK(home(rig) == PreviewHome::JumpAhead && inJump(rig) && rig.asks == 0 && rig.editor->unsaved());
    rig.type("2");
    rig.saveKeys();
    TEST_CHECK(folder.files.read("a.txt") == "12alpha\n" && rig.asks == 0);
  }

  // The edit moves with the preview from its column into the Jump ahead pane and back, under every choice, without a
  // question and without losing a character.
  void theEditMovesBetweenHomes(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    roomy(rig);
    rig.edit("a.txt");
    TEST_CHECK(home(rig) == PreviewHome::Column);
    rig.type("1");
    // "always": from the column into the pane
    rig.choices.previewPlace = WorkbenchPreviewPlace::JumpAheadAlways;
    rig.frames([&] { return home(rig) == PreviewHome::JumpAhead && rig.session->shows(rig.slot()); });
    rig.frames([&] { return rig.codeEditor() != nullptr && rig.editor->textFocused() && rig.codeEditor()->settled(); });
    TEST_CHECK(inJump(rig) && rig.asks == 0 && rig.editor->unsaved());
    rig.type("2");
    // back to the column
    rig.choices.previewPlace = WorkbenchPreviewPlace::JumpAheadWhenNarrow;
    rig.frames([&] { return home(rig) == PreviewHome::Column && rig.session->shows(rig.slot()); });
    rig.frames([&] { return rig.codeEditor() != nullptr && rig.editor->textFocused() && rig.codeEditor()->settled(); });
    TEST_CHECK(!inJump(rig) && rig.asks == 0 && rig.editor->unsaved());
    // the window shrinks: the preview moves into the pane
    small(rig);
    rig.frames([&] { return home(rig) == PreviewHome::JumpAhead && rig.session->shows(rig.slot()); });
    rig.frames([&] { return rig.codeEditor() != nullptr && rig.editor->textFocused() && rig.codeEditor()->settled(); });
    TEST_CHECK(inJump(rig) && rig.asks == 0 && rig.editor->unsaved());
    rig.type("3");
    rig.saveKeys();
    TEST_CHECK(folder.files.read("a.txt") == "123alpha\n");
  }

  // The window shrinks so that the home is nowhere, under "hide when it does not fit": the edit is kept out of sight.
  void shrinkingToNowhereKeepsTheEdit(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    rig.choices.previewPlace = WorkbenchPreviewPlace::HiddenWhenNarrow;
    roomy(rig);
    rig.edit("a.txt");
    rig.type("1");
    small(rig);
    rig.frames([&] { return !rig.session->shows(rig.slot()); });
    frames(rig, 5);
    TEST_CHECK(home(rig) == PreviewHome::None);
    TEST_CHECK(rig.asks == 0 && rig.editor->unsaved() && folder.files.read("a.txt") == "alpha\n");
    roomy(rig);
    rig.frames([&] { return rig.session->shows(rig.slot()) && rig.codeEditor() != nullptr; });
    rig.frames([&] { return rig.editor->textFocused() && rig.codeEditor()->settled(); });
    TEST_CHECK(home(rig) == PreviewHome::Column && rig.asks == 0 && rig.editor->unsaved());
    rig.saveKeys();
    TEST_CHECK(folder.files.read("a.txt") == "1alpha\n");
  }

  // Collapsing Jump ahead while its pane holds the edit hides the preview; the edit is kept, and opening the pane
  // shows it again.
  void collapsingTheHomeKeepsTheEdit(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    small(rig);
    rig.edit("a.txt");
    rig.type("1");
    auto& shell = rig.session->view().shell();
    shell.setState(luciole::workbench::togglePane(shell.state(), luciole::workbench::Pane::Jump));
    rig.frames([&] { return !rig.session->shows(rig.slot()); });
    frames(rig, 5);
    TEST_CHECK(home(rig) == PreviewHome::None && rig.asks == 0 && rig.editor->unsaved());
    shell.setState(luciole::workbench::togglePane(shell.state(), luciole::workbench::Pane::Jump));
    rig.frames([&] { return rig.session->shows(rig.slot()) && rig.codeEditor() != nullptr; });
    rig.frames([&] { return rig.editor->textFocused() && rig.codeEditor()->settled(); });
    TEST_CHECK(rig.asks == 0 && rig.editor->unsaved() && home(rig) == PreviewHome::JumpAhead);
  }

  // Leaving the unsaved edit from the pane asks the one question, Save writes it and No drops it.
  void leavingFromThePaneAsksTheQuestion(::test::SceneCapture& capture) {
    for (const bool save : {true, false}) {
      const Folder folder;
      Rig rig(folder.files, capture, folder.files.folder);
      small(rig);
      rig.edit("a.txt");
      rig.type("1");
      rig.select("b.txt");
      rig.frames([&] { return rig.question.has_value(); });
      TEST_CHECK(rig.asks == 1 && rig.question->name == "a.txt" && rig.codeEditor() != nullptr && inJump(rig));
      if (save) {
        rig.answerSave();
        TEST_CHECK(folder.files.read("a.txt") == "1alpha\n");
      } else {
        rig.answerNo();
        TEST_CHECK(folder.files.read("a.txt") == "alpha\n");
      }
      rig.skew += luciole::WorkbenchPreview::kDwell;
      rig.frames([&] { return rig.offered(); });
      TEST_CHECK(rig.slot().nameLabel().text() == "b.txt" && rig.asks == 1);
    }
  }

  // The same for an edited table when the setting hides the preview and a view change asks: nothing is lost silently.
  void editedTableSurvivesTheSettingSwitch(::test::SceneCapture& capture) {
    const Folder folder;
    Rig rig(folder.files, capture, folder.files.folder);
    small(rig);
    rig.select("t.csv");
    rig.frame();
    rig.skew += luciole::WorkbenchPreview::kDwell;
    rig.frames([&] { return table(rig) != nullptr && table(rig)->table() != nullptr && rig.bar().enableAvailable; });
    rig.click(rig.slot().enableEditingButton());
    rig.frames([&] { return table(rig) != nullptr && table(rig)->editable() && rig.editor->textFocused(); });
    table(rig)->select({.row = 1, .column = 0});
    rig.frame();
    rig.type("z");
    TEST_CHECK(rig.key(XKB_KEY_Return));
    rig.choices.previewPlace = WorkbenchPreviewPlace::HiddenWhenNarrow;
    rig.frames([&] { return !rig.session->shows(rig.slot()); });
    frames(rig, 5);
    TEST_CHECK(rig.asks == 0 && rig.editor->unsaved());
    rig.choices.previewPlace = WorkbenchPreviewPlace::JumpAheadAlways;
    rig.frames([&] { return rig.session->shows(rig.slot()) && table(rig) != nullptr && table(rig)->editable(); });
    rig.frames([&] { return rig.editor->textFocused(); });
    TEST_CHECK(rig.editor->unsaved() && rig.asks == 0 && inJump(rig));
    rig.saveKeys();
    TEST_CHECK(folder.files.read("t.csv") == "name,size\nalpha,1\nz,2\n");
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  textEditsAndSavesInTheJumpAheadPane(*capture);
  tableEditsAndSavesInTheJumpAheadPane(*capture);
  switchingTheSettingKeepsTheEdit(*capture);
  theEditMovesBetweenHomes(*capture);
  shrinkingToNowhereKeepsTheEdit(*capture);
  collapsingTheHomeKeepsTheEdit(*capture);
  leavingFromThePaneAsksTheQuestion(*capture);
  editedTableSurvivesTheSettingSwitch(*capture);
  std::println("preview-edit-place: all checks passed");
  return 0;
}
