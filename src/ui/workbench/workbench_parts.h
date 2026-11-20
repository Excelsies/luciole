#pragma once

#include "browser/workbench_state.h"
#include "ui/workbench/kept_snapshots.h"
#include "ui/workbench/scene_part.h"
#include "ui/workbench/snapshot_places.h"
#include "workbench/color.h"
#include "workbench/measure.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace luciole {
  class FolderTrail;
  class InstrumentView;
  class PartView;
  class RibbonView;
  struct WorkbenchContextRequest;
  struct WorkbenchEntryRequest;
  struct WorkbenchMoreRequest;

  // What the instrument and the ribbon ask of their owner.
  struct WorkbenchPartsCallbacks {
    // A part points at an entry, or at none.
    std::function<void(WorkbenchPane, const std::optional<WorkbenchEntry>&)> hover;
    std::function<void(const WorkbenchEntryRequest&)> go;       // enter a folder
    std::function<void(const WorkbenchEntryRequest&)> select;   // put the list's cursor on an entry
    std::function<void(const WorkbenchEntryRequest&)> activate; // open a file
    std::function<void(const WorkbenchContextRequest&)> context;
    std::function<void(const WorkbenchMoreRequest&)> more;
    std::function<void()> frameNeeded;
  };

  // The instrument and the ribbon of a Workbench view. Both draw the current folder's snapshot with the layout
  // settings, the palette and the page's folder trail, which is mapped to the snapshot's nodes when either changes.
  // It keeps the snapshots their scenes still name, marks in them what other panes point at, and turns what their
  // pointer and keys ask for into entries.
  class WorkbenchParts {
  public:
    WorkbenchParts(InstrumentView& instrument, RibbonView& ribbon, WorkbenchPartsCallbacks callbacks);

    // Borrowed until replaced; null for none.
    void setTrail(const FolderTrail* trail);
    // The current folder's snapshot; empty while it loads.
    void setSnapshot(const PlacedSnapshot& placed);
    void setSettings(const workbench::LayoutSettings& settings) { m_inputs.settings = settings; }
    const workbench::LayoutSettings& settings() const noexcept { return m_inputs.settings; }

    // Gives both parts their inputs; true when one of them shows another snapshot afterwards.
    bool push(const workbench::ColorPalette& palette, bool resizing);
    // Points each part at the entry another pane hovers and at the list's cursor, in the nodes of the snapshot it
    // shows.
    void mark(const std::optional<WorkbenchHover>& hovered, const std::optional<WorkbenchEntry>& cursor);

  private:
    void wire(PartView& view, WorkbenchPane pane);
    void followTrail();
    void request(WorkbenchPane pane, const PartView& view, const PartRequest& request);
    void showMore(WorkbenchPane pane, const PartView& view, const PartRequest& request);

    InstrumentView& m_instrument;
    RibbonView& m_ribbon;
    WorkbenchPartsCallbacks m_callbacks;
    KeptSnapshots m_kept; // from the oldest a part shows or is given to the current one
    PlacedSnapshot m_placed;
    PartInputs m_inputs;
    const FolderTrail* m_trail = nullptr;
    std::uint64_t m_seenTrail = 0;
    bool m_trailDirty = true;
  };
} // namespace luciole
