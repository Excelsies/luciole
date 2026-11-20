#include "ui/controls/table_view.h"

#include "core/input/key_modifiers.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/table_view_cell.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "workbench/format.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace luciole {
  namespace {
    constexpr float kMinTextSize = 6;
    constexpr float kMaxTextSize = 96;
    constexpr float kPaddingEm = 0.5F;
    constexpr float kMinColumnEm = 3.5F;
    constexpr float kMaxColumnEm = 22;
    // Room for kerning the per-character estimate does not see.
    constexpr float kSlackEm = 0.25F;
    constexpr float kMinGutterEm = 2.5F;
    // Column widths come from this many rows: the first half from the top, the rest spread evenly to the end.
    constexpr std::size_t kSampleRows = 200;
    // The bytes of cell text the sample reads, shared evenly by the columns.
    constexpr std::size_t kSampleBytes = 256U * 1024U;
    constexpr std::size_t kSheetNameChars = 32;
    // The wl_pointer axis of horizontal scrolling.
    constexpr std::uint32_t kHorizontalAxis = 1;

    void show(Node& node, bool shown) {
      node.setVisible(shown);
      node.setParticipatesInLayout(shown);
    }

    // The row of sample `index` among kSampleRows in a table of `rows` rows.
    std::size_t sampleRow(std::size_t index, std::size_t rows) {
      constexpr std::size_t kHead = kSampleRows / 2;
      if (rows <= kSampleRows || index < kHead) {
        return index;
      }
      return kHead + ((rows - 1 - kHead) * (index - kHead) / (kSampleRows - 1 - kHead));
    }

    std::string counted(std::size_t count, std::string_view noun) {
      return std::format("{} {}{}", workbench::countText(static_cast<double>(count)), noun, count == 1 ? "" : "s");
    }

    std::string noticeOf(const formats::TableGrid& table) {
      if (table.rowCount == 0 || table.columnCount == 0) {
        return "This table is empty.";
      }
      const std::string rows = counted(table.rowCount, "row");
      const std::string columns = counted(table.columnCount, "column");
      if (table.rowsCut && table.columnsCut) {
        return std::format("Showing the first {} and {}", rows, columns);
      }
      if (table.rowsCut || table.columnsCut) {
        return std::format("Showing the first {}", table.rowsCut ? rows : columns);
      }
      return table.truncated ? "Some of this table is not shown." : std::string{};
    }
  } // namespace

  TableView::TableView() {
    setDirection(FlexDirection::Vertical);
    setAlign(FlexAlign::Stretch);
    setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
    setClipChildren(true);
    auto bar = ui::scrollView(
        {.out = &m_sheetBar,
         .orientation = ScrollOrientation::Horizontal,
         .scrollbarVisible = false,
         .visible = false,
         .participatesInLayout = false}
    );
    auto sheets = std::make_unique<Segmented>();
    sheets->setOnChange([this](std::size_t index) {
      if (m_syncing) {
        return;
      }
      m_chosenSheet = index;
      if (onSheetChosen) {
        onSheetChosen(index);
      }
    });
    m_sheets = static_cast<Segmented*>(m_sheetBar->content()->addChild(std::move(sheets)));
    addChild(std::move(bar));
    addChild(
        ui::row(
            {.out = &m_noticeRow,
             .paddingV = Style::spaceXs,
             .paddingH = Style::spaceSm,
             .visible = false,
             .participatesInLayout = false},
            ui::label(
                {.out = &m_noticeLabel,
                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                 .maxLines = 2,
                 .flexGrow = 1}
            )
        )
    );
    auto edits = std::make_unique<TableEditBar>();
    edits->setTextSize(m_textSize);
    edits->onRequest = [this](TableEdit edit) { pressBar(edit); };
    m_editBar = static_cast<TableEditBar*>(addChild(std::move(edits)));
    show(*m_editBar, false);
    auto canvas = std::make_unique<TableCanvas>();
    canvas->setFlexGrow(1);
    m_canvas = static_cast<TableCanvas*>(addChild(std::move(canvas)));
    m_canvas->editor().onExit = [this](TableFieldExit exit, TableCell source, std::optional<std::string> text) {
      fieldClosed(exit, source, std::move(text));
    };
    m_canvas->fullText = [this](TableCell cell) {
      if (m_table == nullptr) {
        return std::string{};
      }
      return wholeText({.row = m_table->firstSourceRow + cell.row, .column = m_table->firstSourceColumn + cell.column});
    };
    m_canvas->setTextSize(m_textSize);
    m_canvas->onChanged = [this] {
      m_dirty = true;
      needFrame();
    };
    m_canvas->onScrollbar = [this](float x, float y) {
      if (scrollTo(x, y)) {
        needFrame();
      }
    };
    m_canvas->setOnAxisHandler([this](const InputArea::PointerData& pointer) {
      const float delta = pointer.scrollDelta(m_rowHeight * 3);
      const bool across = pointer.axis == kHorizontalAxis;
      const bool moved = scrollTo(m_scrollX + (across ? delta : 0), m_scrollY + (across ? 0 : delta));
      if (moved) {
        needFrame();
      }
      return moved;
    });
    m_canvas->setOnPress([this](const InputArea::PointerData& pointer) { pressCell(pointer); });
    m_canvas->setOnKeyDown([this](const InputArea::KeyData& data) { key(data); });
  }

  TableView::~TableView() { m_canvas->editor().onExit = {}; }

  void TableView::setTable(std::shared_ptr<const formats::TableGrid> table) {
    m_canvas->editor().close(TableFieldExit::Cancel);
    m_canvas->dropTable();
    m_table = std::move(table);
    m_chosenSheet.reset();
    m_tableChanged = true;
    m_planned = false;
    m_scrollX = 0;
    m_scrollY = 0;
    m_selected.reset();
    needFrame();
    if (m_table != nullptr && m_table->rowCount > 0 && m_table->columnCount > 0) {
      select({.row = 0, .column = 0});
    }
    syncEditBar();
  }

  void TableView::setMessage(std::string message) {
    m_canvas->editor().close(TableFieldExit::Cancel);
    m_canvas->dropTable();
    m_message = std::move(message);
    m_tableChanged = true;
    needFrame();
  }

  void TableView::setTextSize(float size) {
    if (!std::isfinite(size) || size == m_textSize) {
      return;
    }
    m_textSize = std::clamp(size, kMinTextSize, kMaxTextSize);
    m_canvas->setTextSize(m_textSize);
    m_editBar->setTextSize(m_textSize);
    m_tableChanged = true;
    if (m_planned && !m_topCell && m_rowHeight > 0 && m_edges.size() > 1) {
      const auto column = std::ranges::upper_bound(m_edges, m_scrollX) - m_edges.begin() - 1;
      m_topCell = TableCell{
          .row = static_cast<std::size_t>(m_scrollY / m_rowHeight),
          .column = static_cast<std::size_t>(
              std::clamp<std::ptrdiff_t>(column, 0, static_cast<std::ptrdiff_t>(m_edges.size()) - 2)
          ),
      };
    }
    m_planned = false;
    needFrame();
  }

  std::optional<TableCell> TableView::selectedSource() const {
    if (!m_selected || m_table == nullptr) {
      return std::nullopt;
    }
    return TableCell{
        .row = m_table->firstSourceRow + m_selected->row,
        .column = m_table->firstSourceColumn + m_selected->column,
    };
  }

  std::string_view TableView::notice() const { return m_noticeRow->visible() ? m_notice : std::string_view{}; }

  void TableView::select(TableCell cell) {
    if (m_table == nullptr || m_table->rowCount == 0 || m_table->columnCount == 0) {
      return;
    }
    cell.row = std::min(cell.row, m_table->rowCount - 1);
    cell.column = std::min(cell.column, m_table->columnCount - 1);
    m_reveal = true;
    m_dirty = true;
    needFrame();
    if (m_selected == cell) {
      return;
    }
    if (editingCell()) {
      // The commit may give a new grid, so the cell is clamped again once the field is closed.
      m_canvas->editor().close(TableFieldExit::Stay);
      select(cell);
      return;
    }
    const bool first = !m_selected;
    m_selected = cell;
    if (first) {
      syncEditBar();
    }
    if (onSelectionChanged) {
      onSelectionChanged(*selectedSource());
    }
  }

  void TableView::update() {
    if (m_tableChanged) {
      m_tableChanged = false;
      applyTable();
    }
    const auto& advances = m_canvas->advances();
    if (!m_planned || m_plannedAdvances != advances.generation()) {
      if (!advances.ready(m_textSize)) {
        return; // the canvas measures text at its next layout and asks for a frame
      }
      planColumns();
    }
    if (m_reveal) {
      m_reveal = false;
      reveal();
    }
    if (const auto top = std::exchange(m_topCell, std::nullopt); top && top->column + 1 < m_edges.size()) {
      scrollTo(m_edges[top->column], static_cast<float>(top->row) * m_rowHeight);
    }
    // The viewport may have changed since the offsets were set.
    scrollTo(m_scrollX, m_scrollY);
    if (m_dirty) {
      m_dirty = false;
      m_canvas->show(frame());
      closeOffscreenField();
    }
  }

  void TableView::setEditable(bool editable) {
    if (editable == m_editable) {
      return;
    }
    if (!editable) {
      m_canvas->editor().close(TableFieldExit::Cancel);
    }
    m_editable = editable;
    m_lastPress.reset();
    m_canvas->setRetainsFocusOnPointerRelease(editable);
    show(*m_editBar, editable);
    syncEditBar();
    needFrame();
  }

  void TableView::needFrame() {
    if (onFrameNeeded) {
      onFrameNeeded();
    }
  }

  // The sheet list, the notice and whether the grid shows, for the current table, message and text size.
  void TableView::applyTable() {
    const bool sheets = m_table != nullptr && m_table->sheetNames.size() > 1;
    show(*m_sheetBar, sheets);
    m_syncing = true;
    m_sheets->clearOptions();
    if (sheets) {
      for (const auto& name : m_table->sheetNames) {
        m_sheets->addOption(workbench::middleEllipsis(name, kSheetNameChars));
      }
      const std::size_t marked = m_chosenSheet.value_or(m_table->sheetIndex);
      m_sheets->setSelectedIndex(std::min(marked, m_table->sheetNames.size() - 1));
    }
    m_sheets->setFontSize(m_textSize);
    m_syncing = false;
    m_notice = !m_message.empty() ? m_message : m_table != nullptr ? noticeOf(*m_table) : std::string{};
    m_noticeLabel->setText(m_notice);
    m_noticeLabel->setFontSize(m_textSize);
    show(*m_noticeRow, !m_notice.empty());
    show(*m_canvas, m_message.empty());
    m_dirty = true;
  }

  // Row height, padding, gutter and column widths from the text size and a sample of the content.
  void TableView::planColumns() {
    m_planned = true;
    m_dirty = true;
    m_canvas->forget();
    const auto& advances = m_canvas->advances();
    m_plannedAdvances = advances.generation();
    m_rowHeight = std::ceil(m_textSize * kTableRowEm);
    m_padding = std::round(m_textSize * kPaddingEm);
    m_edges.assign(1, 0);
    m_gutterWidth = 0;
    if (m_table == nullptr) {
      return;
    }
    const float least = m_textSize * kMinColumnEm;
    const float most = m_textSize * kMaxColumnEm;
    const std::size_t sample = std::min(m_table->rowCount, kSampleRows);
    const std::size_t budget = kSampleBytes / std::max<std::size_t>(1, m_table->columnCount);
    for (std::size_t column = 0; column < m_table->columnCount; ++column) {
      float widest = advances.width(formats::gridColumnName(*m_table, column), true, most);
      std::size_t read = 0;
      for (std::size_t index = 0; index < sample && widest < most && read < budget; ++index) {
        const auto text = formats::gridCellText(*m_table, sampleRow(index, m_table->rowCount), column);
        read += text.size();
        widest = std::max(widest, advances.width(text, false, most));
      }
      const float wanted = std::ceil(widest + (m_textSize * kSlackEm)) + (2 * m_padding);
      m_edges.push_back(m_edges.back() + std::clamp(wanted, least, most));
    }
    const auto last = workbench::countText(static_cast<double>(m_table->firstRowNumber + m_table->rowCount));
    m_gutterWidth = std::max(m_textSize * kMinGutterEm, std::ceil(advances.width(last, false, most) + (2 * m_padding)));
  }

  // Scrolls the least that brings the selected cell into the viewport.
  void TableView::reveal() {
    if (!m_selected || m_selected->column + 1 >= m_edges.size()) {
      return;
    }
    const float viewW = m_canvas->bodyWidth() - Style::scrollbarWidth;
    const float viewH = m_canvas->bodyHeight() - Style::scrollbarWidth;
    const float left = m_edges[m_selected->column];
    const float right = m_edges[m_selected->column + 1];
    const float top = static_cast<float>(m_selected->row) * m_rowHeight;
    const float bottom = top + m_rowHeight;
    float x = m_scrollX;
    float y = m_scrollY;
    x = right > x + viewW ? right - viewW : x;
    x = left < x ? left : x;
    y = bottom > y + viewH ? bottom - viewH : y;
    y = top < y ? top : y;
    scrollTo(x, y);
  }

  // Sets the offsets within the content; true when they moved.
  bool TableView::scrollTo(float x, float y) {
    const float contentW = m_edges.empty() ? 0 : m_edges.back();
    const float contentH = m_table == nullptr ? 0 : static_cast<float>(m_table->rowCount) * m_rowHeight;
    const float maxX = std::max(0.0F, contentW + Style::scrollbarWidth - m_canvas->bodyWidth());
    const float maxY = std::max(0.0F, contentH + Style::scrollbarWidth - m_canvas->bodyHeight());
    x = std::isfinite(x) ? std::clamp(x, 0.0F, maxX) : m_scrollX;
    y = std::isfinite(y) ? std::clamp(y, 0.0F, maxY) : m_scrollY;
    if (x == m_scrollX && y == m_scrollY) {
      return false;
    }
    m_scrollX = x;
    m_scrollY = y;
    m_dirty = true;
    return true;
  }

  void TableView::key(const InputArea::KeyData& data) {
    if (!m_selected || m_table == nullptr || !m_message.empty() || editKey(data)) {
      return;
    }
    const bool ctrl = (data.modifiers & KeyMod::Ctrl) != 0;
    const auto page = static_cast<std::size_t>(std::max(1.0F, std::floor(m_canvas->bodyHeight() / m_rowHeight) - 1));
    const std::size_t lastRow = m_table->rowCount - 1;
    const std::size_t lastColumn = m_table->columnCount - 1;
    TableCell cell = *m_selected;
    switch (data.sym) {
    case XKB_KEY_Up:
      cell.row -= cell.row > 0 ? 1 : 0;
      break;
    case XKB_KEY_Down:
      cell.row = std::min(lastRow, cell.row + 1);
      break;
    case XKB_KEY_Left:
      cell.column -= cell.column > 0 ? 1 : 0;
      break;
    case XKB_KEY_Right:
      cell.column = std::min(lastColumn, cell.column + 1);
      break;
    case XKB_KEY_Page_Up:
      cell.row -= std::min(cell.row, page);
      break;
    case XKB_KEY_Page_Down:
      cell.row = std::min(lastRow, cell.row + page);
      break;
    case XKB_KEY_Home:
      cell = {.row = ctrl ? 0 : cell.row, .column = 0};
      break;
    case XKB_KEY_End:
      cell = {.row = ctrl ? lastRow : cell.row, .column = lastColumn};
      break;
    default:
      return;
    }
    select(cell);
  }

  TableFrame TableView::frame() const {
    return {
        .table = m_message.empty() ? m_table : nullptr,
        .columnEdges = m_edges,
        .rowHeight = m_rowHeight,
        .headerHeight = m_table == nullptr ? 0 : m_rowHeight,
        .gutterWidth = m_gutterWidth,
        .padding = m_padding,
        .textSize = m_textSize,
        .scrollX = m_scrollX,
        .scrollY = m_scrollY,
        .selected = m_selected,
    };
  }
} // namespace luciole
