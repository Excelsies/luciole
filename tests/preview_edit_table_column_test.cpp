#include "editing/table_session.h"
#include "preview_edit_rig.h"
#include "table_fixture.h"
#include "thumbnails/sheet_preview.h"
#include "ui/controls/input.h"
#include "ui/controls/table_view.h"

#include <format>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

// Editing CSV and TSV files as tables in the column preview: Enable editing, cells, rows and columns, Save and the
// bytes on disk, the reasons a table cannot be edited, a refused change, and leaving unsaved changes.
namespace {
  using namespace luciole::test::edit;
  using luciole::TableEdit;
  using luciole::TableView;
  namespace formats = luciole::formats;

  TableView* table(const Rig& rig) { return dynamic_cast<TableView*>(rig.slot().content()); }

  // The file is shown read-only as a table, and its edit bar says whether it can be edited.
  bool offeredTable(const Rig& rig) {
    const auto* shown = table(rig);
    return shown != nullptr
        && shown->table() != nullptr
        && !shown->editable()
        && rig.bar().shown
        && !rig.bar().editing
        && (rig.bar().enableAvailable || !rig.bar().reason.empty());
  }

  void visitTable(Rig& rig, std::string_view name) {
    rig.select(name);
    rig.frame();
    rig.skew += luciole::WorkbenchPreview::kDwell;
    rig.frames([&] { return offeredTable(rig); });
    TEST_CHECK(rig.slot().nameLabel().text() == name);
  }

  // Visits `name`, presses Enable editing and frames until the table is editable with the keyboard on its grid.
  void editTable(Rig& rig, std::string_view name) {
    visitTable(rig, name);
    TEST_CHECK(rig.bar().enableAvailable);
    rig.click(rig.slot().enableEditingButton());
    rig.frames([&] {
      const auto* shown = table(rig);
      return shown != nullptr && shown->editable() && rig.editor->textFocused();
    });
    TEST_CHECK(rig.bar().editing && !rig.bar().unsaved && !rig.bar().saveAvailable);
  }

  // Types into the field of a grid cell, replacing its text, without committing it.
  void typeInCell(Rig& rig, std::size_t row, std::size_t column, std::string_view text) {
    table(rig)->select({.row = row, .column = column});
    rig.frame();
    rig.type(text);
    TEST_CHECK(table(rig)->editingCell());
  }

  void setCell(Rig& rig, std::size_t row, std::size_t column, std::string_view text) {
    typeInCell(rig, row, column, text);
    TEST_CHECK(rig.key(XKB_KEY_Return));
    TEST_CHECK(!table(rig)->editingCell());
  }

  void press(Rig& rig, TableEdit edit) { rig.click(table(rig)->editBar().button(edit)); }

  // Comma, tab and semicolon files: a cell, a row and a column changed, undone and redone, and saved; the bytes the
  // changes did not touch, the quoting, the line endings and the byte order mark are as they were.
  void testEditsAndSaves(::test::SceneCapture& capture) {
    const Files files;
    struct Case {
      std::string name;
      std::string bytes;
      std::string saved;   // a cell changed and a row added
      std::string resaved; // then that row deleted
    };
    const std::vector<Case> cases{
        {"comma.csv", "name,size\nalpha,12\n\"beta, b\",7\n", "name,size\nalpha,9\n,\n\"beta, b\",7\n",
         "name,size\nalpha,9\n\"beta, b\",7\n"},
        {"tabs.tsv", "name\tsize\r\nalpha\t12\r\nbeta\t7\r\n", "name\tsize\r\nalpha\t9\r\n\t\r\nbeta\t7\r\n",
         "name\tsize\r\nalpha\t9\r\nbeta\t7\r\n"},
        {"semi.csv", "name;size\nalpha;12\nbeta;7", "name;size\nalpha;9\n;\nbeta;7", "name;size\nalpha;9\nbeta;7"},
        {"mark.csv", "\xEF\xBB\xBFname,size\r\nalpha,12\r\n\"be\"\"ta\",7\r\n",
         "\xEF\xBB\xBFname,size\r\nalpha,9\r\n,\r\n\"be\"\"ta\",7\r\n",
         "\xEF\xBB\xBFname,size\r\nalpha,9\r\n\"be\"\"ta\",7\r\n"},
    };
    for (const auto& item : cases) {
      files.write(item.name, item.bytes);
    }
    Rig rig(files, capture, files.folder);
    for (const auto& item : cases) {
      editTable(rig, item.name);
      setCell(rig, 0, 1, "9");
      TEST_CHECK(rig.bar().unsaved && rig.bar().saveAvailable && rig.editor->unsaved());
      table(rig)->select({.row = 0, .column = 0});
      press(rig, TableEdit::InsertRowBelow);
      press(rig, TableEdit::InsertColumnRight);
      TEST_CHECK(table(rig)->table()->rowCount == 3 && table(rig)->table()->columnCount == 3);
      TEST_CHECK(rig.editor->textFocused()); // the bar gave the keyboard back to the grid
      TEST_CHECK(rig.key('z', KeyMod::Ctrl));
      TEST_CHECK(table(rig)->table()->columnCount == 2);
      TEST_CHECK(rig.key('y', KeyMod::Ctrl));
      table(rig)->select({.row = 0, .column = 1});
      press(rig, TableEdit::DeleteColumn);
      TEST_CHECK(files.read(item.name) == item.bytes); // only Save writes
      rig.saveKeys();
      TEST_CHECK(files.read(item.name) == item.saved);
      if (files.read(item.name) != item.saved) {
        std::println("{} saved as {:?}", item.name, files.read(item.name));
      }
      TEST_CHECK(!rig.bar().unsaved && !rig.editor->unsaved() && rig.bar().notice.empty());
      TEST_CHECK(table(rig) != nullptr && table(rig)->editable()); // saving leaves the table editable

      // A second save writes over the first: the revision our own save made is the one it expects.
      table(rig)->select({.row = 1, .column = 0});
      press(rig, TableEdit::DeleteRow);
      rig.saveKeys();
      TEST_CHECK(files.read(item.name) == item.resaved);
      rig.click(rig.slot().stopEditingButton());
      rig.frames([&] { return offeredTable(rig); });
      TEST_CHECK(!rig.editor->gate().editing());
    }
    TEST_CHECK(rig.asks == 0);
  }

