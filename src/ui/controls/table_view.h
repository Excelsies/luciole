#pragma once

#include "formats/table_grid.h"
#include "ui/controls/flex.h"
#include "ui/controls/table_cell_editor.h"
#include "ui/controls/table_edit_bar.h"
#include "ui/controls/table_view_canvas.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Label;
class ScrollView;
class Segmented;

namespace luciole {
  // A change the owner applies, with the source row and column of the selected cell when it was asked for.
  struct TableEditRequest {
    TableEdit edit = TableEdit::Undo;
    TableCell source;
  };

  // The edits the owner can apply now.
  struct TableEditAvailability {
    bool undo = false;
    bool redo = false;
    bool deleteRow = true;    // false while one row is left
    bool deleteColumn = true; // false while one column is left
  };

  // A table preview: a sheet list when the workbook has more than one sheet, a notice when the table is cut, then
  // the grid with a header row, row numbers and cells aligned by type, numbers to the right. Column widths come from
  // the header and the first rows, within a minimum and a maximum that follow the text size. The grid scrolls both
  // ways and only the cells in view have nodes. One cell is selected; the arrow keys, Page Up and Down, Home and End
  // move it and keep it in view. The view does no I/O: the owner loads the table and reloads it by sheet index.
  //
  // In editing mode an edit bar shows above the grid and the selected cell opens a one-line field. The view only
  // reports what the user asks for: the owner holds the file, applies each change and sets the new grid.
  class TableView final : public Flex {
  public:
    static constexpr float kDefaultTextSize = 12;

    TableView();
    ~TableView() override;
    TableView(const TableView&) = delete;
    TableView& operator=(const TableView&) = delete;
    TableView(TableView&&) = delete;
    TableView& operator=(TableView&&) = delete;

    // Shows a table at its top left with its first cell selected; none shows nothing.
    void setTable(std::shared_ptr<const formats::TableGrid> table);
    // A sentence shown in place of the grid, such as why a sheet could not be read; empty shows the grid again. The
    // sheet list stays, with the sheet last picked marked, so another sheet can be chosen.
    void setMessage(std::string message);
    // The size of all text in the view, clamped to 6 to 96; other values are ignored. The cell at the top left stays
    // there.
    void setTextSize(float size);
    // Applies what changed since the last call and binds the cells in view, at most once per call. The owner calls
    // it once per frame, before layout.
    void update();
    // Selects a cell of the grid, clamped to the table, and scrolls it into view at the next update. An open cell
    // field on another cell commits.
    void select(TableCell cell);

    // Editing mode, off by default. Turning it off closes an open cell field without a commit.
    void setEditable(bool editable);
    void setEditAvailability(TableEditAvailability availability);
    // Shows the grid the owner made after a change: the selection and the scroll position stay on the same source
    // cell, or move to the nearest cell where it is gone. An open cell field closes without a commit.
    void replaceTable(std::shared_ptr<const formats::TableGrid> table);
    // Offers a key before the page's own shortcuts; true when the table took it, and the page then does nothing more
    // with it. While the cell field is open every key but Ctrl+S is delivered to it here, Tab and Shift+Tab
    // included; while the grid shows and has focus in editing mode the editing keys are the table's. Every other key
    // is left for the page.
    bool offerKey(const InputArea::KeyData& key);
    // Commits an open cell field where it is, as losing focus does; nothing while it is closed. An owner calls it
    // before it asks whether the file has unsaved changes, so a close that leaves the focus alone loses no text.
    void commitField();

