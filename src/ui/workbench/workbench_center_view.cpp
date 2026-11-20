#include "ui/workbench/workbench_center_view.h"

#include "browser/workbench_model.h"
#include "render/animation/motion_service.h"
#include "ui/controls/pane_divider.h"
#include "ui/style.h"
#include "ui/workbench/quiet_column_view.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace luciole {
  namespace {
    namespace wb = workbench;

    constexpr std::int32_t kDividerZ = 60; // above the input area of a list's grid (50)
    constexpr std::array<std::string_view, wb::kColumnsAhead> kDividerTips{
        "Resize the first column ahead; Left/Right adjusts, Home or a double-click resets",
        "Resize the second column ahead; Left/Right adjusts, Home or a double-click resets",
        "Resize the third column ahead; Left/Right adjusts, Home or a double-click resets",
    };

    // Owned nodes go before the dividers, so the dividers stay last and on top.
    constexpr std::size_t kContentIndex = 1;

    // Opacity the motion starts from; the columns ahead start from nothing.
    constexpr float kListFadeFrom = 0.35F;
    constexpr float kStripsFadeFrom = 0.5F;

    void atRest(Node& node) {
      node.setTranslate(0, 0);
      node.setOpacity(1);
    }
  } // namespace

  CenterMotion centerMotion(const Location& from, const Location& to) {
    if (to.isInside(from)) {
      return CenterMotion::Deeper;
    }
    return from.isInside(to) ? CenterMotion::Up : CenterMotion::Across;
  }

  WorkbenchCenterView::WorkbenchCenterView() {
    setClipChildren(true);
    auto strips = std::make_unique<AncestorStrips>();
    strips->setParticipatesInLayout(false);
    m_strips = static_cast<AncestorStrips*>(addChild(std::move(strips)));
    m_strips->onHover = [this](std::optional<std::uint64_t> id) {
      if (onStripHover) {
        onStripHover(id && *id < m_ancestors.size() ? std::optional(m_ancestors[*id]) : std::nullopt);
      }
    };
    m_strips->onActivate = [this](std::uint64_t id, std::uint32_t button) {
      if (id >= m_ancestors.size() || !onStripActivate) {
        return;
      }
      if (button == 0) {
        m_stripKeyFrom = m_folder; // the focused strip may hide in the new folder
      }
      const auto folder = m_ancestors[id]; // a copy: the owner may change the folder
      onStripActivate(folder, button);
    };
    for (std::size_t column = 0; column < wb::kColumnsAhead; ++column) {
      buildDivider(column);
    }
    m_stripsEdge =
        std::make_unique<StripsPaneEdge>(*this, kDividerZ, [this] { markLayoutDirty(); }, [this] { commit(m_widths); });
  }

  void WorkbenchCenterView::buildDivider(std::size_t column) {
    auto divider = std::make_unique<PaneDivider>(PaneDividerAxis::Column);
    divider->setParticipatesInLayout(false);
    divider->setZIndex(kDividerZ);
    divider->setTooltip(std::string(kDividerTips[column]));
    divider->setCallbacks({
        .preview = [this, column](float value) { dragTo(column, value, false); },
        .commit =
            [this, column](float value) {
              if (m_drag && m_drag->column == column) {
                dragTo(column, value, true);
              } else {
                step(column, value);
              }
            },
        .cancel = [this](float) { cancel(); },
        .reset = [this, column] { reset(column); },
    });
    m_dividers[column] = static_cast<PaneDivider*>(addChild(std::move(divider)));
  }

  std::unique_ptr<Node> WorkbenchCenterView::setList(std::unique_ptr<Node> list) {
    std::unique_ptr<Node> previous;
    if (m_list != nullptr) {
      previous = removeChild(std::exchange(m_list, nullptr));
      atRest(*previous);
    }
    if (list) {
      m_list = insertChildAt(kContentIndex, std::move(list));
    }
    markLayoutDirty();
    return previous;
  }

  std::unique_ptr<Node> WorkbenchCenterView::setColumn(std::size_t slot, std::unique_ptr<Node> content) {
    if (slot >= wb::kColumnsAhead) {
      return content;
    }
    std::unique_ptr<Node> previous;
    if (m_columns[slot] != nullptr) {
      previous = removeChild(std::exchange(m_columns[slot], nullptr));
      previous->setVisible(true);
      atRest(*previous);
    }
    if (content) {
      m_columns[slot] = insertChildAt(kContentIndex, std::move(content));
    }
    markLayoutDirty();
    return previous;
  }

  void WorkbenchCenterView::setSpanFrom(std::optional<std::size_t> slot) {
    m_spanFrom = slot;
    // the motion moves only the slots before the span, so the ones from it on stay where it left them otherwise
    for (std::size_t at = slot.value_or(wb::kColumnsAhead); at < wb::kColumnsAhead; ++at) {
      if (m_columns[at] != nullptr) {
        atRest(*m_columns[at]);
      }
    }
    markLayoutDirty();
  }

  void WorkbenchCenterView::setFolder(const Location& folder) {
    if (m_folder == folder) {
      return;
    }
    std::vector<AncestorEntry> entries;
    m_ancestors.clear();
    auto at = folder.parent();
    for (std::size_t i = 0; i < WorkbenchModel::kMaxAncestors && at; ++i) {
      entries.push_back({
          .name = at->displayName().value_or(at->uri()),
          .path = at->nativePath().value_or(at->uri()),
          .id = i,
      });
      m_ancestors.push_back(*at);
      at = at->parent();
    }
    const auto from = std::exchange(m_folder, folder);
    setAncestors(std::move(entries));
    if (from) {
      beginMotion(centerMotion(*from, folder));
    }
  }

  void WorkbenchCenterView::setAncestors(std::vector<AncestorEntry> ancestors) {
    m_ancestorCount = ancestors.size();
    m_strips->setAncestors(std::move(ancestors));
    if (m_planned.width >= 0) {
      replan(); // the strips know their new levels before the next layout, for the keyboard focus
    }
    markLayoutDirty();
  }

  void WorkbenchCenterView::setPointedFolder(const std::optional<Location>& folder) {
    std::optional<std::uint64_t> strip;
    if (folder) {
      const auto found = std::ranges::find(m_ancestors, *folder);
      if (found != m_ancestors.end()) {
        strip = static_cast<std::uint64_t>(std::distance(m_ancestors.begin(), found));
      }
    }
    m_strips->setPointed(strip);
  }

  std::optional<InputArea*> WorkbenchCenterView::takeStripFocus() {
    if (!m_stripKeyFrom || !m_strips->focused()) {
      m_stripKeyFrom.reset();
      return std::nullopt;
    }
    if (m_folder == m_stripKeyFrom) {
      return std::nullopt;
    }
    m_stripKeyFrom.reset();
    return m_strips->nearest(); // setFolder planned the strips of the new folder already
  }

  void WorkbenchCenterView::beginMotion(CenterMotion motion) {
    stopMotion();
    auto* animations = animationManager();
    if (animations == nullptr || !MotionService::instance().enabled() || m_planned.width <= 0 || !shownInScene()) {
      return;
    }
    const float travel = motion == CenterMotion::Deeper ? kMotionSlide
        : motion == CenterMotion::Up                    ? -kMotionSlide
                                                        : 0.0F;
    m_slide = Style::rtl() ? -travel : travel;
    showMotion(0);
    const auto run = m_motionRun;
    m_motion = animations->animate(
        0.0F, 1.0F, m_stripsShown ? kMotionMs : kFlatMotionMs, Easing::EaseOutCubic,
        [this, run](float progress) {
          if (run == m_motionRun) {
            showMotion(progress);
          }
        },
        [this, run] {
          if (run == m_motionRun) {
            m_motion = 0;
            showMotion(1);
          }
        },
        this
    );
  }

  void WorkbenchCenterView::settle() {
    if (moving() && !shownInScene()) {
      stopMotion();
    }
  }

  void WorkbenchCenterView::stopMotion() {
    if (m_motion != 0 && animationManager() != nullptr) {
      animationManager()->cancel(m_motion);
    }
    m_motion = 0;
    ++m_motionRun;
    showMotion(1);
  }

  void WorkbenchCenterView::showMotion(float progress) {
    const float offset = m_slide * (1 - progress);
    const auto place = [offset, progress](Node* node, float from) {
      if (node != nullptr) {
        node->setTranslate(offset, 0);
        node->setOpacity(from + ((1 - from) * progress));
      }
    };
    place(m_strips, kStripsFadeFrom);
    place(m_list, kListFadeFrom);
    // a preview's slot keeps still: the page places the preview pane at its scene rectangle
    const std::size_t still = std::min(m_spanFrom.value_or(wb::kColumnsAhead), wb::kColumnsAhead);
    for (std::size_t slot = 0; slot < still; ++slot) {
      place(m_columns[slot], 0);
    }
  }

  bool WorkbenchCenterView::shownInScene() const {
    for (const Node* at = this; at != nullptr; at = at->parent()) { // bounded by the depth of the scene
      if (!at->visible()) {
        return false;
      }
    }
    return true;
  }

  void WorkbenchCenterView::setStripsShown(bool shown) {
    m_stripsShown = shown;
    markLayoutDirty();
  }

  void WorkbenchCenterView::setWidths(const wb::ColumnWidths& widths) {
    if (resizing()) {
      return;
    }
    m_widths = wb::normalizeWidths(widths);
    markLayoutDirty();
  }

  void WorkbenchCenterView::setStripsWidth(std::optional<double> width) {
    if (!resizing()) {
      m_stripsEdge->setWidth(width);
    }
  }

  void WorkbenchCenterView::setOnWidths(std::function<void(const wb::ColumnWidths&)> callback) {
    m_onWidths = std::move(callback);
  }

  Node* WorkbenchCenterView::column(std::size_t slot) const {
    return slot < wb::kColumnsAhead ? m_columns[slot] : nullptr;
  }

  PaneDivider& WorkbenchCenterView::divider(std::size_t column) const {
    return *m_dividers[std::min(column, wb::kColumnsAhead - 1)];
  }

  bool WorkbenchCenterView::resizing() const {
    return m_stripsEdge->divider().dragging()
        || std::ranges::any_of(m_dividers, [](const PaneDivider* divider) { return divider->dragging(); });
  }

  void WorkbenchCenterView::dragTo(std::size_t column, float value, bool finished) {
    if (!m_drag || m_drag->column != column) {
      // the divider's value at the press is the column's edge in the plan it was last placed by
      const auto& base = m_dragBases[column];
      if (!base) {
        return;
      }
      m_drag = base->drag;
      m_held = base->columns;
    }
    const auto next = wb::dragColumn(*m_drag, value, wb::kListFloor);
    if (finished) {
      commit(next);
      return;
    }
    if (next != m_widths) {
      m_widths = next;
      markLayoutDirty();
    }
  }

  void WorkbenchCenterView::step(std::size_t column, float value) {
    if (column >= m_plan.columns.size()) {
      return;
    }
    const auto& planned = m_plan.columns[column];
    // moving the edge toward the list widens the column
    const auto next = wb::resizeColumn(m_widths, m_plan, column, planned.width - (value - planned.x), wb::kListFloor);
    // a step at a limit continues from the clamped edge, also before the next layout
    divider(column).setValue(static_cast<float>(planned.x - (next.columns[column] - planned.width)));
    commit(next);
  }

  void WorkbenchCenterView::reset(std::size_t column) {
    if (column >= m_plan.columns.size()) {
      return;
    }
    auto next = wb::resizeColumn(m_widths, m_plan, column, wb::kDefaultColumnWidths[column], wb::kListFloor);
    if (next.columns == wb::kDefaultColumnWidths) {
      next.list.reset(); // all three at their defaults: the narrower list is forgotten too
    }
    commit(next);
  }

  void WorkbenchCenterView::cancel() {
    if (m_drag) {
      m_widths = m_drag->widths;
    }
    m_drag.reset();
    m_held.reset();
    markLayoutDirty();
  }

  void WorkbenchCenterView::commit(const wb::ColumnWidths& widths) {
    m_drag.reset();
    m_held.reset();
    m_widths = widths;
    markLayoutDirty();
    if (m_onWidths) {
      m_onWidths(m_widths);
    }
  }

  LayoutSize WorkbenchCenterView::doMeasure(Renderer&, const LayoutConstraints& constraints) {
    return constraints.constrain(
        {constraints.hasMaxWidth ? constraints.maxWidth : 0, constraints.hasMaxHeight ? constraints.maxHeight : 0}
    );
  }

  void WorkbenchCenterView::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

  void WorkbenchCenterView::replan() {
    const Inputs inputs{
        .width = width(),
        .ancestors = m_ancestorCount,
        .strips = m_stripsShown,
        .widths = m_widths,
        .stripsWidth = m_stripsEdge->width(),
        .held = m_held,
    };
    if (inputs == m_planned) {
      return;
    }
    auto plan = wb::planColumns(
        inputs.width, inputs.ancestors,
        {.strips = inputs.strips,
         .widths = inputs.widths,
         .listMin = wb::kListFloor,
         .maxColumns = inputs.held,
         .stripsWidth = inputs.stripsWidth}
    );
    if (plan.strips != m_plan.strips) {
      m_strips->setPlan(plan.strips);
    }
    m_plan = std::move(plan);
    m_planned = inputs;
  }

  void WorkbenchCenterView::doLayout(Renderer& renderer) {
    replan();
    m_strips->setVisible(!m_plan.strips.empty());
    if (!m_plan.strips.empty()) {
      m_strips->arrange(renderer, sceneRect(m_plan.margin, m_plan.stripsWidth));
    }
    if (m_list != nullptr) {
      m_list->arrange(renderer, sceneRect(m_plan.list.x, m_plan.list.width));
    }
    placeColumns(renderer);
    placeDividers(renderer);
    const double edge = m_plan.margin + m_plan.stripsWidth;
    m_stripsEdge->place(renderer, m_plan, sceneRect(edge - (PaneDivider::kHitSize / 2), PaneDivider::kHitSize));
  }

  void WorkbenchCenterView::placeColumns(Renderer& renderer) {
    const auto& columns = m_plan.columns;
    // the slot whose node takes the room of every planned column from it on; none past the planned ones
    const std::size_t span = m_spanFrom.value_or(columns.size());
    for (std::size_t slot = 0; slot < wb::kColumnsAhead; ++slot) {
      Node* node = m_columns[slot];
      if (node == nullptr) {
        continue;
      }
      const bool shown = slot < columns.size() && slot <= span;
      node->setVisible(shown);
      if (!shown) {
        continue; // its node keeps its state while hidden
      }
      const auto& planned = columns[slot];
      const double right = slot == span ? columns.back().x + columns.back().width : planned.x + planned.width;
      node->arrange(renderer, sceneRect(planned.x, right - planned.x));
    }
  }

  void WorkbenchCenterView::placeDividers(Renderer& renderer) {
    constexpr double kHalf = PaneDivider::kHitSize / 2;
    const auto& columns = m_plan.columns;
    const std::size_t span = m_spanFrom.value_or(columns.size());
    for (std::size_t column = 0; column < wb::kColumnsAhead; ++column) {
      auto& divider = *m_dividers[column];
      const bool dragging = divider.dragging();
      // a column with nothing to show has no edge to drag
      const auto* quiet = dynamic_cast<const QuietColumnView*>(m_columns[column]);
      if (column >= columns.size() || column > span || (quiet != nullptr && quiet->blank())) {
        // a divider keeps its place and stays attached until its own drag ends; a hidden one takes no keys
        divider.setVisible(dragging);
        divider.setDraggable(false);
        if (!dragging) {
          m_dragBases[column].reset();
        }
        continue;
      }
      divider.setVisible(true);
      divider.setDraggable(true);
      divider.setValue(static_cast<float>(columns[column].x));
      if (!dragging) { // like the divider's value, the base stays as it was at the press
        m_dragBases[column] = wb::startColumnDrag(m_plan, column, columns[column].x, m_widths)
                                  .transform([&columns](const wb::ColumnDrag& drag) {
                                    return DragBase{.drag = drag, .columns = columns.size()};
                                  });
      }
      divider.arrange(renderer, sceneRect(columns[column].x - kHalf, PaneDivider::kHitSize));
    }
  }

  LayoutRect WorkbenchCenterView::sceneRect(double x, double extent) const {
    const double left = Style::rtl() ? static_cast<double>(width()) - x - extent : x;
    return {static_cast<float>(left), 0, static_cast<float>(extent), height()};
  }
} // namespace luciole
