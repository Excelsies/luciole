#pragma once

#include "model/directory_options.h"
#include "ui/workbench/part_view.h"
#include "workbench/pane_layout.h"

#include <cstdint>
#include <functional>

namespace luciole {
  // The Workbench instrument: Nest, Iris or Atlas drawn in one shape view. Its wheel does what the kind's own view
  // does: Nest and Iris step into the folder under the pointer or up one level, Atlas zooms about the pointer.
  class InstrumentView final : public PartView {
  public:
    // `modifiers` reports the keyboard modifiers held.
    InstrumentView(WorkbenchInstrument kind, std::function<std::uint32_t()> modifiers);

    // Another kind tweens from the shown scene at the next update.
    void setKind(WorkbenchInstrument kind);
    WorkbenchInstrument kind() const noexcept { return m_kind; }
    // The body size the instrument needs.
    static workbench::InstrumentLimits limits();

  private:
    WorkbenchInstrument m_kind;
  };
} // namespace luciole
