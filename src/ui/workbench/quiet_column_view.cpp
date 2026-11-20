#include "ui/workbench/quiet_column_view.h"

#include "browser/location_presentation.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/pane_header.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/list_rows.h"
#include "ui/workbench/workbench_row_tooltip.h"
#include "workbench/column_plan.h"
#include "workbench/format.h"

#include <algorithm>
#include <linux/input-event-codes.h>
#include <string_view>
#include <utility>

namespace {
  using luciole::QuietNotice;

  constexpr auto kHeaderHeight = static_cast<float>(luciole::workbench::kColumnHeaderHeight);

  QuietNotice noticeOf(const luciole::DirectoryModel& listing) {
    const auto& snapshot = listing.snapshot();
    if (snapshot.status == luciole::DirectoryStatus::Error) {
      const bool gone = snapshot.error && snapshot.error->kind == luciole::FileErrorKind::Missing;
      return gone ? QuietNotice::Gone : QuietNotice::Unreadable;
    }
    if (!listing.rows().empty()) {
      return QuietNotice::None;
    }
    const bool loading =
        snapshot.status == luciole::DirectoryStatus::Idle || snapshot.status == luciole::DirectoryStatus::Loading;
    return loading ? QuietNotice::Loading : QuietNotice::Empty;
  }

  std::string_view noticeText(QuietNotice notice) {
    switch (notice) {
    case QuietNotice::Loading:
      return "Loading…";
    case QuietNotice::Empty:
      return "This folder is empty";
    case QuietNotice::Unreadable:
      return "This folder could not be read";
    case QuietNotice::Gone:
      return "This folder no longer exists";
    case QuietNotice::NoFolders:
      return "No folders ahead";
    case QuietNotice::None:
      break;
    }
    return {};
  }

  void show(Node& node, bool shown) {
    node.setVisible(shown);
    node.setParticipatesInLayout(shown);
  }
} // namespace

class luciole::QuietColumnView::Adapter final : public VirtualGridAdapter {
public:
  Adapter(QuietColumnView& owner, FileIconCache& icons, std::function<std::uint32_t()> modifiers)
      : m_owner(owner), m_icons(icons), m_modifiers(std::move(modifiers)) {}

  std::uint32_t modifiers() const { return m_modifiers ? m_modifiers() : 0; }

  std::size_t itemCount() const override { return m_owner.m_rowCount; }
  std::unique_ptr<Node> createTile() override { return std::make_unique<QuietRow>(m_icons, m_owner.m_column); }
  void releaseTile(Node& tile, Renderer& renderer) override { static_cast<QuietRow&>(tile).release(renderer); }

  void bindTile(Node& tile, std::size_t index, bool, bool) override {
    const auto id = m_owner.entryAt(index);
    const auto* entry = id ? m_owner.m_content.listing->entry(*id) : nullptr;
    if (entry == nullptr) {
      return;
    }
    const auto& content = m_owner.m_content;
    const RowMarks marks{
        .followed = content.followed == id,
        .pinned = content.pinned == id,
        .linked = content.linked == id,
        .hovered = m_owner.m_hovered == id,
    };
    const auto share = m_owner.share ? m_owner.share(*id) : std::nullopt;
    static_cast<QuietRow&>(tile).bind(*entry, marks, share, m_owner.m_textSize);
  }

  bool onPointerPress(std::size_t index, float, float, float, float) override {
    m_pressed = m_owner.entryAt(index);
    m_pressModifiers = modifiers();
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
    if (!pressed || dragged || !index || m_owner.entryAt(*index) != pressed) {
      m_clicks.reset();
      return {};
    }
    const bool twice = m_clicks.click(*pressed, DoubleClick::Clock::now());
    m_click = Click{
        .event = {.entry = *pressed, .button = BTN_LEFT, .modifiers = m_pressModifiers},
        .twice = twice,
    };
    return {.rebind = false, .activateIndex = index};
  }
  void onPointerCancel() override {
    m_pressed.reset();
    m_dragged = false;
    m_clicks.reset();
  }
  // Forgets a held press and a first click: the ids of a new listing can repeat the old one's.
  void forgetClicks() {
    onPointerCancel();
    m_click.reset();
  }
  // Runs last: the owner may replace or destroy the view.
  void onActivate(std::size_t) override {
    const auto click = std::exchange(m_click, std::nullopt);
    if (!click) {
      return;
    }
    const auto& callback = click->twice ? m_owner.onRowActivate : m_owner.onRowClick;
    if (callback) {
      callback(click->event);
    }
  }
  void onSecondaryActivate(std::size_t index, float x, float y) override {
    m_clicks.reset();
    const auto id = m_owner.entryAt(index);
    if (id && m_owner.onContextRequest) {
      m_owner.onContextRequest(*id, x, y);
    }
  }

private:
  struct Click {
    QuietRowEvent event;
    bool twice = false;
  };

