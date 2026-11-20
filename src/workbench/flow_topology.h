#pragma once

#include "workbench/measure.h"
#include "workbench/tree.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// The records of the flow diagram: the current folder, the next three levels of folders as columns, the folders
// folded into More records, and one Files record per column.
namespace luciole::workbench {
  inline constexpr std::size_t kNoRecord = std::numeric_limits<std::size_t>::max();
  inline constexpr std::size_t kFlowColumns = 4;

  enum class FlowRecordKind : std::uint8_t {
    Current, // the current folder, column 0
    Folder,  // one folder
    More,    // the folders of one parent that are not drawn
    Files,   // every file of the column's parents, as one quiet bar with no parent and no ribbon
  };

  // Why column 1 is empty: the folder could not be read, its entries were not expanded in this snapshot or it lies
  // on another filesystem, it has no entries, or no visible folders among them.
  enum class FlowEmpty : std::uint8_t { None, Unreadable, NotExpanded, Empty, NoFolders };

  // Summed values a record stands for; modified is the newest. `state` is the most doubtful state of the summed
  // folders.
  struct FlowTotals {
    double bytes = 0;
    double files = 0;
    double items = 0;
    double modified = 0;
    double visits = 0;
    MeasureState state = MeasureState::Done;
  };

  struct FlowRecord {
    FlowRecordKind kind = FlowRecordKind::Folder;
    std::size_t column = 0;
    // Current and Folder: the folder. More: the parent folder whose entries it holds. Files: the current folder.
    // With kind (and column for Files) this identifies a record across layouts.
    NodeId node = kNoNode;
    std::size_t parent = kNoRecord; // record index; none for Current and Files
    std::vector<std::size_t> kids;  // record indexes in the next column
    std::vector<NodeId> folders;    // More: the folders it stands for, heaviest first
    std::size_t count = 0;          // More: folders, Files: files
    double weight = 0;
    FlowTotals totals;
    bool collapsed = false; // More: a whole group folded, labeled "N folders" instead of "+N more"
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    double sliceTop = 0; // where the ribbon meets the parent bar
    double sliceBottom = 0;
  };

  struct FlowTopology {
    NodeId folder = kNoNode;
    std::vector<FlowRecord> records; // column by column; record 0 is the current folder, none for an unknown id
    std::array<std::vector<std::size_t>, kFlowColumns> columns;
    std::size_t spent = 0;  // child entries read
    bool truncated = false; // a parent was skipped to stay within the budget
    FlowEmpty empty = FlowEmpty::None;
  };

  struct FlowOptions {
    std::size_t budget = 60000; // child entries read in total; column 1 is always read, later parents are skipped
                                // once this is reached
    std::size_t perColumn = 12; // records in a later column, files included
    std::size_t firstMax = 8;   // folders drawn in column 1
  };

  // Columns for `folder`: its heaviest subfolders, then three and then two per parent, the lightest groups folded
  // into More records first. Only drawn folders are read, heaviest first, within the budget.
  FlowTopology
  buildFlowTopology(const Tree& tree, NodeId folder, const LayoutSettings& settings, const FlowOptions& options = {});

  // The record that stands for `node`: its own, the nearest drawn ancestor, or the column 1 More record holding it.
  std::optional<std::size_t> flowRecordFor(const Tree& tree, const FlowTopology& topology, NodeId node);

  // Per record: true on the way back to the current folder and everywhere downstream of `record`.
  std::vector<bool> flowFocus(const FlowTopology& topology, std::size_t record);

  // "+N more", "N folders" for a folded group, "files", or the folder name ("/" when it has none).
  std::string flowRecordName(const Tree& tree, const FlowTopology& topology, std::size_t record);

  // The measure text of a record, from its totals for More and Files.
  std::string
  flowRecordValue(const Tree& tree, const FlowTopology& topology, std::size_t record, WorkbenchMeasure measure);
} // namespace luciole::workbench
