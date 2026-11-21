#include "ui/controls/pane_header.h"

#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <utility>

namespace {
  constexpr float kButtonSize = 20;
  constexpr float kGlyphSize = 14;
  constexpr float kButtonPadding = (kButtonSize - kGlyphSize) / 2;

  void show(Node& node, bool shown) {
    node.setVisible(shown);
    node.setParticipatesInLayout(shown);
  }
} // namespace

std::unique_ptr<Button>
paneHeaderButton(std::string_view glyph, std::string_view tooltip, std::function<void()> onClick) {
  auto button = std::make_unique<Button>();
  button->setVariant(ButtonVariant::Ghost);
  button->setGlyph(glyph);
  button->setGlyphSize(kGlyphSize);
  button->setControlHeight(kButtonSize);
  button->setMinWidth(kButtonSize);
  button->setPadding(kButtonPadding);
  button->setTooltip(tooltip);
  button->setOnClick(std::move(onClick));
  return button;
}

std::string closedPaneTitle(std::string_view title, std::string_view reason) {
  return reason.empty() ? std::string(title) : std::format("{} (closed: {})", title, reason);
}

PaneHeader::PaneHeader(PaneHeaderOptions options) : m_options(std::move(options)) {
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Center);
  setGap(Style::spaceXs);
  setPadding(0, 2, 0, Style::spaceSm);
  setMinHeight(kHeight);
  setMaxHeight(kHeight);
  setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
  auto title = std::make_unique<Label>();
  title->setFontSize(Style::fontSizeCaption);
  title->setMaxLines(1);
  title->setFlexGrow(1);
  m_titleLabel = static_cast<Label*>(addChild(std::move(title)));
  if (m_options.expandable) {
    m_expand = static_cast<Button*>(addChild(paneHeaderButton("arrows-maximize", {}, [this] {
      if (m_onExpand) {
        m_onExpand();
      }
    })));
  }
  m_collapse = static_cast<Button*>(addChild(paneHeaderButton("chevron-up", {}, [this] {
    if (m_onCollapse) {
      m_onCollapse();
    }
  })));
  refresh();
}

void PaneHeader::setHeight(float height) {
  setMinHeight(height);
  setMaxHeight(height);
}

void PaneHeader::setTitle(std::string_view title) {
  if (title != m_title) {
    m_title = title;
    refresh();
  }
}

Node* PaneHeader::setControls(std::unique_ptr<Node> controls) {
  if (m_controls != nullptr) {
    (void)removeChild(m_controls);
    m_controls = nullptr;
  }
  if (!controls) {
    return nullptr;
  }
  const auto& kids = children();
  const auto title = std::ranges::find_if(kids, [this](const auto& child) { return child.get() == m_titleLabel; });
  const auto after = static_cast<std::size_t>(title - kids.begin()) + 1;
  m_controls = insertChildAt(std::min(after, kids.size()), std::move(controls));
  return m_controls;
}

void PaneHeader::setCollapsed(bool collapsed) {
  if (collapsed != m_collapsed) {
    m_collapsed = collapsed;
    refresh();
  }
}

void PaneHeader::setExpanded(bool expanded) {
  if (expanded != m_expanded) {
    m_expanded = expanded;
    refresh();
  }
}

void PaneHeader::setClosedReason(std::string_view reason) {
  if (reason != m_reason) {
    m_reason = reason;
    refresh();
  }
}

void PaneHeader::setOnCollapse(std::function<void()> callback) { m_onCollapse = std::move(callback); }

void PaneHeader::setOnExpand(std::function<void()> callback) { m_onExpand = std::move(callback); }

void PaneHeader::refresh() {
  const auto& name = m_options.name;
  const bool forced = !m_reason.empty();
  m_titleLabel->setText(closedPaneTitle(m_title, m_reason));
  // a pane at the top folds up; one at the bottom folds down
  const bool pointsUp = m_collapsed == m_options.collapsesDown;
  m_collapse->setGlyph(pointsUp ? "chevron-up" : "chevron-down");
  m_collapse->setEnabled(!forced);
  m_collapse->setTooltip(
      forced            ? std::format("{}: closed, the window is {}", name, m_reason)
          : m_collapsed ? std::format("Restore {}", name)
                        : std::format("Collapse {}", name)
  );
  show(*m_collapse, !m_expanded || m_expand == nullptr);
  if (m_expand != nullptr) {
    m_expand->setGlyph(m_expanded ? "arrows-minimize" : "arrows-maximize");
    m_expand->setTooltip(
        m_expanded ? std::format("Return {} to its pane", name) : std::format("Expand {} to the whole view", name)
    );
    show(*m_expand, !m_collapsed);
  }
  markLayoutDirty();
}