    const std::shared_ptr<const formats::TableGrid>& table() const noexcept { return m_table; }
    float textSize() const noexcept { return m_textSize; }
    float scrollX() const noexcept { return m_scrollX; }
    float scrollY() const noexcept { return m_scrollY; }
    std::optional<TableCell> selectedCell() const noexcept { return m_selected; }
    // The selected cell's row and column in the source: the file's row as the delimited readers count it, or the
    // sheet's own row and column.
    std::optional<TableCell> selectedSource() const;
    // The sentence above the grid; empty while none is shown.
    std::string_view notice() const;
    // The grid; it takes the keys.
    TableCanvas& canvas() const noexcept { return *m_canvas; }
    Segmented& sheetList() const noexcept { return *m_sheets; }
    bool editable() const noexcept { return m_editable; }
    // Whether the cell field is open, and so has the keyboard for text.
    bool editingCell() const noexcept { return m_canvas->editor().cell().has_value(); }
    TableEditBar& editBar() const noexcept { return *m_editBar; }

    // A sheet was picked in the sheet list.
    std::function<void(std::size_t sheet)> onSheetChosen;
    // The selection moved; with its source row and column.
    std::function<void(TableCell source)> onSelectionChanged;
    // Something waits for the next update(): the owner schedules a frame.
    std::function<void()> onFrameNeeded;
    // The whole text of a cell by its source row and column, since the grid cuts cell text at 256 bytes and shows
    // control characters as spaces. A cell field opens only with it; without it a tooltip shows the grid's text.
    std::function<std::string(TableCell source)> cellText;
    // A cell field committed text other than the cell's, or a cell was cleared.
    std::function<void(TableCell source, std::string text)> onCellEdited;
    // A button of the edit bar or its key was pressed while the edit is available; after a press on the bar the grid
    // has the keyboard again.
    std::function<void(const TableEditRequest& request)> onEditRequest;
    // Ctrl+S in editing mode, after an open cell field commits.
    std::function<void()> onSaveRequested;
    // Keyboard focus belongs here now: the cell field when it opens, the grid when a key or a scroll closes it.
    std::function<void(InputArea* area)> onFocus;

  private:
    void needFrame();
    void applyTable();
    void planColumns();
    void reveal();
    bool scrollTo(float x, float y);
    void key(const InputArea::KeyData& data);
    TableFrame frame() const;
    // Editing, in table_view_editing.cpp.
    void pressCell(const InputArea::PointerData& pointer);
    bool editKey(const InputArea::KeyData& data);
    void openField(const std::optional<std::string>& typed);
    void fieldClosed(TableFieldExit exit, TableCell source, std::optional<std::string> text);
    void closeOffscreenField();
    void request(TableEdit edit);
    void pressBar(TableEdit edit);
    void syncEditBar();
    std::string wholeText(TableCell source) const;

    std::shared_ptr<const formats::TableGrid> m_table;
    std::string m_message;
    std::string m_notice;
    float m_textSize = kDefaultTextSize;
    std::vector<float> m_edges;
    float m_rowHeight = 0;
    float m_gutterWidth = 0;
    float m_padding = 0;
    float m_scrollX = 0;
    float m_scrollY = 0;
    std::optional<TableCell> m_selected;
    std::optional<TableCell> m_topCell;       // kept at the top left once a new text size is planned
    std::optional<std::size_t> m_chosenSheet; // picked in the sheet list since the table was set
    std::uint64_t m_plannedAdvances = 0;      // the advances generation the column widths come from
    bool m_tableChanged = true;               // sheets and notice apply at the next update
    bool m_planned = false;                   // the column widths fit the table and the text size
    bool m_dirty = true;                      // the grid shows a new frame at the next update
    bool m_reveal = false;                    // the next update scrolls the selected cell into view
    bool m_syncing = false;                   // the sheet list is being set, not picked
    bool m_editable = false;
    TableEditAvailability m_availability;
    std::optional<TableCell> m_lastPress; // the cell of the last primary press, for a double click
    std::uint32_t m_lastPressTime = 0;    // its time in milliseconds
    TableEditBar* m_editBar = nullptr;
    ScrollView* m_sheetBar = nullptr;
    Segmented* m_sheets = nullptr;
    Flex* m_noticeRow = nullptr;
    Label* m_noticeLabel = nullptr;
    TableCanvas* m_canvas = nullptr;
  };
} // namespace luciole
