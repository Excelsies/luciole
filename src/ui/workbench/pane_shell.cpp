#include "ui/workbench/pane_shell.h"

#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/pane_divider.h"
#include "ui/controls/pane_header.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/text_scale.h"
#include "workbench/preview_home.h"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

namespace luciole {
  namespace {
    namespace wb = workbench;

    constexpr std::int32_t kDividerZ = 60; // above the input area of a list's grid (50)
    constexpr std::array<std::string_view, 3> kPaneNames{"Instrument", "Jump ahead", "Ribbon"};
    constexpr std::array<std::string_view, 2> kRailTips{"Show the instrument", "Show Jump ahead"};
    constexpr std::array<std::string_view, 3> kDividerTips{
        "Resize the left column; Left/Right adjusts, Home or a double-click resets",
        "Resize the instrument; Up/Down adjusts, Home or a double-click resets",
        "Resize the ribbon; Up/Down adjusts, Home or a double-click resets",
    };

    // Enumerators index the arrays above; an out-of-range value reads the last entry.
    template <class Enum> std::size_t indexOf(Enum value, std::size_t count) {
      return std::min(static_cast<std::size_t>(value), count - 1);
    }

    std::string_view reasonText(bool forced, wb::ForcedReason why) {
      if (!forced) {
        return {};
      }
      switch (why) {
      case wb::ForcedReason::Short:
        return "too short";
      case wb::ForcedReason::Narrow:
        return "too narrow";
      case wb::ForcedReason::ShortAndNarrow:
        return "too short and narrow";
      case wb::ForcedReason::None:
      default:
        return "too small";
      }
    }
  } // namespace