  // Each reason a table cannot be edited, as the disabled button's tooltip.
  void testDisabledReasons(::test::SceneCapture& capture) {
    const Files files;
    luciole::test::table::writeZip(
        files.folder / "book.xlsx",
        luciole::test::table::xlsxMembers(
            {{"Sheet", "<row r=\"1\">" + luciole::test::table::textCell("A1", "read only") + "</row>"}}
        )
    );
    std::string tall = "n\n";
    for (std::size_t row = 0; row <= formats::kMaxGridRows; ++row) {
      tall += std::format("{}\n", row);
    }
    files.write("tall.csv", tall);
    files.write("open.csv", "name,note\nalpha,\"never closed\n");
    files.write("latin.csv", "name,size\ncaf\xE9,1\n");
    files.write("locked.csv", "name,size\nalpha,1\n", 0444);
    files.write("header.csv", "name,size\n");
    files.write("empty.csv", "");
    files.write("fine.csv", "name,size\nalpha,1\n");
    Rig rig(files, capture, files.folder);
    const auto reason = [&](std::string_view name, std::string_view words) {
      visitTable(rig, name);
      TEST_CHECK(!rig.bar().enableAvailable && !rig.slot().enableEditingButton().enabled());
      TEST_CHECK(rig.bar().reason.contains(words));
      if (!rig.bar().reason.contains(words)) {
        std::println("{}: {}", name, rig.bar().reason);
      }
      rig.click(rig.slot().enableEditingButton());
      TEST_CHECK(!rig.editor->gate().editing() && !table(rig)->editable());
    };
    reason("book.xlsx", "spreadsheets are read-only here");
    rig.idle();
    keepForViewing(rig.pixels, "preview-edit-table-read-only.png");
    reason("tall.csv", "more rows or columns than the preview shows");
    reason("open.csv", "never closed");
    reason("latin.csv", "not UTF-8");
    reason("locked.csv", "read-only");
    reason("header.csv", "no rows to edit");
    reason("empty.csv", "no rows to edit");
    visitTable(rig, "fine.csv");
    TEST_CHECK(rig.bar().enableAvailable && rig.bar().reason.empty());
    rig.idle();
    keepForViewing(rig.pixels, "preview-edit-table-offered.png");
  }

  // A change the file cannot take shows why in the bar and changes nothing. The table is larger than the grids built
  // at once, so the grids of the changes it takes arrive from a worker.
  void testRefusedChange(::test::SceneCapture& capture) {
    const Files files;
    std::string full = "name,text\n";
    for (std::size_t row = 0; full.size() + 120 < luciole::editing::kTableByteLimit; ++row) {
      full += std::format("{},{}\n", row % 10, std::string(110, 'x'));
    }
    files.write("full.csv", full);
    Rig rig(files, capture, files.folder);
    editTable(rig, "full.csv");
    press(rig, TableEdit::InsertColumnRight);
    TEST_CHECK(rig.bar().notice.contains("larger than 1 MiB"));
    TEST_CHECK(!rig.bar().unsaved && !rig.editor->unsaved() && table(rig)->table()->columnCount == 2);
    rig.idle();
    keepForViewing(rig.pixels, "preview-edit-table-refused.png");

    setCell(rig, 0, 1, "short");
    rig.frames([&] { return formats::gridCellText(*table(rig)->table(), 0, 1) == "short"; });
    rig.saveKeys();
    const auto rest = full.substr(full.find('\n', full.find('\n') + 1) + 1);
    TEST_CHECK(files.read("full.csv") == "name,text\n0,short\n" + rest);
  }

