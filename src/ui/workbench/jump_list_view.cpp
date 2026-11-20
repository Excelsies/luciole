#include "ui/workbench/jump_list_view.h"

#include "core/input/key_symbols.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/list_rows.h"
#include "ui/workbench/workbench_row_tooltip.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  using luciole::JumpNotice;

  bool sameRow(const luciole::workbench::JumpRow& left, const luciole::workbench::JumpRow& right) {
    return left.id == right.id
        && left.path == right.path
        && left.split == right.split
        && left.label == right.label
        && left.share == right.share
        && left.partial == right.partial;
  }

  // Where the cursor at `cursor` in `before` goes in `rows`: its folder's row, else the same place within the rows.
  std::optional<std::size_t> cursorAfter(
      const std::vector<luciole::workbench::JumpRow>& before, std::size_t cursor,
      const std::vector<luciole::workbench::JumpRow>& rows
  ) {
    if (rows.empty()) {
      return std::nullopt;
    }
    if (cursor < before.size()) {
      const auto found = std::ranges::find(rows, before[cursor].path, &luciole::workbench::JumpRow::path);
      if (found != rows.end()) {
        return static_cast<std::size_t>(found - rows.begin());
      }
    }
    return std::min(cursor, rows.size() - 1);
  }

  std::string_view noticeText(JumpNotice notice) {
    switch (notice) {
    case JumpNotice::Empty:
      return "Nothing two levels down";
    case JumpNotice::Measuring:
      return "Measuring…";
    case JumpNotice::None:
      break;
    }
    return {};
  }

  void show(Node& node, bool shown) {
    node.setVisible(shown);
    node.setParticipatesInLayout(shown);
  }
} // namespace

class luciole::JumpListView::Adapter final : public VirtualGridAdapter {
public:
  explicit Adapter(JumpListView& owner) : m_owner(owner) {}

  std::size_t itemCount() const override { return m_owner.m_content.list.rows.size(); }
  std::unique_ptr<Node> createTile() override { return std::make_unique<JumpRowTile>(); }

  void bindTile(Node& tile, std::size_t index, bool, bool) override {
    const auto& rows = m_owner.m_content.list.rows;
    if (index >= rows.size()) {
      return;
    }
    const RowMarks marks{.linked = rows[index].id == m_owner.m_content.linked, .hovered = m_owner.m_hovered == index};
    const bool cursor = m_owner.m_cursor == index && m_owner.m_grid->focusArea()->focused();
    static_cast<JumpRowTile&>(tile).bind(rows[index], marks, cursor);
  }

  bool onPointerPress(std::size_t index, float, float, float, float) override {
    const auto& rows = m_owner.m_content.list.rows;
    m_pressed = index < rows.size() ? std::optional(rows[index].path) : std::nullopt;
    m_dragged = false;
    return true;
  }
  bool onPointerDrag(std::optional<std::size_t>, float, float, float, float) override {
    m_dragged = true;
    return false;
  }
  PointerReleaseResult onPointerRelease(std::optional<std::size_t> index) override {
    const auto pressed = std::exchange(m_pressed, std::nullopt);
    const bool dragged = std::exchange(m_dragged, false);
    const auto& rows = m_owner.m_content.list.rows;
    if (!pressed || dragged || !index || *index >= rows.size() || rows[*index].path != *pressed) {
      m_clicks.reset();
      return {};
    }
    const bool twice = m_clicks.click(rows[*index].id, DoubleClick::Clock::now());
    m_owner.moveCursor(*index);
    if (!twice && m_owner.m_activationMode != ActivationMode::SingleClick) {
      return {};
    }
    m_clicks.reset();
    m_activate = true;
    return {.rebind = false, .activateIndex = index};
  }
  void onPointerCancel() override {
    m_pressed.reset();
    m_dragged = false;
    m_clicks.reset();
  }
  // Runs last: the owner may replace or destroy the view.
  void onActivate(std::size_t index) override {
    if (std::exchange(m_activate, false)) {
      m_owner.activate(index);
    }
  }

private:
  JumpListView& m_owner;
  std::optional<std::string> m_pressed;
  bool m_dragged = false;
  bool m_activate = false;
  DoubleClick m_clicks;
};

