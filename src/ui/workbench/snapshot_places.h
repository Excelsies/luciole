#pragma once

#include "browser/workbench_snapshot.h"
#include "browser/workbench_state.h"
#include "fs/location.h"
#include "workbench/tree.h"

#include <memory>
#include <optional>
#include <vector>

namespace luciole {
  // A Workbench snapshot with the locations of the folders its tree holds above the current folder, so nodes of the
  // tree and entries the page points at can be named for each other.
  struct PlacedSnapshot {
    std::shared_ptr<const WorkbenchSnapshot> snapshot;
    std::vector<Location> ancestors; // nearest first, one per node of tree.ancestors while the location has parents
  };

  // Empty for a null snapshot.
  PlacedSnapshot placeSnapshot(std::shared_ptr<const WorkbenchSnapshot> snapshot);

  // Where a node of the tree lives; none for an unknown node, a folded remainder, or a node above the ancestors the
  // location reaches.
  std::optional<WorkbenchEntry> entryOf(const PlacedSnapshot& placed, workbench::NodeId node);

  // The node at `location`; kNoNode when the tree does not hold it.
  workbench::NodeId nodeOf(const PlacedSnapshot& placed, const Location& location);

  // The folder of the Jump ahead row for `node`; none when no row of the snapshot stands for it.
  std::optional<Location> jumpLocation(const PlacedSnapshot& placed, workbench::NodeId node);

  // The entry of `folder` that is `location` or holds it; none when `location` is not below `folder`.
  std::optional<Location> entryWithin(const Location& folder, const Location& location);
} // namespace luciole
