#include "ui/workbench/list_rows.h"

#include "ui/browser/file_icon.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "workbench/column_plan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {
  constexpr std::size_t kQuietColumns = luciole::workbench::kColumnsAhead;
  constexpr float kListTextMin = 6;
  constexpr float kListTextMax = 96;
  constexpr float kBelowList = 1; // the most a column's text may reach is this far under the list's

  constexpr std::array<float, kQuietColumns> kIconSizes{16, 14, 12};
  constexpr std::array<float, kQuietColumns> kBarWidths{44, 32, 22};
  // Columns further right fade their icon and bar.
  constexpr std::array<float, kQuietColumns> kIconOpacity{1.0F, 0.8F, 0.65F};
  constexpr std::array<float, kQuietColumns> kBarOpacity{0.85F, 0.65F, 0.5F};
  constexpr float kRowPadding = 6;
  constexpr float kBarHeight = 4;
  constexpr float kLeadWidth = 2;
  constexpr float kMinShare = 0.06F; // a positive share stays visible
  constexpr float kJumpBarWidth = 44;
  constexpr float kMeasuringOpacity = 0.6F;

  std::size_t metricsColumn(std::size_t column) { return std::min(column, kQuietColumns - 1); }

  float fill(double share) {
    const auto value = static_cast<float>(std::clamp(share, 0.0, 1.0));
    return value > 0 ? std::max(value, kMinShare) : 0.0F;
  }

  std::unique_ptr<ProgressBar> shareBar(float width) {
    auto bar = std::make_unique<ProgressBar>();
    bar->setTrack(colorSpecFromRole(ColorRole::SurfaceContainerHighest));
    bar->setFill(colorSpecFromRole(ColorRole::Primary));
    bar->setRadius(kBarHeight / 2);
    bar->setSize(width, kBarHeight);
    return bar;
  }

  void show(Node& node, bool shown) {
    node.setVisible(shown);
    node.setParticipatesInLayout(shown);
  }

  ColorSpec rowFill(luciole::RowMarks marks) {
    if (marks.followed) {
      return colorSpecFromRole(ColorRole::SurfaceContainerHighest);
    }
    return marks.hovered || marks.linked ? colorSpecFromRole(ColorRole::SurfaceContainerHigh) : clearColorSpec();
  }
} // namespace

float luciole::quietTextSize(float listTextSize, std::size_t column) {
  const float list =
      std::isfinite(listTextSize) ? std::clamp(listTextSize, kListTextMin, kListTextMax) : Style::fontSizeBody;
  const auto rule = static_cast<float>(workbench::columnFont(list, metricsColumn(column)));
  return std::min(rule, list - kBelowList);
}

