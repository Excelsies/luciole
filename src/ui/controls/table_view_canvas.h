#pragma once

#include "formats/table_grid.h"
#include "render/scene/input_area.h"
#include "ui/controls/table_view_cell.h"
#include "ui/controls/table_view_pools.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

class Box;
class Renderer;
class Scrollbar;

namespace luciole {
  class TableCellEditor;

  // What the grid shows. The canvas shares the table and copies the column edges.
  struct TableFrame {
    std::shared_ptr<const formats::TableGrid> table;
    std::span<const float> columnEdges; // columnCount + 1 offsets from the left edge of the first column
    float rowHeight = 0;
    float headerHeight = 0;
    float gutterWidth = 0;
    float padding = 0;
    float textSize = 0;
    float scrollX = 0;
    float scrollY = 0;
    std::optional<TableCell> selected = std::nullopt;
  };

  // The grid of a table view: a header row of column names, a gutter of row numbers and the cells, each in a layer
  // that scrolls on its own axes, with grid lines and a mark on the selected cell. Labels exist only for what is in
  // view and come from pools keyed by cell, so a scroll moves the layers and binds only the cells that come into
  // view. Every color is a palette role. A cell field lies over the selected cell while it is open, and a cell whose
  // text its column cuts shows the whole text as a tooltip.
  class TableCanvas final : public InputArea {
  public:
    TableCanvas();
    ~TableCanvas() override;
    TableCanvas(const TableCanvas&) = delete;
    TableCanvas& operator=(const TableCanvas&) = delete;
    TableCanvas(TableCanvas&&) = delete;
    TableCanvas& operator=(TableCanvas&&) = delete;

    // Binds the frame. The cells in view come from the size of the last layout.
    void show(const TableFrame& frame);
    // The table, its text or its widths changed: every label binds again at the next show().
    void forget();
    // The table is replaced: no point lies on a cell until the next show().
    void dropTable();

    // The text size the advances are measured at, at the next layout.
    void setTextSize(float size);
    const TextAdvances& advances() const noexcept { return m_advances; }
    // The viewport of the cells, from the last layout.
    float bodyWidth() const noexcept;
    float bodyHeight() const noexcept;
    // The cell under a point in the canvas, from the last frame shown.
    std::optional<TableCell> cellAt(float x, float y) const;
    // Labels held by the pools: cells, column names and row numbers.
    std::size_t labelCount() const noexcept;
    // The field over the selected cell; the table view opens and closes it.
    TableCellEditor& editor() const noexcept { return *m_editor; }
    // Whether some of a cell of the last frame shown lies in the cells' viewport.
    bool cellInView(TableCell cell) const;

    // The size changed, or advances were measured: the owner shows a new frame.
    std::function<void()> onChanged;
    // A scrollbar was dragged to an offset.
    std::function<void(float x, float y)> onScrollbar;
    // The whole text of a grid cell, for the tooltip of a cell its column cuts.
    std::function<std::string(TableCell cell)> fullText;

  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
    void doLayout(Renderer& renderer) override;

  private:
    void showLabels(
        const TableFrame& frame, std::size_t firstRow, std::size_t lastRow, std::size_t firstColumn,
        std::size_t lastColumn
    );
    void showLines(
        const TableFrame& frame, std::size_t firstRow, std::size_t lastRow, std::size_t firstColumn,
        std::size_t lastColumn
    );
    void showBars(const TableFrame& frame);
    void place();
    void placeEditor();
    void point(const InputArea::PointerData& pointer);
    void hover(std::optional<TableCell> cell);

    struct PointerAt {
      float x = 0;
      float y = 0;
    };

    TextAdvances m_advances;
    TableFrame m_frame;
    std::vector<float> m_edges; // a copy of the frame's column edges
    std::unique_ptr<TableLabelPool> m_cells;
    std::unique_ptr<TableLabelPool> m_names;
    std::unique_ptr<TableLabelPool> m_numbers;
    std::unique_ptr<TableRulePool> m_rowLines;
    std::unique_ptr<TableRulePool> m_columnLines;
    Box* m_cellFill = nullptr;
    Box* m_headerFill = nullptr;
    Box* m_gutterFill = nullptr;
    Box* m_headerRule = nullptr;
    Box* m_gutterRule = nullptr;
    Box* m_mark = nullptr;
    Node* m_cellClip = nullptr;
    Node* m_cellLayer = nullptr;
    Node* m_nameClip = nullptr;
    Node* m_nameLayer = nullptr;
    Node* m_numberClip = nullptr;
    Node* m_numberLayer = nullptr;
    Node* m_rowLineClip = nullptr;
    Node* m_rowLineLayer = nullptr;
    Node* m_columnLineClip = nullptr;
    Node* m_columnLineLayer = nullptr;
    Scrollbar* m_vertical = nullptr;
    Scrollbar* m_horizontal = nullptr;
    TableCellEditor* m_editor = nullptr;
    std::optional<PointerAt> m_pointer; // in the canvas, while it is over it
    std::optional<TableCell> m_hovered;
    std::uint64_t m_markedKey = ~std::uint64_t{0}; // the cell whose label has the selection's color
    float m_shownWidth = -1;
    float m_shownHeight = -1;
    float m_textSize = 0;
    float m_scale = 0;
    std::uint64_t m_metricsGeneration = 0;
  };
} // namespace luciole
