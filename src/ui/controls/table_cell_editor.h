#pragma once

#include "render/scene/node.h"
#include "ui/controls/table_view_cell.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class Input;
class InputArea;

namespace luciole {
  // How a cell field was left: committed where it is, committed with a move to the next cell down, right or left,
  // or cancelled.
  enum class TableFieldExit : std::uint8_t { Stay, Down, Right, Left, Cancel };

  // A one-line text field over one table cell, inside a layer that clips it to the cells' viewport. Enter, Tab,
  // Shift+Tab and Escape leave it, and so does losing focus, which commits. Every other key is the field's.
  class TableCellEditor final : public Node {
  public:
    TableCellEditor();
    ~TableCellEditor() override;
    TableCellEditor(const TableCellEditor&) = delete;
    TableCellEditor& operator=(const TableCellEditor&) = delete;
    TableCellEditor(TableCellEditor&&) = delete;
    TableCellEditor& operator=(TableCellEditor&&) = delete;

    // Opens the field for a cell, by its source row and column, with `text` and the caret at its end. A commit of
    // `original` changes nothing.
    void open(TableCell cell, std::string_view text, std::string original);
    // Closes the field and reports how through onExit; nothing while it is closed.
    void close(TableFieldExit exit);
    void setTextSize(float size);
    // The cell's rectangle in this layer, and the table's padding inside cells.
    void place(const LayoutRect& cell, float padding);

    // The cell under edit; none while the field is closed.
    std::optional<TableCell> cell() const noexcept { return m_cell; }
    Input& field() const noexcept { return *m_field; }
    InputArea& fieldArea() const noexcept;

    // The field closed: how, its cell, and the new text when a commit changed the cell.
    std::function<void(TableFieldExit exit, TableCell cell, std::optional<std::string> text)> onExit;

  protected:
    void doLayout(Renderer& renderer) override;

  private:
    bool fieldKey(std::uint32_t sym, std::uint32_t modifiers);

    Input* m_field = nullptr;
    std::optional<TableCell> m_cell;
    std::string m_original;
    LayoutRect m_rect{};
  };
} // namespace luciole
