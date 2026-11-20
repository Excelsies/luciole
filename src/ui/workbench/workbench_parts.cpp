#include "ui/workbench/workbench_parts.h"

#include "ui/workbench/instrument_view.h"
#include "ui/workbench/ribbon_view.h"
#include "ui/workbench/workbench_columns.h"
#include "ui/workbench/workbench_view.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace luciole {
  namespace {
    namespace wb = workbench;

    template <class Callback, class... Args> void fire(const Callback& callback, Args&&... args) {
      if (callback) {
        callback(std::forward<Args>(args)...);
      }
    }

    // The folders of `trail`, each a child of the one before it and the first a child of the snapshot's folder, as
    // nodes of the snapshot, up to the first one it does not hold as a folder.
    PartTrail trailIn(const PlacedSnapshot& placed, const std::vector<Location>& trail) {
      PartTrail mapped;
      if (!placed.snapshot) {
        return mapped;
      }
      const auto& tree = placed.snapshot->tree;
      wb::NodeId folder = tree.root;
      const Location* above = &placed.snapshot->location;
      // `trail` holds at most FolderTrail::kDepth folders.
      for (const auto& location : trail) {
        const auto name = location.basename();
        if (!name || location.parent() != *above) {
          break;
        }
        const auto children = wb::children(tree, folder);
        const auto found = std::ranges::find_if(children, [&](wb::NodeId id) {
          return wb::hasNode(tree, id) && tree.nodes[id].folder && tree.nodes[id].name == *name;
        });
        if (found == children.end()) {
          break;
        }
        folder = *found;
        mapped.folders.push_back(folder);
        above = &location;
      }
      return mapped;
    }
  } // namespace

  WorkbenchParts::WorkbenchParts(InstrumentView& instrument, RibbonView& ribbon, WorkbenchPartsCallbacks callbacks)
      : m_instrument(instrument), m_ribbon(ribbon), m_callbacks(std::move(callbacks)) {
    wire(m_instrument, WorkbenchPane::Instrument);
    wire(m_ribbon, WorkbenchPane::Ribbon);
  }

  void WorkbenchParts::wire(PartView& view, WorkbenchPane pane) {
    view.onHover = [this, &view, pane](wb::NodeId node) {
      fire(m_callbacks.hover, pane, m_kept.entryIn(view.shownSnapshot(), node));
    };
    view.onRequest = [this, &view, pane](const PartRequest& asked) { request(pane, view, asked); };
    view.onFrameNeeded = [this] { fire(m_callbacks.frameNeeded); };
  }

  void WorkbenchParts::setTrail(const FolderTrail* trail) {
    m_trail = trail;
    m_trailDirty = true;
  }

  void WorkbenchParts::setSnapshot(const PlacedSnapshot& placed) {
    m_placed = placed;
    m_kept.add(placed);
    m_inputs.snapshot = {};
    if (const auto& snapshot = placed.snapshot) {
      m_inputs.snapshot = {.tree = {snapshot, &snapshot->tree}, .serial = snapshot->serial};
    }
    m_trailDirty = true;
  }

  void WorkbenchParts::followTrail() {
    const auto serial = m_trail != nullptr ? m_trail->serial() : 0;
    if (!m_trailDirty && serial == m_seenTrail) {
      return;
    }
    m_trailDirty = false;
    m_seenTrail = serial;
    const auto& folder = m_placed.snapshot;
    m_inputs.trail = m_trail != nullptr && folder
        ? trailIn(m_placed, m_trail->trail(folder->location, FolderTrail::kDepth))
        : PartTrail{};
  }

  bool WorkbenchParts::push(const wb::ColorPalette& palette, bool resizing) {
    followTrail();
    m_inputs.palette = palette;
    const std::array before{m_instrument.shownSnapshot(), m_ribbon.shownSnapshot()};
    m_instrument.update(m_inputs, resizing);
    m_ribbon.update(m_inputs, resizing);
    const std::array inUse{
        m_instrument.shownSnapshot(),
        m_instrument.inputs().snapshot,
        m_ribbon.shownSnapshot(),
        m_ribbon.inputs().snapshot,
    };
    m_kept.keepFrom(inUse);
    const auto moved = [](const PartSnapshot& from, const PartSnapshot& to) {
      return from.tree != to.tree || from.serial != to.serial;
    };
    return moved(before[0], inUse[0]) || moved(before[1], inUse[2]);
  }

  void WorkbenchParts::mark(const std::optional<WorkbenchHover>& hovered, const std::optional<WorkbenchEntry>& cursor) {
    const auto markIn = [&](PartView& part, WorkbenchPane pane) {
      const auto& shown = part.shownSnapshot();
      const bool pointed = hovered && hovered->pane != pane;
      part.setPointed(pointed ? m_kept.nodeIn(shown, hovered->entry.location) : wb::kNoNode);
      part.setCursor(cursor ? m_kept.nodeIn(shown, cursor->location) : wb::kNoNode);
    };
    markIn(m_instrument, WorkbenchPane::Instrument);
    markIn(m_ribbon, WorkbenchPane::Ribbon);
  }

  void WorkbenchParts::request(WorkbenchPane pane, const PartView& view, const PartRequest& request) {
    auto entry = m_kept.entryIn(view.shownSnapshot(), request.node);
    switch (request.kind) {
    case PartRequestKind::Go:
    case PartRequestKind::Select:
    case PartRequestKind::Open: {
      if (!entry) {
        return;
      }
      const WorkbenchEntryRequest asked{
          .location = std::move(entry->location), .button = request.button, .modifiers = request.modifiers
      };
      const auto kind = request.kind;
      fire(
          kind == PartRequestKind::Go           ? m_callbacks.go
              : kind == PartRequestKind::Select ? m_callbacks.select
                                                : m_callbacks.activate,
          asked
      );
      return;
    }
    case PartRequestKind::Context: {
      float x = 0;
      float y = 0;
      Node::absolutePosition(&view, x, y);
      fire(
          m_callbacks.context,
          WorkbenchContextRequest{
              .pane = pane,
              .entry = entry ? std::optional(std::move(entry->location)) : std::nullopt,
              .sceneX = x + request.x,
              .sceneY = y + request.y,
          }
      );
      return;
    }
    case PartRequestKind::ShowMore:
      showMore(pane, view, request);
      return;
    }
  }

  void WorkbenchParts::showMore(WorkbenchPane pane, const PartView& view, const PartRequest& request) {
    const auto& shown = view.shownSnapshot();
    const auto folder = m_kept.entryIn(shown, request.node);
    // the flame graph and the flow diagram draw folders only, so their merged pieces hold folders only
    const bool foldersOnly = pane == WorkbenchPane::Ribbon && m_ribbon.kind() != WorkbenchRibbon::Ladder;
    auto merged = m_kept.mergedIn(
        shown, request.node, view.shapeView().shapes(), view.inputs().settings, foldersOnly, WorkbenchView::kMoreListed
    );
    if (!folder || merged.count == 0) {
      return;
    }
    // a click lists them at the pointer, a key at the piece: where the part placed it, else the middle of what it drew
    const bool placed = request.button != 0 || request.anchored;
    const auto anchor = placed ? std::nullopt : mergedAnchor(view.shapeView().shapes(), request.node);
    const auto at = anchor.value_or(wb::ShapePoint{.x = request.x, .y = request.y});
    float x = 0;
    float y = 0;
    Node::absolutePosition(&view.shapeView(), x, y);
    fire(
        m_callbacks.more,
        WorkbenchMoreRequest{
            .pane = pane,
            .folder = folder->location,
            .listed = std::move(merged.listed),
            .count = merged.count,
            .sceneX = x + at.x,
            .sceneY = y + at.y,
        }
    );
  }
} // namespace luciole
