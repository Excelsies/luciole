#include "ui/controls/table_view_canvas.h"

#include "render/core/renderer.h"
#include "ui/controls/box.h"
#include "ui/controls/label.h"
#include "ui/controls/scrollbar.h"
#include "ui/controls/table_cell_editor.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "workbench/format.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace luciole {
  namespace {
    // Bounds on what one frame shows, whatever the viewport and text size.
    constexpr std::size_t kMaxShownRows = 256;
    constexpr std::size_t kMaxShownColumns = 64;
    constexpr std::uint64_t kNoKey = ~std::uint64_t{0};

    std::uint64_t cellKey(std::size_t row, std::size_t column) {
      return (static_cast<std::uint64_t>(row) << 32U) | static_cast<std::uint64_t>(column);
    }

    TextAlign alignOf(formats::GridCellKind kind) {
      switch (kind) {
      case formats::GridCellKind::Number:
      case formats::GridCellKind::Date:
      case formats::GridCellKind::Time:
        return TextAlign::End;
      case formats::GridCellKind::Boolean:
      case formats::GridCellKind::Error:
        return TextAlign::Center;
      case formats::GridCellKind::Text:
        break;
      }
      return TextAlign::Start;
    }

    std::unique_ptr<Box> fill(ColorRole role) {
      auto box = std::make_unique<Box>();
      box->setFill(colorSpecFromRole(role));
      box->setHitTestVisible(false);
      return box;
    }

    Node* clipLayer(Node& parent, Node*& layer) {
      auto clip = std::make_unique<Node>();
      clip->setClipChildren(true);
      clip->setHitTestVisible(false);
      layer = clip->addChild(std::make_unique<Node>());
      return parent.addChild(std::move(clip));
    }
  } // namespace

  TableCanvas::TableCanvas() {
    setFocusable(true);
    setClipChildren(true);
    m_cellFill = static_cast<Box*>(addChild(fill(ColorRole::SurfaceContainerLow)));
    m_cellClip = clipLayer(*this, m_cellLayer);
    auto mark = fill(ColorRole::PrimaryContainer);
    mark->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
    m_mark = static_cast<Box*>(m_cellLayer->addChild(std::move(mark)));
    m_mark->setVisible(false);
    m_cells = std::make_unique<TableLabelPool>(*m_cellLayer, FontWeight::Normal, ColorRole::OnSurface);
    m_headerFill = static_cast<Box*>(addChild(fill(ColorRole::SurfaceContainer)));
    m_nameClip = clipLayer(*this, m_nameLayer);
    m_names = std::make_unique<TableLabelPool>(*m_nameLayer, FontWeight::SemiBold, ColorRole::OnSurfaceVariant);
    m_gutterFill = static_cast<Box*>(addChild(fill(ColorRole::SurfaceContainer)));
    m_numberClip = clipLayer(*this, m_numberLayer);
    m_numbers = std::make_unique<TableLabelPool>(*m_numberLayer, FontWeight::Normal, ColorRole::OnSurfaceVariant);
    m_rowLineClip = clipLayer(*this, m_rowLineLayer);
    m_rowLines = std::make_unique<TableRulePool>(*m_rowLineLayer);
    m_columnLineClip = clipLayer(*this, m_columnLineLayer);
    m_columnLines = std::make_unique<TableRulePool>(*m_columnLineLayer);
    m_headerRule = static_cast<Box*>(addChild(fill(ColorRole::OutlineVariant)));
    m_gutterRule = static_cast<Box*>(addChild(fill(ColorRole::OutlineVariant)));
    m_editor = static_cast<TableCellEditor*>(addChild(std::make_unique<TableCellEditor>()));
    setOnEnter([this](const InputArea::PointerData& pointer) { point(pointer); });
    setOnMotion([this](const InputArea::PointerData& pointer) { point(pointer); });
    setOnLeave([this] {
      m_pointer.reset();
      hover(std::nullopt);
    });
    setTooltipProvider([this] -> TooltipContent {
      const auto* table = m_frame.table.get();
      if (!m_hovered
          || table == nullptr
          || m_hovered->row >= table->rowCount
          || m_hovered->column + 1 >= m_edges.size()) {
        return {};
      }
      const auto [row, column] = *m_hovered;
      const auto text = formats::gridCellText(*table, row, column);
      const float room = m_edges[column + 1] - m_edges[column] - (2 * m_frame.padding);
      const bool cut = text.size() >= formats::kMaxGridCellBytes || m_advances.width(text, false, room + 1) > room;
      if (!cut) {
        return {};
      }
      return fullText ? fullText(*m_hovered) : std::string(text);
    });
    auto vertical = std::make_unique<Scrollbar>();
    vertical->setOrientation(ScrollOrientation::Vertical);
    vertical->setOnScrollChanged([this](float offset) {
      if (onScrollbar) {
        onScrollbar(m_frame.scrollX, offset);
      }
    });
    m_vertical = static_cast<Scrollbar*>(addChild(std::move(vertical)));
    auto horizontal = std::make_unique<Scrollbar>();
    horizontal->setOrientation(ScrollOrientation::Horizontal);
    horizontal->setOnScrollChanged([this](float offset) {
      if (onScrollbar) {
        onScrollbar(offset, m_frame.scrollY);
      }
    });
    m_horizontal = static_cast<Scrollbar*>(addChild(std::move(horizontal)));
  }

  TableCanvas::~TableCanvas() = default;

  float TableCanvas::bodyWidth() const noexcept { return std::max(0.0F, width() - m_frame.gutterWidth); }

  float TableCanvas::bodyHeight() const noexcept { return std::max(0.0F, height() - m_frame.headerHeight); }

  std::size_t TableCanvas::labelCount() const noexcept { return m_cells->size() + m_names->size() + m_numbers->size(); }

  bool TableCanvas::cellInView(TableCell cell) const {
    if (m_frame.rowHeight <= 0 || cell.column + 1 >= m_edges.size()) {
      return false;
    }
    const float top = (static_cast<float>(cell.row) * m_frame.rowHeight) - m_frame.scrollY;
    const float left = m_edges[cell.column] - m_frame.scrollX;
    const float right = m_edges[cell.column + 1] - m_frame.scrollX;
    return right > 0
        && left < bodyWidth() - Style::scrollbarWidth
        && top + m_frame.rowHeight > 0
        && top < bodyHeight() - Style::scrollbarWidth;
  }

  void TableCanvas::point(const InputArea::PointerData& pointer) {
    m_pointer = PointerAt{.x = pointer.localX, .y = pointer.localY};
    hover(cellAt(pointer.localX, pointer.localY));
  }

  void TableCanvas::hover(std::optional<TableCell> cell) {
    if (cell != m_hovered) {
      m_hovered = cell;
      requestTooltipRefresh();
    }
  }

  void TableCanvas::forget() {
    m_cells->forget();
    m_names->forget();
    m_numbers->forget();
  }

  void TableCanvas::dropTable() { m_frame.table.reset(); }

  std::optional<TableCell> TableCanvas::cellAt(float x, float y) const {
    const auto* table = m_frame.table.get();
    if (table == nullptr
        || m_frame.rowHeight <= 0
        || x < m_frame.gutterWidth
        || y < m_frame.headerHeight
        || m_edges.size() != table->columnCount + 1) {
      return std::nullopt;
    }
    const float contentX = x - m_frame.gutterWidth + m_frame.scrollX;
    const float contentY = y - m_frame.headerHeight + m_frame.scrollY;
    const auto row = static_cast<std::size_t>(std::max(0.0F, contentY / m_frame.rowHeight));
    const auto after = std::ranges::upper_bound(m_edges, contentX);
    if (row >= table->rowCount || after == m_edges.begin() || after == m_edges.end()) {
      return std::nullopt;
    }
    return TableCell{.row = row, .column = static_cast<std::size_t>(after - m_edges.begin() - 1)};
  }

  void TableCanvas::show(const TableFrame& frame) {
    m_frame = frame;
    m_edges.assign(frame.columnEdges.begin(), frame.columnEdges.end());
    m_frame.columnEdges = m_edges;
    const auto* table = frame.table.get();
    const bool usable = table != nullptr && frame.rowHeight > 0 && m_edges.size() == table->columnCount + 1;
    std::size_t firstRow = 0;
    std::size_t lastRow = 0;
    std::size_t firstColumn = 0;
    std::size_t lastColumn = 0;
    if (usable) {
      firstRow = std::min(table->rowCount, static_cast<std::size_t>(std::max(0.0F, frame.scrollY / frame.rowHeight)));
      const auto rowsShown = static_cast<std::size_t>(std::ceil(bodyHeight() / frame.rowHeight)) + 1;
      lastRow = std::min({table->rowCount, firstRow + rowsShown, firstRow + kMaxShownRows});
      const auto after = std::ranges::upper_bound(m_edges, frame.scrollX);
      firstColumn = after == m_edges.begin() ? 0 : static_cast<std::size_t>(after - m_edges.begin() - 1);
      firstColumn = std::min(firstColumn, table->columnCount);
      lastColumn = firstColumn;
      while (lastColumn < table->columnCount
             && lastColumn < firstColumn + kMaxShownColumns
             && m_edges[lastColumn] < frame.scrollX + bodyWidth()) {
        ++lastColumn;
      }
    }
    m_cellLayer->setPosition(-frame.scrollX, -frame.scrollY);
    m_nameLayer->setPosition(-frame.scrollX, 0);
    m_numberLayer->setPosition(0, -frame.scrollY);
    m_rowLineLayer->setPosition(0, -frame.scrollY);
    m_columnLineLayer->setPosition(-frame.scrollX, 0);
    showLabels(frame, firstRow, lastRow, firstColumn, lastColumn);
    showLines(frame, firstRow, lastRow, firstColumn, lastColumn);
    showBars(frame);
    place();
    placeEditor();
    // A scroll moves another cell under a pointer that stays still.
    if (m_pointer) {
      hover(cellAt(m_pointer->x, m_pointer->y));
    }
  }

  // The open field lies over the selected cell, in the coordinates of the cells' viewport.
  void TableCanvas::placeEditor() {
    const auto& selected = m_frame.selected;
    if (!m_editor->cell() || !selected || selected->column + 1 >= m_edges.size()) {
      return;
    }
    const float left = m_edges[selected->column];
    m_editor->place(
        {.x = left - m_frame.scrollX,
         .y = (static_cast<float>(selected->row) * m_frame.rowHeight) - m_frame.scrollY,
         .width = m_edges[selected->column + 1] - left,
         .height = m_frame.rowHeight},
        m_frame.padding
    );
  }

  void TableCanvas::showLabels(
      const TableFrame& frame, std::size_t firstRow, std::size_t lastRow, std::size_t firstColumn,
      std::size_t lastColumn
  ) {
    struct Wanted {
      std::size_t row = 0;
      std::size_t column = 0;
    };
    std::vector<Wanted> cells;
    std::vector<std::size_t> names;
    std::vector<std::size_t> numbers;
    m_cells->begin();
    m_names->begin();
    m_numbers->begin();
    for (std::size_t row = firstRow; row < lastRow; ++row) {
      for (std::size_t column = firstColumn; column < lastColumn; ++column) {
        if (!m_cells->keep(cellKey(row, column))) {
          cells.push_back({.row = row, .column = column});
        }
      }
      if (!m_numbers->keep(row)) {
        numbers.push_back(row);
      }
    }
    for (std::size_t column = firstColumn; column < lastColumn; ++column) {
      if (!m_names->keep(column)) {
        names.push_back(column);
      }
    }
    const float pad = frame.padding;
    const float rowHeight = frame.rowHeight;
    // Without a table the ranges are empty and nothing below reads it.
    static const formats::TableGrid kNone;
    const auto& table = frame.table != nullptr ? *frame.table : kNone;
    for (const auto& [row, column] : cells) {
      const LayoutRect rect{
          .x = m_edges[column] + pad,
          .y = static_cast<float>(row) * rowHeight,
          .width = m_edges[column + 1] - m_edges[column] - (2 * pad),
          .height = rowHeight,
      };
      Label& label = m_cells->claim(cellKey(row, column), rect);
      label.setText(formats::gridCellText(table, row, column));
      label.setFontSize(frame.textSize);
      label.setTextAlign(alignOf(formats::gridCellKind(table, row, column)));
    }
    for (const std::size_t column : names) {
      const LayoutRect rect{
          .x = m_edges[column] + pad,
          .y = 0,
          .width = m_edges[column + 1] - m_edges[column] - (2 * pad),
          .height = frame.headerHeight,
      };
      Label& label = m_names->claim(column, rect);
      label.setText(formats::gridColumnName(table, column));
      label.setFontSize(frame.textSize);
    }
    for (const std::size_t row : numbers) {
      const LayoutRect rect{
          .x = pad,
          .y = static_cast<float>(row) * rowHeight,
          .width = frame.gutterWidth - (2 * pad),
          .height = rowHeight,
      };
      Label& label = m_numbers->claim(row, rect);
      label.setText(workbench::countText(static_cast<double>(table.firstRowNumber + row)));
      label.setFontSize(frame.textSize);
      label.setTextAlign(TextAlign::End);
    }
    m_cells->end();
    m_names->end();
    m_numbers->end();
    // The selected cell's text takes the selection's color, and the cell it left takes back the usual one.
    const std::uint64_t selected = frame.selected ? cellKey(frame.selected->row, frame.selected->column) : kNoKey;
    if (Label* left = m_cells->find(m_markedKey); left != nullptr && m_markedKey != selected) {
      left->setColor(colorSpecFromRole(ColorRole::OnSurface));
    }
    if (Label* label = m_cells->find(selected)) {
      label->setColor(colorSpecFromRole(ColorRole::OnPrimaryContainer));
    }
    m_markedKey = selected;
  }

  void TableCanvas::showLines(
      const TableFrame& frame, std::size_t firstRow, std::size_t lastRow, std::size_t firstColumn,
      std::size_t lastColumn
  ) {
    // Row rules scroll only down and column rules only across, so each ends where the cells in view end.
    const float line = Style::borderWidth;
    const float contentWidth = m_edges.empty() ? 0 : m_edges.back();
    const float contentHeight =
        frame.table == nullptr ? 0 : static_cast<float>(frame.table->rowCount) * frame.rowHeight;
    const float rowRule = frame.gutterWidth + std::max(0.0F, contentWidth - frame.scrollX);
    std::size_t rows = 0;
    for (std::size_t row = firstRow; row < lastRow; ++row) {
      const float y = (static_cast<float>(row + 1) * frame.rowHeight) - line;
      m_rowLines->show(rows++, {.x = 0, .y = y, .width = rowRule, .height = line});
    }
    m_rowLines->hideFrom(rows);
    const float columnRule = frame.headerHeight + std::max(0.0F, contentHeight - frame.scrollY);
    std::size_t columns = 0;
    for (std::size_t column = firstColumn; column < lastColumn; ++column) {
      const float x = m_edges[column + 1] - line;
      m_columnLines->show(columns++, {.x = x, .y = 0, .width = line, .height = columnRule});
    }
    m_columnLines->hideFrom(columns);
    const auto& selected = frame.selected;
    m_mark->setVisible(selected.has_value() && selected->column + 1 < m_edges.size());
    if (m_mark->visible()) {
      const float left = m_edges[selected->column];
      m_mark->setPosition(left, static_cast<float>(selected->row) * frame.rowHeight);
      m_mark->setFrameSize(m_edges[selected->column + 1] - left, frame.rowHeight);
    }
  }

  void TableCanvas::showBars(const TableFrame& frame) {
    // The bars lie over the cells, so the content reaches one bar width further to let its end clear them.
    const float thickness = Style::scrollbarWidth;
    const float contentWidth = m_edges.empty() ? 0 : m_edges.back();
    const float contentHeight =
        frame.table == nullptr ? 0 : static_cast<float>(frame.table->rowCount) * frame.rowHeight;
    m_vertical->setPosition(width() - thickness, frame.headerHeight);
    m_vertical->update(bodyHeight(), contentHeight + thickness, frame.scrollY);
    m_horizontal->setPosition(frame.gutterWidth, height() - thickness);
    m_horizontal->update(bodyWidth(), contentWidth + thickness, frame.scrollX);
  }

  // Frames the fills, rules and clips around the header and gutter at the current size.
  void TableCanvas::place() {
    const float w = width();
    const float h = height();
    const float header = m_frame.headerHeight;
    const float gutter = m_frame.gutterWidth;
    const float bodyW = bodyWidth();
    const float bodyH = bodyHeight();
    const auto frame = [](Node& node, float x, float y, float fw, float fh) {
      node.setPosition(x, y);
      node.setFrameSize(std::max(0.0F, fw), std::max(0.0F, fh));
    };
    frame(*m_cellFill, 0, 0, w, h);
    frame(*m_cellClip, gutter, header, bodyW, bodyH);
    frame(*m_headerFill, 0, 0, w, header);
    frame(*m_nameClip, gutter, 0, bodyW, header);
    frame(*m_gutterFill, 0, header, gutter, bodyH);
    frame(*m_numberClip, 0, header, gutter, bodyH);
    frame(*m_rowLineClip, 0, header, w, bodyH);
    frame(*m_columnLineClip, gutter, 0, bodyW, h);
    frame(*m_headerRule, 0, header - Style::borderWidth, w, Style::borderWidth);
    frame(*m_gutterRule, gutter - Style::borderWidth, 0, Style::borderWidth, h);
    frame(*m_editor, gutter, header, bodyW, bodyH);
  }

  LayoutSize TableCanvas::doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) {
    return constraints.constrain({});
  }

  void TableCanvas::setTextSize(float size) {
    if (size != m_textSize) {
      m_textSize = size;
      m_editor->setTextSize(size);
      markLayoutDirty();
    }
  }

  void TableCanvas::doLayout(Renderer& renderer) {
    // Text measures change with the pixel scale and the fonts.
    const bool fonts = renderer.renderScale() != m_scale || renderer.textMetricsGeneration() != m_metricsGeneration;
    if (fonts) {
      m_scale = renderer.renderScale();
      m_metricsGeneration = renderer.textMetricsGeneration();
      m_cells->remeasure();
      m_names->remeasure();
      m_numbers->remeasure();
    }
    const bool measured = fonts || !m_advances.ready(m_textSize);
    if (measured) {
      m_advances.measure(renderer, m_textSize);
    }
    const bool resized = width() != m_shownWidth || height() != m_shownHeight;
    m_shownWidth = width();
    m_shownHeight = height();
    place();
    m_cells->arrange(renderer);
    m_names->arrange(renderer);
    m_numbers->arrange(renderer);
    if (m_editor->visible()) {
      m_editor->layout(renderer);
    }
    if ((measured || resized) && onChanged) {
      onChanged();
    }
  }
} // namespace luciole
