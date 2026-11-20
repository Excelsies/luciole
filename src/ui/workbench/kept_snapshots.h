#pragma once

#include "browser/workbench_state.h"
#include "fs/location.h"
#include "ui/workbench/scene_part.h"
#include "ui/workbench/snapshot_places.h"
#include "workbench/measure.h"
#include "workbench/shape.h"
#include "workbench/tree.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace luciole {
  // An entry a merged piece ("+N more") stands for, as a list of what it holds names it.
  struct MergedEntry {
    WorkbenchEntry entry;
    std::string name;
    std::string value; // in the measure entries are sized by
  };

  struct MergedEntries {
    std::vector<MergedEntry> listed; // heaviest first
    std::size_t count = 0;           // every entry the piece stands for, listed or not, folded ones included
  };

  // The middle of the box around the hittable shapes of the merged piece of `folder` in `drawn`; none when no such
  // shape has a valid geometry.
  std::optional<workbench::ShapePoint> mergedAnchor(std::span<const workbench::Shape> drawn, workbench::NodeId folder);

  // The placed snapshots the Workbench parts may still show while a newer one lays out, oldest first, so the nodes
  // of the scene each part shows and the entries the page points at can be named for each other.
  class KeptSnapshots {
  public:
    static constexpr std::size_t kMax = 8;

    // Keeps `placed` as the newest; the oldest goes beyond kMax. An empty snapshot is not kept.
    void add(const PlacedSnapshot& placed);
    // Lets go of every snapshot kept before the first one `inUse` names, and of all of them when it names none.
    void keepFrom(std::span<const PartSnapshot> inUse);

    // The entry a node of `shown` stands for; none when that snapshot is not kept.
    std::optional<WorkbenchEntry> entryIn(const PartSnapshot& shown, workbench::NodeId node) const;
    // The node of `shown` at `location`; kNoNode when that snapshot is not kept or does not hold it.
    workbench::NodeId nodeIn(const PartSnapshot& shown, const Location& location) const;
    // What a merged piece of `folder` holds in the scene `drawn` from `shown`: the folder's visible entries, folders
    // only when `foldersOnly`, that no shape stands for, at most `limit` of them listed. The entries a remainder node
    // folds are counted and never listed. Empty for a snapshot that is not kept and for a node that is no folder.
    MergedEntries mergedIn(
        const PartSnapshot& shown, workbench::NodeId folder, std::span<const workbench::Shape> drawn,
        const workbench::LayoutSettings& settings, bool foldersOnly, std::size_t limit
    ) const;

  private:
    const PlacedSnapshot* find(const PartSnapshot& shown) const;

    std::vector<PlacedSnapshot> m_kept;
  };
} // namespace luciole
