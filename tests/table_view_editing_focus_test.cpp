#include "core/input/key_modifiers.h"
#include "formats/table_grid.h"
#include "table_view_owner.h"
#include "table_view_rig.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/table_edit_bar.h"
#include "ui/controls/table_view.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <print>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// Where the keyboard goes around the table's editing: after the edit bar, on save, when the owner commits, and
// while a message hides the grid.
namespace {
  using namespace luciole;
  using namespace luciole::test::tableview;

  bool offer(Rig& rig, std::uint32_t sym, std::uint32_t utf32 = 0, std::uint32_t modifiers = 0) {
    return rig.view->offerKey({.sym = sym, .utf32 = utf32, .modifiers = modifiers, .pressed = true});
  }

  void barKeepsGridKeys() {
    Rig rig;
    Sheet sheet(6, 4);
    sheet.attach(rig);
    std::vector<std::string> heard;
    const auto apply = rig.view->onEditRequest;
    rig.view->onEditRequest = [&](const TableEditRequest& request) {
      heard.push_back(std::format("request {}", static_cast<int>(request.edit)));
      apply(request);
    };
    const auto edit = rig.view->onCellEdited;
    rig.view->onCellEdited = [&](TableCell source, std::string text) {
      heard.push_back(std::format("edit {} {} {}", source.row, source.column, text));
      edit(source, std::move(text));
    };
    TableEditBar& bar = rig.view->editBar();
    focusCell(rig, "r1c1");
    const std::array<TableEdit, kTableEditCount> edits{
        TableEdit::InsertRowBelow, TableEdit::DeleteRow, TableEdit::InsertColumnRight,
        TableEdit::DeleteColumn,   TableEdit::Undo,      TableEdit::Redo,
    };
    // After each press the grid has the keyboard again, so Ctrl+Z is the table's undo and not the page's.
    for (const TableEdit pressed : edits) {
      rig.view->setEditAvailability({.undo = true, .redo = true});
      rig.click(center(bar.button(pressed)));
      TEST_CHECK(!heard.empty() && heard.back() == std::format("request {}", static_cast<int>(pressed)));
      TEST_CHECK(canvasFocused(rig));
      TEST_CHECK(offer(rig, XKB_KEY_z, 0, KeyMod::Ctrl));
      TEST_CHECK(heard.back() == std::format("request {}", static_cast<int>(TableEdit::Undo)));
    }
    TEST_CHECK(heard.size() == 2 * kTableEditCount);
    // A press that commits an open field gives the keyboard back to the grid too.
    rig.key(XKB_KEY_k, 'k');
    TEST_CHECK(rig.view->editingCell());
    rig.click(center(bar.button(TableEdit::InsertRowBelow)));
    TEST_CHECK(heard.size() == (2 * kTableEditCount) + 2);
    TEST_CHECK(heard[2 * kTableEditCount] == "edit 2 1 k");
    TEST_CHECK(!rig.view->editingCell() && canvasFocused(rig));
    TEST_CHECK(offer(rig, XKB_KEY_z, 0, KeyMod::Ctrl));
  }