luciole::QuietRow::QuietRow(FileIconCache& icons, std::size_t column) : m_column(metricsColumn(column)) {
  setHitTestVisible(false);
  addChild(ui::box({.out = &m_background, .styleRadius = Style::radiusMd}));
  addChild(
      ui::box(
          {.out = &m_lead,
           .fill = colorSpecFromRole(ColorRole::Primary),
           .radius = kLeadWidth / 2,
           .visible = false,
           .participatesInLayout = false}
      )
  );
  auto icon = std::make_unique<FileIcon>(icons, kIconSizes[m_column]);
  m_icon = icon.get();
  m_icon->setOpacity(kIconOpacity[m_column]);
  auto bar = shareBar(kBarWidths[m_column]);
  m_bar = bar.get();
  m_bar->setOpacity(kBarOpacity[m_column]);
  addChild(
      ui::row(
          {.out = &m_row,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .configure = [](Flex& row) { row.setPadding(0, kRowPadding); }},
          std::move(icon),
          ui::label(
              {.out = &m_name,
               .maxLines = 1,
               .ellipsize = TextEllipsize::Middle,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          ),
          ui::glyph(
              {.out = &m_pin,
               .glyph = "pinned",
               .color = colorSpecFromRole(ColorRole::Primary),
               .visible = false,
               .participatesInLayout = false}
          ),
          std::move(bar)
      )
  );
}

void luciole::QuietRow::bind(const DirectoryEntry& entry, RowMarks marks, std::optional<double> share, float textSize) {
  m_entry = entry.id;
  m_marks = marks;
  m_icon->bindFile(entry, colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_name->setText(entry.displayName());
  m_name->setFontSize(textSize);
  const bool loud = m_column == 0 || marks.hovered || marks.followed;
  m_name->setColor(colorSpecFromRole(
      marks.linked ? ColorRole::Primary
          : loud   ? ColorRole::OnSurface
                   : ColorRole::OnSurfaceVariant
  ));
  m_background->setFill(rowFill(marks));
  show(*m_lead, marks.followed);
  show(*m_pin, marks.pinned);
  m_pin->setGlyphSize(textSize);
  const bool bar = share.has_value() && std::isfinite(*share);
  show(*m_bar, bar);
  m_bar->setProgress(bar ? fill(*share) : 0.0F);
  markLayoutDirty();
}

void luciole::QuietRow::release(Renderer& renderer) { m_icon->releaseFile(renderer); }

std::optional<float> luciole::QuietRow::share() const noexcept {
  return m_bar->visible() ? std::optional(m_bar->progress()) : std::nullopt;
}

void luciole::QuietRow::doLayout(Renderer& renderer) {
  const float icon = kIconSizes[m_column];
  float used = kRowPadding * 2 + icon + Style::spaceSm;
  if (m_bar->visible()) {
    used += kBarWidths[m_column] + Style::spaceSm;
  }
  if (m_pin->visible()) {
    used += m_name->fontSize() + Style::spaceSm;
  }
  const float name = std::max(1.0F, width() - used);
  m_name->setMinWidth(name);
  m_name->setMaxWidth(name);
  m_background->arrange(renderer, {0, 0, width(), height()});
  m_lead->arrange(renderer, {0, height() / 4, kLeadWidth, height() / 2});
  m_row->arrange(renderer, {0, 0, width(), height()});
}

luciole::JumpRowTile::JumpRowTile() {
  setHitTestVisible(false);
  addChild(ui::box({.out = &m_background, .styleRadius = Style::radiusMd}));
  auto bar = shareBar(kJumpBarWidth);
  m_bar = bar.get();
  addChild(
      ui::row(
          {.out = &m_content,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .configure = [](Flex& row) { row.setPadding(0, Style::spaceSm); }},
          ui::column(
              {.align = FlexAlign::Stretch, .flexGrow = 1},
              ui::label(
                  {.out = &m_name,
                   .fontSize = Style::fontSizeCaption,
                   .maxLines = 1,
                   .ellipsize = TextEllipsize::End,
                   .configure = [](Label& label) { label.setUseMarkup(false); }}
              ),
              ui::label(
                  {.out = &m_path,
                   .fontSize = Style::fontSizeMini,
                   .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                   .maxLines = 1,
                   .ellipsize = TextEllipsize::Middle,
                   .configure = [](Label& label) { label.setUseMarkup(false); }}
              )
          ),
          ui::label(
              {.out = &m_value,
               .fontSize = Style::fontSizeMini,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxLines = 1,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          ),
          std::move(bar)
      )
  );
}

void luciole::JumpRowTile::bind(const workbench::JumpRow& row, RowMarks marks, bool cursor) {
  m_row = row;
  m_marks = marks;
  m_cursor = cursor;
  const auto slash = row.path.rfind('/');
  m_name->setText(slash == std::string::npos ? row.path : row.path.substr(slash + 1));
  m_name->setColor(colorSpecFromRole(marks.linked ? ColorRole::Primary : ColorRole::OnSurface));
  m_path->setText(row.split.prefix + row.split.leaf);
  m_value->setText(row.label);
  m_bar->setProgress(fill(std::isfinite(row.share) ? row.share : 0.0));
  m_content->setOpacity(row.partial ? kMeasuringOpacity : 1.0F);
  m_background->setFill(rowFill(marks));
  if (cursor) {
    m_background->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
  } else {
    m_background->clearBorder();
  }
  markLayoutDirty();
}

float luciole::JumpRowTile::share() const noexcept { return m_bar->progress(); }

void luciole::JumpRowTile::doLayout(Renderer& renderer) {
  m_background->arrange(renderer, {0, 0, width(), height()});
  const float value = m_value->Node::measure(renderer, LayoutConstraints::available(width(), height())).width;
  const float text = std::max(1.0F, width() - Style::spaceSm * 4 - kJumpBarWidth - value);
  m_name->setMaxWidth(text);
  m_path->setMaxWidth(text);
  m_content->arrange(renderer, {0, 0, width(), height()});
}

luciole::GridPointer::GridPointer(VirtualGridView& grid) : m_grid(grid) {
  setAcceptedButtons(buttonMask(BTN_MIDDLE));
  setAcceptedScrollDirections(0);
  setZIndex(1);
  setOnEnter([this](const PointerData& data) { moved(data.localX, data.localY); });
  setOnMotion([this](const PointerData& data) { moved(data.localX, data.localY); });
  setOnPress([this](const PointerData& data) {
    if (!data.pressed || data.button != BTN_MIDDLE || !onMiddlePress) {
      return;
    }
    if (const auto row = m_grid.itemAtScene(data.sceneX, data.sceneY)) {
      onMiddlePress(*row);
    }
  });
  setOnLeave([this] {
    m_row.reset();
    m_pointer.reset();
    if (onLeave) {
      onLeave();
    }
  });
  setTooltipProvider([this] -> TooltipContent {
    if (!m_row || !tooltip) {
      return {};
    }
    auto text = tooltip(*m_row);
    return text.empty() ? TooltipContent{} : TooltipContent{std::move(text)};
  });
}

luciole::GridPointer& luciole::GridPointer::attach(VirtualGridView& grid) {
  return *static_cast<GridPointer*>(grid.focusArea()->addChild(std::make_unique<GridPointer>(grid)));
}

void luciole::GridPointer::fit() {
  const auto* area = m_grid.focusArea();
  setPosition(0, 0);
  setFrameSize(area->width(), area->height());
  const bool recheck = std::exchange(m_recheck, false);
  if (!m_pointer) {
    return;
  }
  const auto row = m_grid.itemAtScene(m_pointer->x, m_pointer->y);
  if (recheck || row != m_row) {
    report(row);
  }
}

void luciole::GridPointer::moved(float localX, float localY) {
  ScenePoint point;
  Node::mapToScene(this, localX, localY, point.x, point.y);
  m_pointer = point;
  report(m_grid.itemAtScene(point.x, point.y));
}

void luciole::GridPointer::report(std::optional<std::size_t> row) {
  if (row != m_row) {
    m_row = row;
    requestTooltipRefresh();
  }
  if (onMove && m_pointer) {
    onMove(row, m_pointer->x, m_pointer->y);
  }
}

bool luciole::DoubleClick::click(std::uint64_t key, Clock::time_point now) {
  const bool twice = m_last == key && now - m_time < kWindow;
  m_last = twice ? std::nullopt : std::optional(key);
  m_time = now;
  return twice;
}