luciole::JumpListView::JumpListView() : m_adapter(std::make_unique<Adapter>(*this)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setClipChildren(true);
  addChild(
      ui::virtualGridView(
          {.out = &m_grid,
           .columns = 1,
           .cellHeight = luciole::kJumpRowHeight,
           .squareCells = false,
           .columnGap = 0,
           .rowGap = 0,
           .adapter = m_adapter.get(),
           .flexGrow = 1}
      )
  );
  m_grid->scrollView().setViewportPaddingH(Style::spaceXs);
  m_grid->scrollView().setViewportPaddingV(Style::spaceXs);
  auto* area = m_grid->focusArea();
  area->setTabFocusKey("jump-ahead");
  area->setRetainsFocusOnPointerRelease(true);
  area->setOnKeyDown([this](const InputArea::KeyData& event) {
    if (event.pressed && !event.preedit) {
      (void)key(event.sym, event.modifiers);
    }
  });
  area->setOnFocusGain([this] { refresh(m_cursor); });
  area->setOnFocusLoss([this] { refresh(m_cursor); });
  m_pointer = &GridPointer::attach(*m_grid);
  m_pointer->onMove = [this](std::optional<std::size_t> row, float, float) {
    anchorRowTooltip(*m_pointer, *m_grid, row);
    hover(row);
  };
  m_pointer->onLeave = [this] { hover(std::nullopt); };
  m_pointer->onMiddlePress = [this](std::size_t row) {
    const auto& rows = m_content.list.rows;
    if (row < rows.size() && onMiddleClick) {
      const auto copy = rows[row]; // the owner may replace the rows or destroy the view
      onMiddleClick(copy);
    }
  };
  m_pointer->tooltip = [this](std::size_t row) {
    const auto& rows = m_content.list.rows;
    if (row >= rows.size()) {
      return std::string{};
    }
    // The path, and why the value beside it reads "at least" when it does.
    return rows[row].notCounted.empty() ? rows[row].path : rows[row].path + "\n" + rows[row].notCounted;
  };
  addChild(
      ui::column(
          {.out = &m_noticeBox, .padding = Style::spaceMd},
          ui::label(
              {.out = &m_noticeLabel,
               .text = std::string(noticeText(m_notice)),
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxLines = 2,
               .textAlign = TextAlign::Center}
          )
      )
  );
  show(*m_grid, false);
}

luciole::JumpListView::~JumpListView() {
  auto* area = m_grid->focusArea();
  area->setOnKeyDown({});
  area->setOnFocusGain({});
  area->setOnFocusLoss({});
  m_grid->setAdapter(nullptr);
}

InputArea* luciole::JumpListView::focusArea() const noexcept { return m_grid->focusArea(); }

void luciole::JumpListView::setContent(JumpListContent content, std::uint64_t serial) {
  if (m_serial == serial) {
    return;
  }
  m_serial = serial;
  const auto before = std::exchange(m_content, std::move(content));
  const auto& rows = m_content.list.rows;
  if (rows.size() != before.list.rows.size()) {
    m_grid->notifyDataChanged();
  } else {
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const bool linked = rows[i].id == m_content.linked;
      const bool wasLinked = before.list.rows[i].id == before.linked;
      if (!sameRow(rows[i], before.list.rows[i]) || linked != wasLinked) {
        m_grid->notifyItemChanged(i);
      }
    }
  }
  if (const auto previous = m_cursor) {
    m_cursor = cursorAfter(before.list.rows, *previous, rows);
    if (m_cursor != previous) {
      refresh(previous);
      refresh(m_cursor);
    }
  }
  m_notice = !rows.empty() ? JumpNotice::None : m_content.list.partial ? JumpNotice::Measuring : JumpNotice::Empty;
  m_noticeLabel->setText(noticeText(m_notice));
  show(*m_noticeBox, m_notice != JumpNotice::None);
  show(*m_grid, m_notice == JumpNotice::None);
  markLayoutDirty();
  if (m_hovered && (*m_hovered >= rows.size() || rows[*m_hovered].path != before.list.rows[*m_hovered].path)) {
    const auto index = *m_hovered;
    m_hovered.reset();
    hover(index < rows.size() ? std::optional(index) : std::nullopt);
  }
}

