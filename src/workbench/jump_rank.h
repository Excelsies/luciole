#pragma once

#include "workbench/format.h"
#include "workbench/measure.h"
#include "workbench/tree.h"

#include <cstddef>
#include <string>
#include <vector>

// Jump ahead: folders at least two levels below the current folder, worth jumping to.
namespace luciole::workbench {
  inline constexpr std::size_t kJumpRows = 24;
  inline constexpr std::size_t kJumpMinLevel = 2;   // children of the current folder are level 1
  inline constexpr std::size_t kJumpPathChars = 38; // code points of a row's path
  inline constexpr std::size_t kJumpNodeBudget = 200'000;

  struct JumpRow {
    NodeId id = kNoNode;
    std::string path;  // names below the current folder joined by '/'
    PathSplit split;   // the path fitted to kJumpPathChars, leaf kept
    std::string label; // the folder's value in the measure entries are sized by
    double share = 0;  // its weight over its siblings'
    bool partial = false;
    std::string notCounted; // why the label reads "at least"; empty unless it does
  };

  struct JumpList {
    std::vector<JumpRow> rows;
    bool partial = false; // a folder read is still being measured, so the ranking may change
    bool cut = false;     // the node budget ended the walk before every node was read
  };

  struct JumpOptions {
    std::size_t count = kJumpRows;
    std::size_t nodeBudget = kJumpNodeBudget; // nodes read below the current folder, nearer levels first
  };

  // Visible folders at level kJumpMinLevel or deeper, most visits first, then most bytes, then path in byte order.
  // A hidden folder and everything below it are left out unless hidden entries are shown; files and nodes without a
  // name (folded remainders) are never rows. Values that are not finite or are negative read as 0. A partial folder
  // ranks by what is measured so far.
  JumpList jumpAhead(const Tree& tree, const LayoutSettings& settings, const JumpOptions& options = {});
} // namespace luciole::workbench
