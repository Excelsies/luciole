#pragma once

#include "model/directory_options.h"
#include "ui/workbench/part_view.h"
#include "workbench/pane_layout.h"

#include <cstdint>
#include <functional>

namespace luciole {
  // The Workbench ribbon above the list: the flame graph, the flow diagram or the ladder, drawn against the view's
  // bottom edge when the view is taller than the kind's tallest scene.
  class RibbonView final : public PartView {
  public:
    // `modifiers` reports the keyboard modifiers held.
    RibbonView(WorkbenchRibbon kind, std::function<std::uint32_t()> modifiers);

    // Another kind tweens from the shown scene at the next update.
    void setKind(WorkbenchRibbon kind);
    WorkbenchRibbon kind() const noexcept { return m_kind; }
    // The body sizes of the kind for the current snapshot; the ladder's grow with the folder's depth.
    workbench::RibbonLimits limits() const;

  private:
    WorkbenchRibbon m_kind;
  };
} // namespace luciole