  // Leaving unsaved changes asks once: Save writes and moves on, No drops them. Text typed into a cell and not
  // committed yet counts as a change.
  void testLeaves(::test::SceneCapture& capture) {
    const Files files;
    files.write("a.csv", "name,size\nalpha,1\n");
    files.write("b.csv", "name,size\nbeta,2\n");
    files.write("c.csv", "name,size\ngamma,3\n");
    Rig rig(files, capture, files.folder);

    editTable(rig, "a.csv");
    setCell(rig, 0, 1, "10");
    rig.select("b.csv");
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(rig.asks == 1 && rig.question->name == "a.csv");
    TEST_CHECK(table(rig) != nullptr && table(rig)->editable()); // the editor stays until the answer
    rig.answerSave();
    TEST_CHECK(files.read("a.csv") == "name,size\nalpha,10\n");
    rig.frames([&] { return offeredTable(rig) && rig.slot().nameLabel().text() == "b.csv"; });

    editTable(rig, "b.csv");
    setCell(rig, 0, 1, "20");
    rig.select("c.csv");
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(rig.asks == 2 && rig.question->name == "b.csv");
    rig.answerNo();
    rig.frames([&] { return offeredTable(rig) && rig.slot().nameLabel().text() == "c.csv"; });
    TEST_CHECK(files.read("b.csv") == "name,size\nbeta,2\n" && !rig.editor->gate().editing());

    editTable(rig, "c.csv");
    typeInCell(rig, 0, 1, "30");
    rig.idle();
    keepForViewing(rig.pixels, "preview-edit-table-field.png");
    rig.select("a.csv");
    rig.frames([&] { return rig.question.has_value(); });
    TEST_CHECK(rig.asks == 3 && rig.question->name == "c.csv");
    rig.answerSave();
    TEST_CHECK(files.read("c.csv") == "name,size\ngamma,30\n");
    rig.frames([&] { return offeredTable(rig) && rig.slot().nameLabel().text() == "a.csv"; });

    // Escape on the grid stops editing and asks the same question.
    editTable(rig, "a.csv");
    setCell(rig, 0, 0, "omega");
    TEST_CHECK(rig.key(XKB_KEY_Escape));
    rig.frames([&] { return rig.question.has_value(); });
    rig.answerNo();
    rig.frames([&] { return offeredTable(rig); });
    TEST_CHECK(files.read("a.csv") == "name,size\nalpha,10\n" && rig.asks == 4);
  }

  // The grid and the cell field report the keyboard through the seam, so the page's shortcuts and type-ahead stand
  // down, and the table gets keys first.
  void testKeyboard(::test::SceneCapture& capture) {
    const Files files;
    files.write("keys.csv", "name,size\nalpha,1\nbeta,2\n");
    Rig rig(files, capture, files.folder);
    editTable(rig, "keys.csv");
    auto* view = table(rig);
    TEST_CHECK(rig.dispatcher.focusedArea() == &view->canvas());
    TEST_CHECK(!rig.key(XKB_KEY_F5)); // a key the table leaves goes on to the page
    TEST_CHECK(rig.key('q', 0, 'q')); // a typed letter opens the cell field, never type-ahead
    TEST_CHECK(view->editingCell() && rig.editor->textFocused());
    TEST_CHECK(rig.dispatcher.focusedArea() == &view->canvas().editor().fieldArea());
    TEST_CHECK(rig.key(XKB_KEY_Tab)); // Tab commits and moves to the next cell
    TEST_CHECK(!view->editingCell() && view->selectedSource()->column == 1);
    TEST_CHECK(rig.dispatcher.focusedArea() == &view->canvas() && rig.editor->unsaved());
    rig.idle();
    keepForViewing(rig.pixels, "preview-edit-table-editing.png");

    // Out of sight the field commits; the keyboard comes back to the grid once it shows again.
    rig.type("7");
    TEST_CHECK(view->editingCell());
    rig.dispatcher.setFocus(nullptr); // the page drops its keyboard focus as it goes out of sight
    rig.preview->hide();
    rig.pump();
    TEST_CHECK(!view->editingCell() && rig.asks == 0);
    rig.frames([&] { return table(rig) == view && rig.editor->textFocused(); });
    TEST_CHECK(rig.dispatcher.focusedArea() == &view->canvas());
    rig.saveKeys();
    TEST_CHECK(files.read("keys.csv") == "name,size\nq,7\nbeta,2\n");

    // Another view mode takes the view: text still in the field is asked about and saved.
    rig.type("8");
    TEST_CHECK(view->editingCell());
    rig.unmount();
    rig.frame();
    TEST_CHECK(rig.question.has_value() && rig.question->name == "keys.csv");
    rig.answerSave();
    TEST_CHECK(files.read("keys.csv") == "name,size\nq,8\nbeta,2\n" && !rig.editor->gate().editing());
  }
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (!args.empty() && args.front() == "--sheet-preview-worker") {
    return luciole::runSheetPreviewWorker(std::span(args).subspan(1));
  }
  if (::geteuid() == 0) {
    return 77; // root writes read-only files, so that refusal cannot be shown
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  testEditsAndSaves(*capture);
  testDisabledReasons(*capture);
  testRefusedChange(*capture);
  testLeaves(*capture);
  testKeyboard(*capture);
  std::println("preview-edit-table-column: all checks passed");
  return 0;
}