  void saveCommitsField() {
    Rig rig;
    Sheet sheet(6, 4);
    sheet.attach(rig);
    std::vector<std::string> heard;
    const auto edit = rig.view->onCellEdited;
    rig.view->onCellEdited = [&](TableCell source, std::string text) {
      heard.push_back(std::format("edit {} {} {}", source.row, source.column, text));
      edit(source, std::move(text));
    };
    rig.view->onSaveRequested = [&] { heard.emplace_back("save"); };
    focusCell(rig, "r1c1");
    rig.key(XKB_KEY_x, 'x');
    rig.key(XKB_KEY_y, 'y');
    // Ctrl+S commits the field where it is, then asks to save, so the save holds the typed text.
    rig.key(XKB_KEY_s, 0, KeyMod::Ctrl);
    TEST_CHECK((heard == std::vector<std::string>{"edit 2 1 xy", "save"}));
    TEST_CHECK(sheet.rows[2][1] == "xy");
    TEST_CHECK(!rig.view->editingCell() && canvasFocused(rig));
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 1, .column = 1}));
    // An unchanged field saves without an edit.
    rig.key(XKB_KEY_F2);
    TEST_CHECK(rig.view->editingCell());
    rig.key(XKB_KEY_S, 0, KeyMod::Ctrl | KeyMod::Shift);
    TEST_CHECK(rig.view->editingCell() && heard.size() == 2);
    rig.key(XKB_KEY_S, 0, KeyMod::Ctrl);
    TEST_CHECK((heard == std::vector<std::string>{"edit 2 1 xy", "save", "save"}));
    TEST_CHECK(!rig.view->editingCell());
  }

  void ownerCommitsField() {
    Rig rig;
    Sheet sheet(6, 4);
    sheet.attach(rig);
    Heard heard;
    heard.listen(*rig.view);
    // Nothing is open: nothing to report.
    rig.view->commitField();
    TEST_CHECK(heard.quiet());
    focusCell(rig, "r2c3");
    rig.key(XKB_KEY_q, 'q');
    TEST_CHECK(rig.view->editingCell() && fieldFocused(rig));
    // A close that leaves the focus where it is, such as the window closing, commits through the owner.
    rig.view->commitField();
    TEST_CHECK(heard.edits.size() == 1 && heard.edits[0].first == (TableCell{.row = 3, .column = 3}));
    TEST_CHECK(heard.edits[0].second == "q");
    TEST_CHECK(!rig.view->editingCell() && canvasFocused(rig));
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 2, .column = 3}));
    // An unchanged field closes without a report.
    rig.key(XKB_KEY_F2);
    rig.view->commitField();
    TEST_CHECK(heard.edits.size() == 1 && !rig.view->editingCell());
  }

  void fieldNeedsOwnerText() {
    // The grid cuts this cell at 256 bytes and turns the line break of the other into a space.
    const std::string whole(300, 'w');
    const auto text = [&](std::size_t row, std::size_t column) {
      return row == 1 && column == 1 ? whole : row == 2 && column == 1 ? std::string("two\nlines") : std::string("s");
    };
    Rig rig;
    rig.view->setEditable(true);
    rig.show(grid(4, 3, text));
    Heard heard;
    heard.listen(*rig.view);
    for (const std::size_t row : {std::size_t{1}, std::size_t{2}}) {
      rig.view->select({.row = row, .column = 1});
      rig.settle();
      rig.dispatcher.setFocus(&rig.view->canvas());
      // Without the owner's text no field opens, and the keys that would open one are left for the page.
      TEST_CHECK(!offer(rig, XKB_KEY_F2) && !offer(rig, XKB_KEY_Return) && !offer(rig, XKB_KEY_x, 'x'));
      rig.key(XKB_KEY_F2);
      rig.key(XKB_KEY_x, 'x');
      TEST_CHECK(!rig.view->editingCell() && heard.quiet());
    }
    rig.doubleClick(center(*labelWith(rig.root, std::string(formats::kMaxGridCellBytes, 'w'))));
    TEST_CHECK(!rig.view->editingCell() && heard.quiet());
    // Clearing needs no text.
    rig.dispatcher.setFocus(&rig.view->canvas());
    rig.key(XKB_KEY_Delete);
    TEST_CHECK(heard.edits.size() == 1 && heard.edits[0].first == (TableCell{.row = 1, .column = 1}));
    TEST_CHECK(heard.edits[0].second.empty());
    // With it the field holds the whole cell, and a commit keeps all of it.
    rig.view->cellText = [&](TableCell source) { return text(source.row, source.column); };
    rig.key(XKB_KEY_F2);
    rig.key(XKB_KEY_End);
    rig.key(XKB_KEY_x, 'x');
    rig.key(XKB_KEY_Return);
    TEST_CHECK(heard.edits.size() == 2 && heard.edits[1].second == whole + "x");
  }

  void tooltipFollowsScroll() {
    const std::string whole(300, 'w');
    const std::string cut(formats::kMaxGridCellBytes, 'w');
    Rig rig;
    rig.show(grid(300, 3, [&](std::size_t row, std::size_t column) {
      return row == 1 && column == 1 ? whole : std::format("r{}c{}", row, column);
    }));
    InputArea& canvas = rig.view->canvas();
    const Point at = center(*labelWith(rig.root, cut));
    rig.hover(at);
    TEST_CHECK(tooltip(canvas) == cut);
    // The wheel moves another cell under a pointer that stays still.
    rig.dispatcher.pointerAxis(at.x, at.y, 0, 0, 220, 0, 0, 0);
    rig.settle();
    TEST_CHECK(rig.view->scrollY() > 200);
    TEST_CHECK(tooltip(canvas).empty());
    rig.dispatcher.pointerAxis(at.x, at.y, 0, 0, -220, 0, 0, 0);
    rig.settle();
    TEST_NEAR(rig.view->scrollY(), 0, 0.01F);
    TEST_CHECK(tooltip(canvas) == cut);
  }

  void messageTakesNoKeys() {
    Rig rig;
    Sheet sheet(6, 4);
    sheet.attach(rig);
    Heard heard;
    heard.listen(*rig.view);
    rig.view->setEditAvailability({.undo = true, .redo = true});
    focusCell(rig, "r1c1");
    rig.view->setMessage("This sheet could not be read.");
    rig.settle();
    TEST_CHECK(canvasFocused(rig));
    // The grid is hidden, so nothing opens on it and its keys are the page's.
    TEST_CHECK(!offer(rig, XKB_KEY_a, 'a') && !offer(rig, XKB_KEY_F2) && !offer(rig, XKB_KEY_Delete));
    TEST_CHECK(!offer(rig, XKB_KEY_z, 0, KeyMod::Ctrl) && !offer(rig, XKB_KEY_s, 0, KeyMod::Ctrl));
    rig.key(XKB_KEY_a, 'a');
    rig.key(XKB_KEY_Down);
    TEST_CHECK(!rig.view->editingCell() && heard.quiet());
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 1, .column = 1}));
    // The grid back, its keys are the table's again.
    rig.view->setMessage({});
    rig.settle();
    rig.dispatcher.setFocus(&rig.view->canvas());
    TEST_CHECK(offer(rig, XKB_KEY_F2) && rig.view->editingCell());
  }
} // namespace

int main() {
  barKeepsGridKeys();
  saveCommitsField();
  ownerCommitsField();
  fieldNeedsOwnerText();
  tooltipFollowsScroll();
  messageTakesNoKeys();
  std::println("table-view-editing-focus: all checks passed");
  return 0;
}
