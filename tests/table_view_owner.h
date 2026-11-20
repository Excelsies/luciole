#pragma once

#include "table_view_rig.h"
#include "test_check.h"
#include "ui/controls/input.h"
#include "ui/controls/table_cell_editor.h"
#include "ui/controls/table_edit_bar.h"
#include "ui/controls/table_view.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// An owner of an editable table view in a test: what it hears, and a delimited file it edits.
namespace luciole::test::tableview {
  // What an owner hears from the view.
  struct Heard {
    std::vector<std::pair<TableCell, std::string>> edits;
    std::vector<TableEditRequest> requests;
    int saves = 0;

    void listen(TableView& view) {
      view.onCellEdited = [this](TableCell source, std::string text) { edits.emplace_back(source, std::move(text)); };
      view.onEditRequest = [this](const TableEditRequest& request) { requests.push_back(request); };
      view.onSaveRequested = [this] { ++saves; };
    }
    bool quiet() const { return edits.empty() && requests.empty() && saves == 0; }
  };

  // A delimited file as an owner holds it: a header row and rows of cells, by source row and column. It applies
  // each change the view reports and sets the new grid.
  struct Sheet {
    std::vector<std::vector<std::string>> rows;
    TableView* view = nullptr;

    Sheet(std::size_t rowCount, std::size_t columnCount) {
      rows.resize(rowCount + 1);
      for (std::size_t column = 0; column < columnCount; ++column) {
        rows[0].push_back(std::format("h{}", column));
      }
      for (std::size_t row = 1; row <= rowCount; ++row) {
        for (std::size_t column = 0; column < columnCount; ++column) {
          rows[row].push_back(std::format("r{}c{}", row - 1, column));
        }
      }
    }

    std::shared_ptr<const TableGrid> grid() const {
      return luciole::test::tableview::grid(
          rows.size() - 1, rows[0].size(),
          [this](std::size_t row, std::size_t column) { return rows[row + 1][column]; },
          [this](TableGrid& table) {
            table.columnNames = rows[0];
            table.firstSourceRow = 1;
          }
      );
    }

    void attach(Rig& rig) {
      view = rig.view;
      view->cellText = [this](TableCell source) { return rows.at(source.row).at(source.column); };
      view->onCellEdited = [this](TableCell source, std::string text) {
        rows.at(source.row).at(source.column) = std::move(text);
        view->replaceTable(grid());
      };
      view->onEditRequest = [this](const TableEditRequest& request) { apply(request); };
      view->setEditable(true);
      rig.show(grid());
    }

    void apply(const TableEditRequest& request) {
      const std::size_t row = request.source.row;
      const std::size_t column = request.source.column;
      switch (request.edit) {
      case TableEdit::InsertRowBelow:
        rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(row) + 1, std::vector<std::string>(rows[0].size()));
        break;
      case TableEdit::DeleteRow:
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(row));
        break;
      case TableEdit::InsertColumnRight:
        for (auto& cells : rows) {
          cells.insert(cells.begin() + static_cast<std::ptrdiff_t>(column) + 1, std::string{});
        }
        break;
      case TableEdit::DeleteColumn:
        for (auto& cells : rows) {
          cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(column));
        }
        break;
      case TableEdit::Undo:
      case TableEdit::Redo:
        return;
      }
      view->replaceTable(grid());
    }
  };

  inline TableCellEditor& editor(Rig& rig) { return rig.view->canvas().editor(); }

  inline std::string fieldText(Rig& rig) { return editor(rig).field().value(); }

  inline Point cellPoint(Rig& rig, std::string_view text) {
    const auto* label = labelWith(rig.view->canvas(), text);
    TEST_CHECK(label != nullptr);
    return center(*label);
  }

  inline bool canvasFocused(Rig& rig) { return rig.dispatcher.focusedArea() == &rig.view->canvas(); }

  inline bool fieldFocused(Rig& rig) { return rig.dispatcher.focusedArea() == &editor(rig).fieldArea(); }

  inline std::string tooltip(const InputArea& area) {
    const auto content = area.tooltipContent();
    const auto* text = std::get_if<std::string>(&content);
    return text == nullptr ? std::string{} : *text;
  }

  // A rig whose grid has focus on a selected cell.
  inline void focusCell(Rig& rig, std::string_view text) {
    rig.click(cellPoint(rig, text));
    TEST_CHECK(canvasFocused(rig));
  }
} // namespace luciole::test::tableview
