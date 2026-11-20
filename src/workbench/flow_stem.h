#pragma once

#include "workbench/tree.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// The ancestors of the current folder as a row of pills, root first.
namespace luciole::workbench {
  // Ancestors the stem shows at most, nearest first; deeper than any path a filesystem allows.
  inline constexpr std::size_t kMaxFlowStemLevels = 2048;

  enum class FlowPillKind : std::uint8_t { Pill, Dot };

  // One ancestor in the stem, root first.
  struct FlowPill {
    std::size_t level = 0; // 1 is the parent
    NodeId node = kNoNode;
    FlowPillKind kind = FlowPillKind::Pill;
    bool labeled = false;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    double minWidth = 0;
    double alpha = 1;
    double textWidth = 0; // room for the name; no name when 0 or less
  };

  struct FlowStemPlan {
    std::vector<FlowPill> pills; // root first, x from 0
    double width = 0;
    double gap = 0;
  };

  // Pills for the nearest `depth` ancestors, at most kMaxFlowStemLevels, farthest first; the three nearest keep their
  // minimum widths. The stem fits `width` only while every farther pill can shrink to 1 px with 1 px gaps (26
  // ancestors at 150 px); past that it is wider.
  FlowStemPlan flowStemPlan(std::size_t depth, double width);
} // namespace luciole::workbench