  QuietColumnView& m_owner;
  FileIconCache& m_icons;
  std::function<std::uint32_t()> m_modifiers;
  std::optional<EntryId> m_pressed;
  std::uint32_t m_pressModifiers = 0;
  bool m_dragged = false;
  DoubleClick m_clicks;
  std::optional<Click> m_click;
};

luciole::QuietColumnView::QuietColumnView(
    FileIconCache& icons, std::size_t column, std::function<std::uint32_t()> modifiers
)
    : m_adapter(std::make_unique<Adapter>(*this, icons, std::move(modifiers))),
      m_column(std::min(column, workbench::kColumnsAhead - 1)), m_textSize(quietTextSize(Style::fontSizeBody, column)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
  setClipChildren(true);
  auto pin = paneHeaderButton("pin", "Pin this column", [this] {
    if (onPinControl) {
      onPinControl();
    }
  });
  m_pin = pin.get();
  addChild(
      ui::row(
          {.out = &m_header,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .paddingH = Style::spaceSm,
           .minHeight = kHeaderHeight,
           .maxHeight = kHeaderHeight},
          ui::label(
              {.out = &m_title,
               .fontWeight = FontWeight::SemiBold,
               .maxLines = 1,
               .ellipsize = TextEllipsize::Middle,
               .flexGrow = 1,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          ),
          ui::label({.out = &m_count, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1}),
          std::move(pin)
      )
  );
  addChild(
      ui::box({.out = &m_rule, .fill = colorSpecFromRole(ColorRole::OutlineVariant), .height = Style::borderWidth})
  );
  addChild(
      ui::virtualGridView(
          {.out = &m_grid,
           .columns = 1,
           .cellHeight = static_cast<float>(workbench::kColumnRowHeights[m_column]),
           .squareCells = false,
           .columnGap = 0,
           .rowGap = 0,
           .adapter = m_adapter.get(),
           .flexGrow = 1}
      )
  );
  m_grid->scrollView().setViewportPaddingH(Style::spaceSm);
  m_grid->scrollView().setViewportPaddingV(Style::spaceXs);
  // Keys stay where they are: a press on a row takes no focus.
  m_grid->focusArea()->setFocusable(false);
  m_pointer = &GridPointer::attach(*m_grid);
  m_pointer->onMove = [this](std::optional<std::size_t> row, float x, float y) { pointerMoved(row, x, y); };
  m_pointer->onLeave = [this] { pointerLeft(); };
  m_pointer->onMiddlePress = [this](std::size_t row) {
    if (const auto id = entryAt(row); id && onRowClick) {
      onRowClick({.entry = *id, .button = BTN_MIDDLE, .modifiers = m_adapter->modifiers()});
    }
  };
  m_pointer->tooltip = [this](std::size_t row) {
    const auto id = entryAt(row);
    const auto* entry = id ? m_content.listing->entry(*id) : nullptr;
    return entry != nullptr ? entry->displayName() : std::string{};
  };
  addChild(
      ui::column(
          {.out = &m_noticeBox, .padding = Style::spaceMd, .visible = false, .participatesInLayout = false},
          ui::label(
              {.out = &m_noticeLabel,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxLines = 3,
               .textAlign = TextAlign::Center}
          )
      )
  );
  applyHeader();
}

luciole::QuietColumnView::~QuietColumnView() { m_grid->setAdapter(nullptr); }

void luciole::QuietColumnView::setContent(QuietColumnContent content, std::uint64_t serial) {
  if (m_serial == serial) {
    return;
  }
  m_serial = serial;
  const bool listing = content.listing != m_content.listing || content.folder != m_content.folder;
  const bool rows = listing || content.rowsSerial != m_content.rowsSerial || content.noFolders != m_content.noFolders;
  const auto before = std::exchange(m_content, std::move(content));
  if (rows) {
    m_notice = m_content.listing != nullptr ? noticeOf(*m_content.listing)
        : m_content.noFolders               ? QuietNotice::NoFolders
                                            : QuietNotice::None;
    m_rowCount = m_content.listing != nullptr && m_notice == QuietNotice::None ? m_content.listing->rows().size() : 0;
    if (listing) {
      m_adapter->forgetClicks();
      m_hovered.reset();
      m_grid->scrollView().setScrollOffset(0);
    } else if (m_hovered && (m_content.listing == nullptr || !m_content.listing->visible(*m_hovered))) {
      m_hovered.reset();
    }
    m_pointer->recheck();
    m_grid->notifyDataChanged();
  } else {
    for (const auto& [was, now] :
         {std::pair{before.followed, m_content.followed}, std::pair{before.pinned, m_content.pinned},
          std::pair{before.linked, m_content.linked}}) {
      if (was != now) {
        refreshRow(was);
        refreshRow(now);
      }
    }
  }
  applyHeader();
}

void luciole::QuietColumnView::setListTextSize(float size) {
  const float textSize = quietTextSize(size, m_column);
  if (textSize == m_textSize) {
    return;
  }
  m_textSize = textSize;
  applyHeader();
  m_grid->notifyDataChanged();
}

std::optional<luciole::EntryId> luciole::QuietColumnView::entryAt(std::size_t index) const {
  if (m_content.listing == nullptr || index >= m_rowCount || index >= m_content.listing->rows().size()) {
    return std::nullopt;
  }
  return m_content.listing->rows()[index];
}

void luciole::QuietColumnView::refreshRow(std::optional<EntryId> entry) {
  if (!entry || m_content.listing == nullptr) {
    return;
  }
  const auto& rows = m_content.listing->rows();
  const auto end = rows.begin() + static_cast<std::ptrdiff_t>(std::min(m_rowCount, rows.size()));
  if (const auto found = std::find(rows.begin(), end, *entry); found != end) {
    m_grid->notifyItemChanged(static_cast<std::size_t>(found - rows.begin()));
  }
}

void luciole::QuietColumnView::pointerMoved(std::optional<std::size_t> index, float sceneX, float sceneY) {
  anchorRowTooltip(*m_pointer, *m_grid, index);
  const auto entry = index ? entryAt(*index) : std::nullopt;
  if (entry != m_hovered) {
    const auto previous = std::exchange(m_hovered, entry);
    if (previous && entryAt(m_hoveredIndex) == previous) {
      m_grid->notifyItemChanged(m_hoveredIndex);
    } else {
      refreshRow(previous);
    }
    if (entry) {
      m_hoveredIndex = *index;
      m_grid->notifyItemChanged(*index);
    }
  }
  if (onPointerMoved) {
    onPointerMoved(entry, sceneX, sceneY);
  }
}

void luciole::QuietColumnView::pointerLeft() {
  if (const auto previous = std::exchange(m_hovered, std::nullopt)) {
    if (entryAt(m_hoveredIndex) == previous) {
      m_grid->notifyItemChanged(m_hoveredIndex);
    } else {
      refreshRow(previous);
    }
  }
  if (onPointerLeft) {
    onPointerLeft();
  }
}

void luciole::QuietColumnView::applyHeader() {
  const auto& folder = m_content.folder;
  m_blank = m_content.listing == nullptr && m_notice == QuietNotice::None;
  const bool headed = !m_blank && m_notice != QuietNotice::NoFolders;
  setFill(m_blank ? clearColorSpec() : colorSpecFromRole(ColorRole::SurfaceContainerLow));
  show(*m_header, headed);
  show(*m_rule, headed);
  m_title->setText(folder ? folder->displayName().value_or(locationEditText(*folder)) : std::string{});
  const bool counted = m_notice == QuietNotice::None && m_rowCount > 0;
  m_count->setText(counted ? workbench::countText(static_cast<double>(m_rowCount)) : std::string{});
  show(*m_count, counted);
  show(*m_pin, folder.has_value());
  m_pin->setGlyph(m_content.columnPinned ? "pinned" : "pin");
  m_pin->setSelected(m_content.columnPinned);
  m_pin->setTooltip(m_content.columnPinned ? "Unpin this column" : "Pin this column");
  m_pin->setGlyphSize(m_textSize);
  for (auto* label : {m_title, m_count, m_noticeLabel}) {
    label->setFontSize(m_textSize);
  }
  m_noticeLabel->setText(noticeText(m_notice));
  show(*m_noticeBox, m_notice != QuietNotice::None);
  show(*m_grid, m_notice == QuietNotice::None && !m_blank);
  markLayoutDirty();
}

// The pointer area follows the grid's rows after either pass.
void luciole::QuietColumnView::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);
  m_pointer->fit();
}

void luciole::QuietColumnView::doArrange(Renderer& renderer, const LayoutRect& rect) {
  Flex::doArrange(renderer, rect);
  m_pointer->fit();
}
