#include "ui/workbench/ribbon_view.h"

#include "ui/workbench/flame_part.h"
#include "ui/workbench/flow_part.h"
#include "ui/workbench/ladder_part.h"
#include "workbench/flame_bands.h"
#include "workbench/flow_layout.h"
#include "workbench/ladder_plan.h"

#include <utility>

namespace luciole {
  RibbonView::RibbonView(WorkbenchRibbon kind, std::function<std::uint32_t()> modifiers)
      : PartView(std::move(modifiers)), m_kind(kind) {
    setKind(kind);
  }

  void RibbonView::setKind(WorkbenchRibbon kind) {
    m_kind = kind;
    switch (kind) {
    case WorkbenchRibbon::Flow:
      setPart(makeFlowPart());
      return;
    case WorkbenchRibbon::Ladder:
      setPart(makeLadderPart());
      return;
    case WorkbenchRibbon::Flame:
      break;
    }
    setPart(makeFlamePart());
  }

  workbench::RibbonLimits RibbonView::limits() const {
    switch (m_kind) {
    case WorkbenchRibbon::Flow: {
      const auto& flow = workbench::kFlowLimits;
      return {
          .minWidth = flow.minWidth,
          .minHeight = flow.minHeight,
          .preferredHeight = flow.preferredHeight,
          .maxHeight = flow.maxHeight,
      };
    }
    case WorkbenchRibbon::Ladder: {
      const auto& tree = inputs().snapshot.tree;
      const auto ladder = workbench::ladderLimits(tree == nullptr ? 0 : tree->ancestors.size());
      return {
          .minWidth = ladder.minW, .minHeight = ladder.minH, .preferredHeight = ladder.prefH, .maxHeight = ladder.maxH
      };
    }
    case WorkbenchRibbon::Flame:
      break;
    }
    const auto& flame = workbench::kFlameLimits;
    return {.minWidth = flame.minW, .minHeight = flame.minH, .preferredHeight = flame.prefH, .maxHeight = flame.maxH};
  }
} // namespace luciole
