#include "ui/workbench/column_preview_header.h"

#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace luciole {
  namespace {
    constexpr float kPartGap = Style::spaceSm;
  } // namespace

  struct ColumnPreviewHeaderRow::Place {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
  };

  struct ColumnPreviewHeaderRow::Plan {
    Place name;
    Place controls;
    Place open;
    bool showReset = true;
    float height = 0;
  };

  ColumnPreviewHeaderRow::ColumnPreviewHeaderRow(
      std::unique_ptr<Label> name, std::unique_ptr<Flex> controls, Button& reset, std::unique_ptr<Button> open
  )
      : m_name(static_cast<Label*>(addChild(std::move(name)))),
        m_controls(static_cast<Flex*>(addChild(std::move(controls)))), m_reset(&reset),
        m_open(static_cast<Button*>(addChild(std::move(open)))) {
    const auto& parts = m_controls->children();
    m_resetIndex = static_cast<std::size_t>(
        std::ranges::find_if(parts, [this](const auto& part) { return part.get() == m_reset; }) - parts.begin()
    );
  }

  float ColumnPreviewHeaderRow::controlsWidth(Renderer& renderer, bool withReset) {
    const auto natural = [&renderer](Node& node) { return node.measure(renderer, LayoutConstraints::unconstrained()); };
    float sum = withReset ? natural(*m_reset).width : 0;
    for (const auto& child : m_controls->children()) {
      if (child.get() != m_reset) {
        sum += natural(*child).width;
      }
    }
    return sum;
  }

  ColumnPreviewHeaderRow::Plan ColumnPreviewHeaderRow::plan(Renderer& renderer, float width) {
    const auto natural = [&renderer](Node& node) { return node.measure(renderer, LayoutConstraints::unconstrained()); };
    Plan plan;
    plan.showReset = controlsWidth(renderer, true) <= width;
    const float controls = controlsWidth(renderer, plan.showReset);
    const float openWidth = natural(*m_open).width;
    const float lineHeight = std::max(natural(*m_controls).height, natural(*m_open).height);
    const float nameRoom = kNameRoomPerText * m_name->fontSize();
    plan.controls.width = controls;
    plan.open.width = openWidth;
    plan.name.height = plan.controls.height = plan.open.height = lineHeight;
    float lines = 1;
    if (nameRoom + kPartGap + controls + kPartGap + openWidth <= width) {
      plan.name.width = width - controls - openWidth - (2 * kPartGap);
      plan.controls.x = plan.name.width + kPartGap;
      plan.open.x = width - openWidth;
    } else if (nameRoom + kPartGap + openWidth <= width) {
      plan.name.width = width - openWidth - kPartGap;
      plan.open.x = width - openWidth;
      plan.controls.y = lineHeight + kLineGap;
      lines = 2;
    } else {
      plan.name.width = width;
      plan.open.y = plan.controls.y = lineHeight + kLineGap;
      lines = 2;
      if (openWidth + kPartGap + controls <= width) {
        plan.controls.x = openWidth + kPartGap;
      } else {
        plan.controls.y = (2 * (lineHeight + kLineGap));
        lines = 3;
      }
    }
    plan.height = (lines * lineHeight) + ((lines - 1) * kLineGap);
    return plan;
  }

  void ColumnPreviewHeaderRow::place(Renderer& renderer, const Plan& plan) {
    // Out of the row, the step cannot keep the keyboard: the dispatcher drops the focus of a part that left the scene.
    const bool inRow = m_reset->parent() == m_controls;
    if (plan.showReset && !inRow) {
      m_controls->insertChildAt(m_resetIndex, std::move(m_resetHeld));
    } else if (!plan.showReset && inRow) {
      m_resetHeld = m_controls->removeChild(m_reset);
    }
    // A label's cut is decided when it is measured at the width it is given.
    LayoutConstraints nameConstraints;
    nameConstraints.setMaxWidth(plan.name.width);
    static_cast<Node&>(*m_name).measure(renderer, nameConstraints);
    const auto put = [&renderer](Node& node, const Place& at) {
      node.arrange(renderer, {at.x, at.y, at.width, at.height});
    };
    put(*m_name, plan.name);
    put(*m_controls, plan.controls);
    put(*m_open, plan.open);
  }

  LayoutSize ColumnPreviewHeaderRow::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
    if (constraints.hasMaxWidth) {
      return constraints.constrain({constraints.maxWidth, plan(renderer, constraints.maxWidth).height});
    }
    // No bound: everything in one line, the name with at least the room plan() keeps for it.
    const auto natural = [&renderer](Node& node) { return node.measure(renderer, LayoutConstraints::unconstrained()); };
    const float name = std::max(natural(*m_name).width, kNameRoomPerText * m_name->fontSize());
    const float line = name + controlsWidth(renderer, true) + natural(*m_open).width + (2 * kPartGap);
    return constraints.constrain({line, plan(renderer, line).height});
  }

  void ColumnPreviewHeaderRow::doLayout(Renderer& renderer) { place(renderer, plan(renderer, width())); }
} // namespace luciole
