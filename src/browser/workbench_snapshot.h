#pragma once

#include "browser/measured_tree.h"
#include "config/visit_store.h"
#include "fs/file_error.h"
#include "fs/location.h"
#include "workbench/jump_rank.h"
#include "workbench/measure.h"
#include "workbench/tree.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luciole {
  // An entry of the snapshot's folder with its share among its visible siblings.
  struct WorkbenchShare {
    std::string name; // raw bytes
    workbench::NodeId id = workbench::kNoNode;
    double share = 0;
  };

  // One immutable picture of a Workbench page's folder that every pane draws from.
  struct WorkbenchSnapshot {
    std::uint64_t serial = 0; // grows with every published change
    Location location;
    workbench::LayoutSettings settings;
    workbench::Tree tree;
    workbench::JumpList jump;
    std::vector<Location> jumpLocations; // the folder of each Jump ahead row, aligned with jump.rows
    std::vector<WorkbenchShare> shares;  // the folder's entries, ordered by name bytes
    double total = 0;                    // the weight of the folder's visible entries, remainders included
    std::optional<FileError> error;      // the folder could not be measured
    bool notMeasured = false;            // the location is not a local folder: nothing is measured, no walk runs
    // The folder's children its table folded into a remainder, by name and size: the Size cells of rows the tree
    // does not name. Null when none were.
    std::shared_ptr<const FoldedChildren> folded = nullptr;

    // The share of an entry of the folder's listing: its node's share when the tree names it, otherwise the
    // weight of the listing's own values, with no visits, over the same total, as for an entry folded into a
    // remainder. 0 for a hidden entry while hidden entries are not shown.
    double share(const ListedEntry& entry) const;
    // The node of an entry of the folder; kNoNode for a name the tree does not hold.
    workbench::NodeId node(std::string_view name) const;
    // The Size cell of a folder of the listing, from this measure: a hard-linked file counts once, as in the
    // instrument. Empty until the walk counted something, and for a folder that is not measured, was never entered,
    // is on another filesystem or could not be read; "≥ " first while the walk runs or was cut short.
    std::string folderSizeText(std::string_view name) const;
  };

  // A child of the folder measured again on its own; it replaces what the folder's table says of the child, or is
  // added when the tree does not name it.
  struct WorkbenchRefresh {
    std::string name;
    std::shared_ptr<const MeasuredTree> measured; // the child with two levels below it; null when its walk failed
    std::optional<FileError> error;               // why its walk failed: Missing removes the child
    std::shared_ptr<const MeasureTable> from;     // the child's table `measured` was built from
  };

  // The trees of one snapshot and what the page asked for; finishSnapshot completes it on a worker.
  struct WorkbenchDraft {
    Location location;
    std::vector<Location> ancestors; // nearest first, as the measured tree's input listed them
    std::shared_ptr<const MeasuredTree> measured;
    std::vector<WorkbenchRefresh> refreshed;
    std::shared_ptr<const std::vector<VisitRecord>> visits; // ordered by URI; null for none
    workbench::LayoutSettings settings;
    std::optional<FileError> error;
    bool carry = false; // folders not entered yet keep the size `previous` showed for them
  };

  // A child of the folder measured again on its own, as the main context hands it to a build.
  struct WorkbenchChildTable {
    std::string name;
    Location location;
    std::shared_ptr<const MeasureTable> table;
    std::shared_ptr<const MeasuredTree> built; // its levels from an earlier build of the same table; null to build
  };

  // What the main context hands a build: shared pointers and small values, so handing it over copies no tree.
  struct WorkbenchBuildInput {
    std::shared_ptr<const MeasureView> view;
    Location location;
    std::shared_ptr<const std::vector<MeasuredLevel>> ancestors; // nearest first, with their listings; null for none
    std::vector<WorkbenchChildTable> children;
    std::optional<FileError> error; // the folder's walk failed
    bool folderFinished = false;    // the folder's walk has ended
    bool carry = false;             // the second pass runs: folders not entered yet keep the size shown before
    double now = 0;
  };

  // The folder's tree and each refreshed child's levels, built from the view. Empty until the folder's own walk, or a
  // cached ancestor's table, lists its entries, its walk ends or it fails. Processor work only, for a worker.
  std::optional<WorkbenchDraft> buildDraft(const WorkbenchBuildInput& input);

  // Whether a walk of `location` can measure anything: a local folder that is not the Trash.
  bool measurable(const Location& location);

  // The snapshot of a location that is not measured: a tree of one folder named as the location is, with no totals,
  // and no Jump ahead rows. `serial` numbers it.
  std::shared_ptr<const WorkbenchSnapshot>
  unmeasuredSnapshot(const Location& location, const workbench::LayoutSettings& settings, std::uint64_t serial);

  // The snapshot of a folder whose build failed: no tree, only the error; numbered one past `previous`.
  std::shared_ptr<const WorkbenchSnapshot> failedSnapshot(
      const std::shared_ptr<const WorkbenchSnapshot>& previous, const Location& location,
      const workbench::LayoutSettings& settings, const FileError& error
  );

  // Grafts the refreshed children into one copy of the tree and adjusts the folder's totals, sets visit counts,
  // ranks Jump ahead and computes the shares. Null when the result equals `previous` in everything but the tree's
  // time; otherwise numbered one past `previous`. Processor work only, for a worker.
  std::shared_ptr<const WorkbenchSnapshot>
  finishSnapshot(const WorkbenchDraft& draft, const std::shared_ptr<const WorkbenchSnapshot>& previous);
} // namespace luciole