bool luciole::JumpListView::key(std::uint32_t sym, std::uint32_t modifiers) {
  const auto count = m_content.list.rows.size();
  if (count == 0 || modifiers != 0) {
    return false;
  }
  if (KeySymbol::isEnterOrSpace(sym)) {
    if (m_cursor) {
      activate(*m_cursor);
    }
    return true;
  }
  const std::size_t last = count - 1;
  const std::size_t current = m_cursor.value_or(0);
  const std::size_t page = std::max<std::size_t>(1, m_grid->pageItemStride());
  std::size_t next = current;
  if (KeySymbol::isDown(sym)) {
    next = m_cursor ? std::min(current + 1, last) : 0;
  } else if (KeySymbol::isUp(sym)) {
    next = current > 0 ? current - 1 : 0;
  } else if (KeySymbol::isHome(sym)) {
    next = 0;
  } else if (KeySymbol::isEnd(sym)) {
    next = last;
  } else if (KeySymbol::isPageDown(sym)) {
    next = std::min(current + page, last);
  } else if (KeySymbol::isPageUp(sym)) {
    next = current > page ? current - page : 0;
  } else {
    return false;
  }
  moveCursor(next);
  return true;
}

void luciole::JumpListView::moveCursor(std::size_t index) {
  const auto previous = std::exchange(m_cursor, index);
  if (previous != index) {
    refresh(previous);
    refresh(index);
  }
  m_grid->scrollToIndex(index);
}

void luciole::JumpListView::activate(std::size_t index) {
  const auto& rows = m_content.list.rows;
  if (index >= rows.size() || !onActivate) {
    return;
  }
  // A copy: the owner may replace the rows or destroy the view.
  const auto row = rows[index];
  onActivate(row);
}

void luciole::JumpListView::hover(std::optional<std::size_t> index) {
  const auto& rows = m_content.list.rows;
  if (index && *index >= rows.size()) {
    index.reset();
  }
  if (index == m_hovered) {
    return;
  }
  refresh(std::exchange(m_hovered, index));
  refresh(index);
  if (!onHover) {
    return;
  }
  if (index) {
    const auto row = rows[*index];
    onHover(&row);
  } else {
    onHover(nullptr);
  }
}

void luciole::JumpListView::refresh(std::optional<std::size_t> index) {
  if (index) {
    m_grid->notifyItemChanged(*index);
  }
}

// The pointer area follows the grid's rows after either pass.
// A row holds two lines of text, which follow the system font size: the row grows with them past kJumpRowHeight.
void luciole::JumpListView::fitRowHeight(Renderer& renderer) {
  const auto line = [&renderer](float size) {
    const auto metrics = renderer.measureFont(size, TextFont{});
    return metrics.bottom - metrics.top;
  };
  m_grid->setCellHeight(
      std::max(kJumpRowHeight, line(Style::fontSizeCaption) + line(Style::fontSizeMini) + kJumpRowInset)
  );
}

void luciole::JumpListView::doLayout(Renderer& renderer) {
  fitRowHeight(renderer);
  Flex::doLayout(renderer);
  m_pointer->fit();
}

void luciole::JumpListView::doArrange(Renderer& renderer, const LayoutRect& rect) {
  fitRowHeight(renderer);
  Flex::doArrange(renderer, rect);
  m_pointer->fit();
}