  PaneShell::PaneShell() {
    setClipChildren(true);
    m_limits = {.instrument = wb::kPlanningInstrumentLimits, .ribbon = wb::planningRibbonLimits(m_kind)};
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
      auto root = std::make_unique<Node>();
      root->setParticipatesInLayout(false);
      root->setClipChildren(true);
      auto& slot = m_slots[i];
      slot.root = addChild(std::move(root));
      if (i >= kPaneNames.size()) {
        continue; // the center has no header
      }
      const auto pane = static_cast<wb::Pane>(i);
      auto header = std::make_unique<PaneHeader>(PaneHeaderOptions{
          .name = std::string(kPaneNames[i]),
          .expandable = pane == wb::Pane::Instrument,
          .collapsesDown = pane == wb::Pane::Jump,
      });
      header->setTitle(kPaneNames[i]);
      m_titles[i].text = kPaneNames[i];
      composeTitle(i);
      header->setOnCollapse([this, pane] { commit(wb::togglePane(m_state, pane)); });
      if (pane == wb::Pane::Instrument) {
        header->setOnExpand([this] { commit(wb::withExpanded(m_state, !m_state.expanded)); });
      }
      slot.header = static_cast<PaneHeader*>(slot.root->addChild(std::move(header)));
    }
    buildRail();
    for (const auto id : {wb::DividerId::Left, wb::DividerId::Instrument, wb::DividerId::Ribbon}) {
      buildDivider(id);
    }
  }

  void PaneShell::buildRail() {
    auto rail = std::make_unique<Flex>();
    rail->setDirection(FlexDirection::Vertical);
    rail->setAlign(FlexAlign::Center);
    rail->setGap(Style::spaceXs);
    rail->setPadding(Style::spaceXs, 0);
    rail->setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
    rail->setParticipatesInLayout(false);
    rail->setVisible(false);
    m_rail = static_cast<Flex*>(addChild(std::move(rail)));
    const std::array<std::string_view, 2> glyphs{"chart-treemap", "arrow-right"}; // Nest, then Jump ahead
    for (std::size_t i = 0; i < m_railButtons.size(); ++i) {
      const auto pane = static_cast<wb::Pane>(i);
      m_railButtons[i] = static_cast<Button*>(m_rail->addChild(paneHeaderButton(glyphs[i], kRailTips[i], [this, pane] {
        commit(wb::togglePane(m_state, pane));
      })));
    }
  }

  void PaneShell::buildDivider(wb::DividerId id) {
    const auto index = indexOf(id, m_dividers.size());
    auto divider =
        std::make_unique<PaneDivider>(id == wb::DividerId::Left ? PaneDividerAxis::Column : PaneDividerAxis::Row);
    divider->setParticipatesInLayout(false);
    divider->setZIndex(kDividerZ);
    divider->setTooltip(std::string(kDividerTips[index]));
    divider->setCallbacks({
        .preview = [this, id](float value) { dragTo(id, value, false); },
        .commit = [this, id](float value) { dragTo(id, value, true); },
        .cancel =
            [this](float) {
              if (m_drag) {
                m_state = std::exchange(m_drag, std::nullopt)->state;
                markLayoutDirty();
              }
            },
        .reset = [this, id] { commit(wb::withDividerDefault(m_state, id, m_kind)); },
    });
    m_dividers[index] = static_cast<PaneDivider*>(addChild(std::move(divider)));
  }

  Node* PaneShell::setContent(PaneRegion region, std::unique_ptr<Node> content) {
    auto& slot = m_slots[indexOf(region, m_slots.size())];
    if (slot.content != nullptr) {
      (void)slot.root->removeChild(slot.content);
      slot.content = nullptr;
    }
    markLayoutDirty();
    if (!content) {
      return nullptr;
    }
    content->setParticipatesInLayout(false);
    slot.content = slot.root->addChild(std::move(content));
    return slot.content;
  }

  void PaneShell::setState(const wb::PaneState& state) {
    m_state = wb::sanitizePaneState(state).state;
    m_drag.reset();
    markLayoutDirty();
  }

  void PaneShell::setLimits(const wb::PartLimits& limits) {
    m_limits = limits;
    markLayoutDirty();
  }

  void PaneShell::setRibbonKind(wb::RibbonKind kind) {
    m_kind = kind;
    markLayoutDirty();
  }

  void PaneShell::setTitle(wb::Pane pane, std::string_view title) {
    const auto index = indexOf(pane, kPaneNames.size());
    auto& entry = m_titles[index];
    if (entry.text == title) {
      return;
    }
    entry.text = title;
    composeTitle(index);
    m_slots[index].header->setTitle(title);
  }

  void PaneShell::composeTitle(std::size_t index) {
    auto& entry = m_titles[index];
    entry.whole = closedPaneTitle(entry.text, entry.reason);
    entry.room = -1;
  }

  std::string_view PaneShell::titleText(std::size_t index) const {
    const auto& entry = m_titles[index];
    switch (entry.form) {
    case TitleForm::Bare:
      return entry.text;
    case TitleForm::Name:
      return kPaneNames[index];
    case TitleForm::Whole:
    default:
      return entry.whole;
    }
  }

  void PaneShell::fitTitle(Renderer& renderer, std::size_t index, const LayoutRect& rect) {
    auto& entry = m_titles[index];
    auto& header = *m_slots[index].header;
    auto& label = *header.titleLabel();
    const float room = label.width();
    if (room != entry.room) {
      entry.room = room;
      const auto fits = [&renderer, &label, room](std::string_view text) {
        return renderer.measureText(text, label.fontSize(), label.font()).width <= room;
      };
      entry.form = fits(entry.whole)                      ? TitleForm::Whole
          : entry.text != entry.whole && fits(entry.text) ? TitleForm::Bare
                                                          : TitleForm::Name;
      if (entry.form == TitleForm::Whole) {
        label.clearTooltip();
      } else {
        label.setTooltip(entry.whole);
      }
    }
    // the header writes its whole title again whenever its own state changes
    if (const auto shown = titleText(index); label.text() != shown) {
      (void)label.setText(shown);
      header.arrange(renderer, rect);
    }
  }

  void PaneShell::setOnCommit(std::function<void(const wb::PaneState&)> callback) { m_onCommit = std::move(callback); }

  Node* PaneShell::content(PaneRegion region) const { return m_slots[indexOf(region, m_slots.size())].content; }

  PaneHeader& PaneShell::header(wb::Pane pane) const { return *m_slots[indexOf(pane, kPaneNames.size())].header; }

  PaneDivider& PaneShell::divider(wb::DividerId id) const { return *m_dividers[indexOf(id, m_dividers.size())]; }

  Button* PaneShell::railButton(wb::Pane pane) const {
    return pane == wb::Pane::Ribbon ? nullptr : m_railButtons[indexOf(pane, m_railButtons.size())];
  }

  bool PaneShell::resizing() const {
    return std::ranges::any_of(m_dividers, [](const PaneDivider* divider) { return divider->dragging(); });
  }

  bool PaneShell::jumpOpen() const { return m_layoutValid && wb::jumpAheadOpen(m_layout); }

  void PaneShell::dragTo(wb::DividerId id, float value, bool finished) {
    if (!m_drag || m_drag->id != id) {
      m_drag = DragStart{.id = id, .state = m_state};
    }
    const wb::ViewSize view{.width = width(), .height = height()};
    const auto result = wb::dragDivider(m_drag->state, view, limits(), m_kind, id, value, 0);
    // a key step at a limit starts from the clamped value, not from past the limit
    divider(id).setValue(static_cast<float>(result.value));
    if (finished) {
      commit(result.state);
      return;
    }
    m_state = result.state;
    markLayoutDirty();
  }

  void PaneShell::commit(const wb::PaneState& state) {
    m_drag.reset();
    m_state = state;
    markLayoutDirty();
    if (m_onCommit) {
      m_onCommit(m_state);
    }
  }

  LayoutSize PaneShell::doMeasure(Renderer&, const LayoutConstraints& constraints) {
    return constraints.constrain(
        {constraints.hasMaxWidth ? constraints.maxWidth : 0, constraints.hasMaxHeight ? constraints.maxHeight : 0}
    );
  }

  void PaneShell::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

  void PaneShell::doLayout(Renderer& renderer) {
    const wb::ViewSize view{.width = width(), .height = height()};
    m_textScale = systemTextScale(); // the headers and the panes' least sizes follow the system font
    const auto scaled = limits();
    const bool current = m_layoutValid
        && m_layout.state == m_state
        && m_layoutView.width == view.width
        && m_layoutView.height == view.height
        && m_layoutLimits == scaled
        && m_layoutKind == m_kind;
    if (!current) {
      m_layout = wb::layoutPanes(m_state, view, scaled, m_kind);
      m_layoutView = view;
      m_layoutLimits = scaled;
      m_layoutKind = m_kind;
      m_layoutValid = true;
      syncChrome();
    }
    const auto& rects = m_layout.rects;
    const auto& collapsed = m_layout.collapsed;
    placeSlot(renderer, PaneRegion::Instrument, rects.instrument, !collapsed.instrument);
    placeSlot(renderer, PaneRegion::Jump, rects.jump, !collapsed.jump);
    placeSlot(renderer, PaneRegion::Ribbon, rects.ribbon, !collapsed.ribbon);
    placeSlot(renderer, PaneRegion::Center, rects.center, true);
    m_rail->setVisible(rects.rail.has_value());
    if (rects.rail) {
      m_rail->arrange(renderer, sceneRect(*rects.rail));
    }
    placeDividers(renderer);
  }

  void PaneShell::placeSlot(Renderer& renderer, PaneRegion region, const std::optional<wb::PaneRect>& rect, bool open) {
    const auto& slot = m_slots[indexOf(region, m_slots.size())];
    slot.root->setVisible(rect.has_value());
    if (!rect) {
      return; // hidden while the instrument is expanded, or replaced by the rail; its content stays
    }
    const auto box = sceneRect(*rect);
    slot.root->setPosition(box.x, box.y);
    slot.root->setFrameSize(box.width, box.height);
    float top = 0;
    if (slot.header != nullptr) {
      top = std::min(static_cast<float>(m_layoutLimits.header), box.height);
      slot.header->setHeight(static_cast<float>(m_layoutLimits.header));
      const LayoutRect bar{0, 0, box.width, top};
      slot.header->arrange(renderer, bar);
      fitTitle(renderer, indexOf(region, m_titles.size()), bar);
    }
    if (slot.content != nullptr) {
      slot.content->setVisible(open);
      if (open) {
        slot.content->arrange(renderer, {0, top, box.width, std::max(0.0F, box.height - top)});
      }
    }
  }

  void PaneShell::placeDividers(Renderer& renderer) {
    constexpr double kHalf = PaneDivider::kHitSize / 2;
    for (std::size_t i = 0; i < m_dividers.size(); ++i) {
      auto& divider = *m_dividers[i];
      const auto found = std::ranges::find(m_layout.dividers, static_cast<wb::DividerId>(i), &wb::Divider::id);
      if (found == m_layout.dividers.end()) {
        // a divider keeps its place and stays attached until its own drag ends; a removed one takes no keys
        divider.setVisible(divider.dragging());
        divider.setDraggable(false);
        continue;
      }
      const auto& line = *found;
      const auto rect = line.axis == wb::DividerAxis::Vertical
          ? sceneRect({.x = line.x - kHalf, .y = line.y, .width = PaneDivider::kHitSize, .height = line.length})
          : sceneRect({.x = line.x, .y = line.y - kHalf, .width = line.length, .height = PaneDivider::kHitSize});
      divider.setVisible(true);
      divider.setDraggable(line.draggable);
      divider.setValue(static_cast<float>(line.value));
      divider.arrange(renderer, rect);
    }
  }

  void PaneShell::syncChrome() {
    const auto& layout = m_layout;
    const std::array<bool, 3> collapsed{layout.collapsed.instrument, layout.collapsed.jump, layout.collapsed.ribbon};
    const std::array<bool, 3> forced{layout.forced.instrument, layout.forced.jump, layout.forced.ribbon};
    const std::array<wb::ForcedReason, 3> why{layout.why.instrument, layout.why.jump, layout.why.ribbon};
    for (std::size_t i = 0; i < kPaneNames.size(); ++i) {
      auto& header = *m_slots[i].header;
      const auto reason = reasonText(forced[i], why[i]);
      header.setCollapsed(collapsed[i]);
      header.setClosedReason(reason);
      if (reason != m_titles[i].reason) {
        m_titles[i].reason = reason;
        composeTitle(i);
      }
    }
    m_slots[0].header->setExpanded(layout.expanded);
    for (std::size_t i = 0; i < m_railButtons.size(); ++i) {
      const auto reason = reasonText(forced[i], why[i]);
      if (reason == m_railReasons[i]) {
        continue;
      }
      m_railReasons[i] = reason;
      auto& button = *m_railButtons[i];
      button.setEnabled(reason.empty());
      button.setTooltip(
          reason.empty() ? std::string(kRailTips[i])
                         : std::format("{}: closed, the window is {}", kPaneNames[i], reason)
      );
    }
  }

  LayoutRect PaneShell::sceneRect(const wb::PaneRect& rect) const {
    // the layout covers whole pixels: an edge on its border reaches the shell's real, possibly fractional, edge
    const auto reach = [](double edge, double whole, float real) {
      return edge >= whole && real > whole ? static_cast<double>(real) : edge;
    };
    const double right = reach(rect.x + rect.width, m_layout.view.width, width());
    const double bottom = reach(rect.y + rect.height, m_layout.view.height, height());
    const double x = Style::rtl() ? reach(m_layout.view.width, m_layout.view.width, width()) - right : rect.x;
    return {
        static_cast<float>(x), static_cast<float>(rect.y), static_cast<float>(right - rect.x),
        static_cast<float>(bottom - rect.y)
    };
  }
} // namespace luciole
