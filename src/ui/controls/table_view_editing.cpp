#include "core/input/key_chord.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "formats/utf8.h"
#include "ui/controls/input.h"
#include "ui/controls/table_view.h"

#include <algorithm>
#include <linux/input-event-codes.h>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

// The editing half of the table view: the cell field, the editing keys, the edit bar and a grid set after a change.
namespace luciole {
  namespace {
    // Two primary presses on one cell within this many milliseconds are a double click.
    constexpr std::uint32_t kDoubleClickMs = 400;
    constexpr std::uint32_t kModifiers = KeyMod::Shift | KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super;

    bool isLetter(std::uint32_t sym, std::uint32_t lower) { return sym == lower || sym == lower - ('a' - 'A'); }

    bool isSave(const InputArea::KeyData& data) {
      return data.pressed && isLetter(data.sym, XKB_KEY_s) && (data.modifiers & kModifiers) == KeyMod::Ctrl;
    }

    // The nearest index of `count` to a source index, where the grid starts at source index `first`.
    std::size_t nearest(std::size_t source, std::size_t first, std::size_t count) {
      return std::min(source - std::min(source, first), count - 1);
    }
  } // namespace

  void TableView::setEditAvailability(TableEditAvailability availability) {
    m_availability = availability;
    syncEditBar();
  }

  void TableView::replaceTable(std::shared_ptr<const formats::TableGrid> table) {
    m_canvas->editor().close(TableFieldExit::Cancel);
    const auto before = selectedSource();
    // The grid keeps showing the last frame until the next update, so a press in between lands on the cell the user
    // sees, and select() clamps it to the new table.
    m_table = std::move(table);
    m_tableChanged = true;
    m_planned = false;
    m_dirty = true;
    m_selected.reset();
    needFrame();
    if (m_table != nullptr && m_table->rowCount > 0 && m_table->columnCount > 0) {
      const TableCell source =
          before.value_or(TableCell{.row = m_table->firstSourceRow, .column = m_table->firstSourceColumn});
      m_selected = TableCell{
          .row = nearest(source.row, m_table->firstSourceRow, m_table->rowCount),
          .column = nearest(source.column, m_table->firstSourceColumn, m_table->columnCount),
      };
      m_reveal = true;
    }
    syncEditBar();
    if (selectedSource() != before && m_selected && onSelectionChanged) {
      onSelectionChanged(*selectedSource());
    }
  }

  bool TableView::offerKey(const InputArea::KeyData& key) {
    auto& editor = m_canvas->editor();
    if (editor.cell()) {
      // Ctrl+S saves the field's text with the file; while text is being composed it belongs to the composition.
      if (isSave(key) && !editor.field().composing()) {
        editor.close(TableFieldExit::Stay);
        if (onSaveRequested) {
          onSaveRequested();
        }
        return true;
      }
      editor.fieldArea().dispatchKey(key.sym, key.utf32, key.modifiers, key.pressed, key.preedit);
      return true;
    }
    return m_canvas->focused() && editKey(key);
  }

  void TableView::commitField() { m_canvas->editor().close(TableFieldExit::Stay); }

  // A press selects the cell under it; in editing mode a second primary press on it opens its field.
  void TableView::pressCell(const InputArea::PointerData& pointer) {
    const auto cell = m_canvas->cellAt(pointer.localX, pointer.localY);
    if (!cell) {
      return;
    }
    select(*cell);
    if (!m_editable || !pointer.pressed || pointer.button != BTN_LEFT) {
      return;
    }
    const bool twice = m_lastPress == cell && pointer.time - m_lastPressTime < kDoubleClickMs;
    m_lastPress = twice ? std::nullopt : cell;
    m_lastPressTime = pointer.time;
    if (twice) {
      openField(std::nullopt);
    }
  }

