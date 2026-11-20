#pragma once

#include "workbench/flow_stem.h"
#include "workbench/flow_topology.h"
#include "workbench/measure.h"
#include "workbench/tree.h"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

// The flow diagram: the current folder as one bar, the next three levels of folders as columns of bars, ribbons
// from each bar to its parent, a label per bar, and the ancestors as a stem of pills on the left.
namespace luciole::workbench {
  // Size limits of the diagram's pane. A taller host leaves the space above the diagram empty.
  struct FlowLimits {
    double minWidth = 560;
    double minHeight = 180;
    double preferredHeight = 280;
    double maxHeight = 420;
  };
  inline constexpr FlowLimits kFlowLimits{};

  // A thin line from one stem pill to the next, or from the parent pill to the current bar.
  struct FlowLink {
    double x0 = 0;
    double x1 = 0;
    double centerY = 0;
  };

  struct FlowPoint {
    double x = 0;
    double y = 0;
  };

  // A band from a slice of the parent bar (x0) to the child bar (x1), drawn as two cubic curves joined by straight
  // lines at both ends.
  struct FlowRibbon {
    std::size_t record = kNoRecord;
    double x0 = 0;
    double x1 = 0;
    double parentTop = 0;
    double parentBottom = 0;
    double childTop = 0;
    double childBottom = 0;
    std::array<FlowPoint, 4> top;    // from (x0, parentTop) to (x1, childTop)
    std::array<FlowPoint, 4> bottom; // from (x1, childBottom) to (x0, parentBottom)
    bool muted = false;              // the ribbon of a More record
  };

  struct FlowLabelBox {
    double centerY = 0;
    double height = 0;
  };

  // The box of one record's label, left aligned at x. Two lines put the value under the name.
  struct FlowLabel {
    std::size_t record = kNoRecord;
    double x = 0;
    double centerY = 0;
    double height = 0;
    std::size_t lines = 1;
    double maxWidth = 0;
  };

  struct FlowLayout {
    double width = 0;
    double height = 0;
    FlowTopology topology; // records carry their bars
    std::array<double, kFlowColumns> columnX{};
    std::vector<FlowRibbon> ribbons;
    std::vector<FlowLabel> labels;
    std::vector<FlowPill> stem;
    std::vector<FlowLink> links;
  };

  // Records a column of a band this tall can hold, 3 to 12. A non-finite height counts as 0.
  std::size_t flowCapacity(double height);

  // Records per column and folders in column 1 for a band this tall.
  FlowOptions flowOptions(double height);

  // Centers of label boxes moved apart in order, 1 px between boxes, inside lo..hi when they fit.
  std::vector<double> flowSpreadLabels(std::span<const FlowLabelBox> boxes, double lo, double hi);

  // Bars, ribbons and labels of a topology in a width by height box, with `stemWidth` kept free on the left.
  // Empty for a width or height that is not a positive finite number, a negative or non-finite stem width, or a
  // topology that does not hold together: record 0 alone in column 0 as the current folder, each column listing
  // existing records of that column, and each bar but Files having a parent one column to its left.
  FlowLayout placeFlow(FlowTopology topology, double width, double height, double stemWidth);

  // The whole diagram for Tree::root, records per column from the height, Tree::ancestors as the stem. Empty for
  // a size that is not a positive finite number or an unknown root.
  FlowLayout flowLayout(const Tree& tree, const LayoutSettings& settings, double width, double height);
} // namespace luciole::workbench
