#include "core/input/key_modifiers.h"
#include "formats/table_grid.h"
#include "table_view_owner.h"
#include "table_view_rig.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/table_cell_editor.h"
#include "ui/controls/table_edit_bar.h"
#include "ui/controls/table_view.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  using namespace luciole;
  using namespace luciole::test::tableview;

  void staysQuietWhenNotEditable() {
    Rig rig;
    Heard heard;
    heard.listen(*rig.view);
    rig.show(Sheet(5, 3).grid());
    TEST_CHECK(!rig.view->editable());
    TEST_CHECK(!shown(rig.view->editBar()));
    rig.dispatcher.setFocus(&rig.view->canvas());
    struct Key {
      std::uint32_t sym;
      std::uint32_t utf32;
      std::uint32_t modifiers;
    };
    const std::array<Key, 9> keys{{
        {XKB_KEY_Return, 0, 0},
        {XKB_KEY_F2, 0, 0},
        {XKB_KEY_x, 'x', 0},
        {XKB_KEY_Delete, 0, 0},
        {XKB_KEY_BackSpace, 0, 0},
        {XKB_KEY_z, 0, KeyMod::Ctrl},
        {XKB_KEY_Z, 0, KeyMod::Ctrl | KeyMod::Shift},
        {XKB_KEY_y, 0, KeyMod::Ctrl},
        {XKB_KEY_s, 0, KeyMod::Ctrl},
    }};
    for (const auto& key : keys) {
      TEST_CHECK(
          !rig.view->offerKey({.sym = key.sym, .utf32 = key.utf32, .modifiers = key.modifiers, .pressed = true})
      );
      rig.dispatcher.setFocus(&rig.view->canvas());
      rig.key(key.sym, key.utf32, key.modifiers);
    }
    rig.doubleClick(cellPoint(rig, "r1c1"));
    TEST_CHECK(!rig.view->editingCell() && !editor(rig).visible());
    TEST_CHECK(heard.quiet());
    // The grid does not keep focus after a click, as before editing existed.
    TEST_CHECK(!canvasFocused(rig));
  }

  void opensField() {
    Rig rig;
    Sheet sheet(6, 4);
    sheet.attach(rig);
    // The owner's text is whole; this one differs from the grid's to show where the field's text comes from.
    rig.view->cellText = [](TableCell source) { return std::format("whole {} {}", source.row, source.column); };
    Heard heard;
    heard.listen(*rig.view);
    TEST_CHECK(rig.view->editable() && shown(rig.view->editBar()));
    const auto* cell = labelWith(rig.view->canvas(), "r2c1");
    const Point at = center(*cell);
    float cellX = 0;
    float cellY = 0;
    Node::absolutePosition(cell, cellX, cellY);
    const auto opened = [&](std::string_view text) {
      TEST_CHECK(rig.view->editingCell());
      TEST_CHECK(editor(rig).cell() == (TableCell{.row = 3, .column = 1}));
      TEST_CHECK(fieldText(rig) == text);
      TEST_CHECK(fieldFocused(rig));
      // The field covers the cell: its column's width and its row's height, where its text sits.
      const Input& field = editor(rig).field();
      float x = 0;
      float y = 0;
      Node::absolutePosition(&field, x, y);
      TEST_NEAR(x, cellX - kPad, 0.5F);
      TEST_NEAR(field.width(), cell->width() + (2 * kPad), 0.5F);
      TEST_NEAR(field.height(), kRow, 0.01F);
      TEST_NEAR(center(field).y, at.y, 0.5F);
      // Escape leaves without a commit and gives the keyboard back to the grid.
      rig.key(XKB_KEY_Escape);
      TEST_CHECK(!rig.view->editingCell() && !editor(rig).visible());
      TEST_CHECK(canvasFocused(rig));
      TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 2, .column = 1}));
    };
    focusCell(rig, "r2c1");
    rig.key(XKB_KEY_Return);
    opened("whole 3 1");
    rig.key(XKB_KEY_F2);
    opened("whole 3 1");
    rig.doubleClick(at);
    opened("whole 3 1");
    rig.key(XKB_KEY_q, 'q');
    // The field's text starts where the cell's did.
    const auto typed = labels(editor(rig).field());
    TEST_CHECK(typed.size() == 1 && typed.front()->text() == "q");
    float typedX = 0;
    float typedY = 0;
    Node::absolutePosition(typed.front(), typedX, typedY);
    TEST_NEAR(typedX, cellX, 0.5F);
    opened("q");
    // Shift types a capital, as text does.
    rig.key(XKB_KEY_Q, 'Q', KeyMod::Shift);
    opened("Q");
    TEST_CHECK(heard.quiet());
    // Two clicks a second apart are two clicks.
    rig.click(at);
    rig.click(at);
    TEST_CHECK(!rig.view->editingCell());
    // The field's text is the table's size.
    rig.view->setTextSize(16);
    rig.settle();
    rig.key(XKB_KEY_F2);
    // The text is wider than the cell, so the field shows the end of it, where the caret is.
    const auto shownText = labels(editor(rig).field());
    TEST_CHECK(shownText.size() == 1 && shownText.front()->fontSize() == 16);
    TEST_CHECK(std::string_view("whole 3 1").ends_with(shownText.front()->text()));
    TEST_NEAR(editor(rig).field().height(), 29, 0.01F);
  }

  void leavesField() {
    Rig rig;
    Sheet sheet(6, 4);
    sheet.attach(rig);
    std::vector<std::pair<TableCell, std::string>> edits;
    const auto owner = rig.view->onCellEdited;
    rig.view->onCellEdited = [&](TableCell source, std::string text) {
      edits.emplace_back(source, text);
      owner(source, std::move(text));
    };
    focusCell(rig, "r2c1");
    // Enter commits and moves down.
    rig.key(XKB_KEY_F2);
    rig.key(XKB_KEY_BackSpace);
    rig.key(XKB_KEY_x, 'x');
    TEST_CHECK(fieldText(rig) == "r2cx");
    rig.key(XKB_KEY_Return);
    TEST_CHECK(edits.size() == 1 && edits.back().first == (TableCell{.row = 3, .column = 1}));
    TEST_CHECK(edits.back().second == "r2cx");
    TEST_CHECK(sheet.rows[3][1] == "r2cx" && labelWith(rig.root, "r2cx") != nullptr);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 3, .column = 1}));
    TEST_CHECK(!rig.view->editingCell() && canvasFocused(rig));
    // Tab commits and moves right, Shift+Tab left.
    rig.key(XKB_KEY_a, 'a');
    rig.key(XKB_KEY_Tab);
    TEST_CHECK(edits.back().first == (TableCell{.row = 4, .column = 1}) && edits.back().second == "a");
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 3, .column = 2}));
    rig.key(XKB_KEY_b, 'b');
    rig.key(XKB_KEY_ISO_Left_Tab, 0, KeyMod::Shift);
    TEST_CHECK(edits.back().first == (TableCell{.row = 4, .column = 2}) && edits.back().second == "b");
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 3, .column = 1}));
    TEST_CHECK(edits.size() == 3);
    // A commit that changes nothing reports nothing, and still moves.
    rig.key(XKB_KEY_F2);
    rig.key(XKB_KEY_Return);
    TEST_CHECK(edits.size() == 3);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 4, .column = 1}));
    // Moves stop at the edges of the table.
    rig.press(XKB_KEY_End, KeyMod::Ctrl);
    rig.key(XKB_KEY_F2);
    rig.key(XKB_KEY_Tab);
    rig.key(XKB_KEY_F2);
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 5, .column = 3}));
    rig.press(XKB_KEY_Home, KeyMod::Ctrl);
    rig.key(XKB_KEY_F2);
    rig.key(XKB_KEY_ISO_Left_Tab, 0, KeyMod::Shift);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 0, .column = 0}) && edits.size() == 3);
    // Losing focus commits: a click on another cell reports the edit of the first, then selects the second.
    rig.key(XKB_KEY_z, 'z');
    rig.click(cellPoint(rig, "r1c2"));
    TEST_CHECK(edits.size() == 4 && edits.back().first == (TableCell{.row = 1, .column = 0}));
    TEST_CHECK(edits.back().second == "z" && sheet.rows[1][0] == "z");
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 1, .column = 2}) && !rig.view->editingCell());
    // A click outside the table takes the focus too, and commits.
    rig.key(XKB_KEY_w, 'w');
    rig.dispatcher.setFocus(nullptr);
    rig.settle();
    TEST_CHECK(edits.size() == 5 && edits.back().second == "w" && !rig.view->editingCell());
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 1, .column = 2}));
  }

  void clearsCells() {
    Rig rig;
    Sheet sheet(4, 3);
    sheet.attach(rig);
    Heard heard;
    heard.listen(*rig.view);
    focusCell(rig, "r1c1");
    rig.key(XKB_KEY_Delete);
    TEST_CHECK(heard.edits.size() == 1 && heard.edits[0].first == (TableCell{.row = 2, .column = 1}));
    TEST_CHECK(heard.edits[0].second.empty());
    rig.press(XKB_KEY_Right);
    rig.key(XKB_KEY_BackSpace);
    TEST_CHECK(heard.edits.size() == 2 && heard.edits[1].first == (TableCell{.row = 2, .column = 2}));
    TEST_CHECK(heard.edits[1].second.empty());
    // A cell that is already empty changes nothing.
    sheet.rows[1][0].clear();
    rig.press(XKB_KEY_Home, KeyMod::Ctrl);
    rig.key(XKB_KEY_Delete);
    TEST_CHECK(heard.edits.size() == 2);
    TEST_CHECK(!rig.view->editingCell());
  }

  void followsScroll() {
    Rig rig;
    Sheet sheet(300, 4);
    sheet.attach(rig);
    Heard heard;
    heard.listen(*rig.view);
    focusCell(rig, "r2c1");
    rig.key(XKB_KEY_F2);
    rig.key(XKB_KEY_x, 'x');
    const float before = center(editor(rig).field()).y;
    rig.wheel(0, 30);
    TEST_NEAR(rig.view->scrollY(), 30, 0.01F);
    TEST_CHECK(rig.view->editingCell());
    TEST_NEAR(center(editor(rig).field()).y, before - 30, 0.5F);
    // Scrolled out of view, the field commits and the grid keeps the keyboard.
    rig.wheel(0, 400);
    TEST_CHECK(!rig.view->editingCell());
    TEST_CHECK(heard.edits.size() == 1 && heard.edits[0].first == (TableCell{.row = 3, .column = 1}));
    TEST_CHECK(heard.edits[0].second == "r2c1x");
    TEST_CHECK(canvasFocused(rig));
    // A field opened on a cell out of view brings the cell into view.
    rig.key(XKB_KEY_F2);
    TEST_CHECK(rig.view->editingCell());
    TEST_NEAR(rig.view->scrollY(), 2 * kRow, 0.01F);
  }

  void takesKeys() {
    Rig rig;
    Sheet sheet(6, 4);
    sheet.attach(rig);
    Heard heard;
    heard.listen(*rig.view);
    focusCell(rig, "r1c2");
    const auto offer = [&](std::uint32_t sym, std::uint32_t utf32 = 0, std::uint32_t modifiers = 0) {
      return rig.view->offerKey({.sym = sym, .utf32 = utf32, .modifiers = modifiers, .pressed = true});
    };
    // Undo and redo report only while the owner has them.
    rig.key(XKB_KEY_z, 0, KeyMod::Ctrl);
    rig.key(XKB_KEY_y, 0, KeyMod::Ctrl);
    TEST_CHECK(heard.requests.empty());
    rig.view->setEditAvailability({.undo = true, .redo = true});
    rig.key(XKB_KEY_z, 0, KeyMod::Ctrl);
    rig.key(XKB_KEY_Z, 0, KeyMod::Ctrl | KeyMod::Shift);
    rig.key(XKB_KEY_y, 0, KeyMod::Ctrl);
    TEST_CHECK(heard.requests.size() == 3);
    TEST_CHECK(heard.requests[0].edit == TableEdit::Undo);
    TEST_CHECK(heard.requests[0].source == (TableCell{.row = 2, .column = 2}));
    TEST_CHECK(heard.requests[1].edit == TableEdit::Redo && heard.requests[2].edit == TableEdit::Redo);
    rig.key(XKB_KEY_s, 0, KeyMod::Ctrl);
    TEST_CHECK(heard.saves == 1);
    // Every other key is left for the page; the arrows then reach the grid through the focus.
    TEST_CHECK(!offer(XKB_KEY_Escape));
    TEST_CHECK(!offer(XKB_KEY_a, 'a', KeyMod::Ctrl));
    TEST_CHECK(!offer(XKB_KEY_F5));
    TEST_CHECK(!offer(XKB_KEY_Return, 0, KeyMod::Ctrl));
    TEST_CHECK(!offer(XKB_KEY_Down));
    rig.key(XKB_KEY_Down);
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 2, .column = 2}));
    // Without the focus the table takes nothing: typing belongs to whatever has it.
    rig.dispatcher.setFocus(nullptr);
    TEST_CHECK(!offer(XKB_KEY_a, 'a') && !offer(XKB_KEY_s, 0, KeyMod::Ctrl) && !offer(XKB_KEY_Return));
    // While the field is open it has every key but Ctrl+S, the page's shortcuts included.
    rig.dispatcher.setFocus(&rig.view->canvas());
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.view->editingCell());
    TEST_CHECK(offer(XKB_KEY_z, 0, KeyMod::Ctrl) && offer(XKB_KEY_F5));
    TEST_CHECK(offer(XKB_KEY_Down) && offer(XKB_KEY_a, 'a', KeyMod::Ctrl));
    TEST_CHECK(heard.saves == 1 && heard.requests.size() == 3);
    TEST_CHECK(rig.view->editingCell() && rig.view->selectedCell() == (TableCell{.row = 2, .column = 2}));
  }

  void requestsEdits() {
    Rig rig;
    Sheet sheet(6, 4);
    sheet.attach(rig);
    Heard heard;
    heard.listen(*rig.view);
    TableEditBar& bar = rig.view->editBar();
    // The bar lies above the grid, its icons no larger than the table's text, each named by a tooltip.
    TEST_CHECK(center(bar).y < center(rig.view->canvas()).y);
    const std::array<std::pair<TableEdit, std::string_view>, kTableEditCount> names{{
        {TableEdit::InsertRowBelow, "Insert row below"},
        {TableEdit::DeleteRow, "Delete row"},
        {TableEdit::InsertColumnRight, "Insert column to the right"},
        {TableEdit::DeleteColumn, "Delete column"},
        {TableEdit::Undo, "Undo"},
        {TableEdit::Redo, "Redo"},
    }};
    for (const auto& [edit, name] : names) {
      const Button& button = bar.button(edit);
      TEST_CHECK(tooltip(*button.inputArea()) == name);
      TEST_CHECK(button.glyph() != nullptr && button.glyph()->width() <= rig.view->textSize());
      TEST_CHECK(button.height() <= kRow);
    }
    focusCell(rig, "r2c3");
    const auto press = [&](TableEdit edit) { rig.click(center(bar.button(edit))); };
    // Undo and redo wait for the owner.
    TEST_CHECK(!bar.available(TableEdit::Undo) && !bar.available(TableEdit::Redo));
    press(TableEdit::Undo);
    press(TableEdit::Redo);
    TEST_CHECK(heard.requests.empty());
    for (const auto& [edit, name] : names) {
      rig.view->setEditAvailability({.undo = true, .redo = true});
      press(edit);
      TEST_CHECK(!heard.requests.empty() && heard.requests.back().edit == edit);
      TEST_CHECK(heard.requests.back().source == (TableCell{.row = 3, .column = 3}));
    }
    TEST_CHECK(heard.requests.size() == kTableEditCount);
    // With one row or column left the owner takes deleting it away.
    rig.view->setEditAvailability({.deleteRow = false, .deleteColumn = false});
    TEST_CHECK(!bar.available(TableEdit::DeleteRow) && !bar.available(TableEdit::DeleteColumn));
    TEST_CHECK(bar.available(TableEdit::InsertRowBelow) && bar.available(TableEdit::InsertColumnRight));
    press(TableEdit::DeleteRow);
    press(TableEdit::DeleteColumn);
    TEST_CHECK(heard.requests.size() == kTableEditCount);
    // A press on the bar commits an open field first.
    rig.view->setEditAvailability({});
    rig.click(cellPoint(rig, "r2c3"));
    rig.key(XKB_KEY_k, 'k');
    press(TableEdit::InsertRowBelow);
    TEST_CHECK(heard.edits.size() == 1 && heard.edits[0].second == "k");
    TEST_CHECK(heard.requests.back().edit == TableEdit::InsertRowBelow);
    // The icons follow the text size.
    rig.view->setTextSize(9);
    rig.settle();
    TEST_CHECK(bar.button(TableEdit::Undo).glyph()->width() <= 9);
  }

  void keepsSelectionAcrossNewGrid() {
    Rig rig;
    Sheet sheet(300, 30);
    sheet.attach(rig);
    std::vector<TableCell> moves;
    rig.view->onSelectionChanged = [&](TableCell source) { moves.push_back(source); };
    rig.view->select({.row = 150, .column = 20});
    rig.settle();
    const float scrollX = rig.view->scrollX();
    const float scrollY = rig.view->scrollY();
    TEST_CHECK(scrollX > 0 && scrollY > 0);
    moves.clear();
    sheet.apply({.edit = TableEdit::InsertRowBelow, .source = {.row = 151, .column = 20}});
    rig.settle();
    TEST_CHECK(rig.view->selectedSource() == (TableCell{.row = 151, .column = 20}));
    TEST_NEAR(rig.view->scrollX(), scrollX, 0.01F);
    TEST_NEAR(rig.view->scrollY(), scrollY, 0.01F);
    TEST_CHECK(moves.empty());
    TEST_CHECK(rig.view->table()->rowCount == 301);
    // A deleted column moves the selection to the nearest that is left.
    rig.view->select({.row = 300, .column = 29});
    rig.settle();
    sheet.apply({.edit = TableEdit::DeleteColumn, .source = {.row = 301, .column = 29}});
    rig.settle();
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 300, .column = 28}));
    sheet.apply({.edit = TableEdit::DeleteRow, .source = {.row = 301, .column = 28}});
    rig.settle();
    TEST_CHECK(rig.view->selectedCell() == (TableCell{.row = 299, .column = 28}));
    TEST_CHECK(
        (moves
         == std::vector<TableCell>{{.row = 301, .column = 29}, {.row = 301, .column = 28}, {.row = 300, .column = 28}})
    );
    // The last cell stays in view.
    TEST_CHECK(labelWith(rig.root, sheet.rows[300][28]) != nullptr);
    // A new grid while the field is open closes it without a commit.
    Heard heard;
    heard.listen(*rig.view);
    rig.dispatcher.setFocus(&rig.view->canvas());
    rig.key(XKB_KEY_m, 'm');
    TEST_CHECK(rig.view->editingCell());
    rig.view->replaceTable(sheet.grid());
    rig.settle();
    TEST_CHECK(!rig.view->editingCell() && heard.quiet() && canvasFocused(rig));
    // A grid with nothing in it selects nothing.
    rig.view->replaceTable(grid(0, 0, [](std::size_t, std::size_t) { return ""; }));
    rig.settle();
    TEST_CHECK(!rig.view->selectedCell());
    TEST_CHECK(!rig.view->editBar().available(TableEdit::InsertRowBelow));
  }

  void showsCutText() {
    const std::string whole(300, 'w');
    const std::string cut(formats::kMaxGridCellBytes, 'w');
    for (const bool editable : {false, true}) {
      Rig rig;
      rig.view->setEditable(editable);
      rig.show(grid(3, 3, [&](std::size_t row, std::size_t column) {
        return row == 1 && column == 1 ? whole : std::format("r{}c{}", row, column);
      }));
      InputArea& canvas = rig.view->canvas();
      rig.hover(cellPoint(rig, "r0c0"));
      TEST_CHECK(tooltip(canvas).empty());
      rig.hover(center(*labelWith(rig.root, cut)));
      TEST_CHECK(tooltip(canvas) == cut);
      // With the owner's whole text, the tooltip shows all of it.
      rig.view->cellText = [&](TableCell source) {
        return source == TableCell{.row = 1, .column = 1} ? whole : std::string("short");
      };
      TEST_CHECK(tooltip(canvas) == whole);
      rig.hover(cellPoint(rig, "r2c2"));
      TEST_CHECK(tooltip(canvas).empty());
      // Off the cells there is nothing to show.
      rig.hover(center(*labelWith(rig.root, "B")));
      TEST_CHECK(tooltip(canvas).empty());
    }
  }

  void stopsEditing() {
    Rig rig;
    Sheet sheet(4, 3);
    sheet.attach(rig);
    Heard heard;
    heard.listen(*rig.view);
    focusCell(rig, "r1c1");
    rig.key(XKB_KEY_x, 'x');
    TEST_CHECK(rig.view->editingCell());
    rig.view->setEditable(false);
    rig.settle();
    TEST_CHECK(!rig.view->editingCell() && !shown(rig.view->editBar()) && heard.quiet());
    rig.key(XKB_KEY_Return);
    rig.key(XKB_KEY_Delete);
    TEST_CHECK(!rig.view->editingCell() && heard.quiet());
    // A new table closes an open field without a commit too.
    rig.view->setEditable(true);
    rig.key(XKB_KEY_x, 'x');
    TEST_CHECK(rig.view->editingCell());
    rig.show(sheet.grid());
    TEST_CHECK(!rig.view->editingCell() && heard.quiet());
  }
} // namespace

int main() {
  staysQuietWhenNotEditable();
  opensField();
  leavesField();
  clearsCells();
  followsScroll();
  takesKeys();
  requestsEdits();
  keepsSelectionAcrossNewGrid();
  showsCutText();
  stopsEditing();
  std::println("table-view-editing: all checks passed");
  return 0;
}