  // The keys of a selected cell in editing mode while its field is closed and the grid shows; false for any other
  // key. The keys that open the field are left for the page while the owner gives no text to open it with.
  bool TableView::editKey(const InputArea::KeyData& data) {
    if (!m_editable || !m_selected || editingCell() || !m_message.empty() || !data.pressed) {
      return false;
    }
    const std::uint32_t held = data.modifiers & kModifiers;
    const std::uint32_t sym = data.sym;
    const bool opens = static_cast<bool>(cellText);
    if (opens && (KeySymbol::isEnter(sym) || sym == XKB_KEY_F2) && held == 0) {
      openField(std::nullopt);
    } else if (KeySymbol::isBackspaceOrDelete(sym) && held == 0) {
      const auto source = selectedSource();
      if (!wholeText(*source).empty() && onCellEdited) {
        onCellEdited(*source, {});
      }
    } else if (opens && isPlainPrintableKey(data.utf32, data.modifiers, data.preedit)) {
      std::string typed;
      formats::appendUtf8(typed, data.utf32);
      openField(typed);
    } else if (isLetter(sym, XKB_KEY_z) && held == KeyMod::Ctrl) {
      request(TableEdit::Undo);
    } else if (
        (isLetter(sym, XKB_KEY_z) && held == (KeyMod::Ctrl | KeyMod::Shift))
        || (isLetter(sym, XKB_KEY_y) && held == KeyMod::Ctrl)
    ) {
      request(TableEdit::Redo);
    } else if (isSave(data)) {
      if (onSaveRequested) {
        onSaveRequested();
      }
    } else {
      return false;
    }
    return true;
  }

  // Opens the field on the selected cell with the owner's whole text, or with a typed character in place of it.
  void TableView::openField(const std::optional<std::string>& typed) {
    const auto source = selectedSource();
    if (!m_editable || !source || !cellText) {
      return;
    }
    const std::string whole = cellText(*source);
    auto& editor = m_canvas->editor();
    editor.open(*source, typed.value_or(whole), whole);
    m_reveal = true;
    m_dirty = true;
    needFrame();
    if (onFocus) {
      onFocus(&editor.fieldArea());
    }
  }

  void TableView::fieldClosed(TableFieldExit exit, TableCell source, std::optional<std::string> text) {
    const bool focused = m_canvas->editor().fieldArea().focused();
    if (text && onCellEdited) {
      onCellEdited(source, std::move(*text));
    }
    if (m_selected) {
      TableCell cell = *m_selected;
      if (exit == TableFieldExit::Down) {
        ++cell.row;
      } else if (exit == TableFieldExit::Right) {
        ++cell.column;
      } else if (exit == TableFieldExit::Left) {
        cell.column -= cell.column > 0 ? 1 : 0;
      }
      select(cell);
    }
    m_dirty = true;
    needFrame();
    if (focused && onFocus) {
      onFocus(m_canvas);
    }
  }

  // A field whose cell the grid scrolled out of the viewport commits.
  void TableView::closeOffscreenField() {
    const bool sized = m_canvas->bodyWidth() > 0 && m_canvas->bodyHeight() > 0;
    if (editingCell() && sized && m_selected && !m_canvas->cellInView(*m_selected)) {
      m_canvas->editor().close(TableFieldExit::Stay);
    }
  }

  void TableView::request(TableEdit edit) {
    m_canvas->editor().close(TableFieldExit::Stay);
    if (!m_editable || !m_editBar->available(edit) || !onEditRequest) {
      return;
    }
    onEditRequest({.edit = edit, .source = selectedSource().value_or(TableCell{})});
  }

  // The press took the keyboard from the grid; it goes back, so the next Ctrl+Z is still the table's.
  void TableView::pressBar(TableEdit edit) {
    request(edit);
    if (m_editable && onFocus) {
      onFocus(m_canvas);
    }
  }

  void TableView::syncEditBar() {
    const bool cell = m_selected.has_value();
    m_editBar->setAvailable(TableEdit::InsertRowBelow, cell);
    m_editBar->setAvailable(TableEdit::DeleteRow, cell && m_availability.deleteRow);
    m_editBar->setAvailable(TableEdit::InsertColumnRight, cell);
    m_editBar->setAvailable(TableEdit::DeleteColumn, cell && m_availability.deleteColumn);
    m_editBar->setAvailable(TableEdit::Undo, m_availability.undo);
    m_editBar->setAvailable(TableEdit::Redo, m_availability.redo);
  }

  std::string TableView::wholeText(TableCell source) const {
    if (cellText) {
      return cellText(source);
    }
    if (m_table == nullptr || source.row < m_table->firstSourceRow || source.column < m_table->firstSourceColumn) {
      return {};
    }
    return std::string(
        formats::gridCellText(
            *m_table, source.row - m_table->firstSourceRow, source.column - m_table->firstSourceColumn
        )
    );
  }
} // namespace luciole
