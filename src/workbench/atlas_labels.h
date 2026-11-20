#pragma once

#include "workbench/atlas_scene.h"
#include "workbench/pane_layout.h"

#include <cstdint>

// Where the Atlas labels its circles; text fitting is left to the view.
namespace luciole::workbench {
  enum class LabelKind : std::uint8_t { None, More, Center, Title, EmptyNote };

  // Where and how strongly an item is labeled; text fitting is left to the view.
  struct LabelPlan {
    LabelKind kind = LabelKind::None;
    double alpha = 0;
    double x = 0;           // Center: the visible part's center; Title: the circle's center
    double y = 0;           // Center: the visible part's center; Title: the chip's center line
    double r = 0;           // Center: the visible part's radius
    double fontSize = 0;    // name size
    double metricAlpha = 0; // Center: strength of the metric line, multiplied by alpha; 0 hides the line
    double room = 0;        // Title: width for the chip; EmptyNote: width for the note
    bool icon = false;
    bool backdrop = false; // a pill behind the label, over the children
    bool moreLine = false; // More: room for the "more" line under the count
  };

  // Length of the chord at distance dy from the center of a circle of radius r.
  double chord(double r, double dy);
  // Width for a centered label line at distance dy from the center.
  double labelWidth(double r, double dy);
  LabelPlan labelPlan(const SceneItem& item, ViewSize view);
} // namespace luciole::workbench
