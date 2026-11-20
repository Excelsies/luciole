#pragma once

#include "workbench/atlas_scene.h"
#include "workbench/color.h"
#include "workbench/flame_row.h"
#include "workbench/measure.h"
#include "workbench/scene_style.h"
#include "workbench/shape.h"

#include <utility>
#include <vector>

// The Atlas's labels with the shapes sized to their measured text; private to the scene shape builders.
namespace luciole::workbench {
  // Shapes drawn under labels, each with the area its labels are ranked by.
  using RankedShapes = std::vector<std::pair<double, Shape>>;

  // Labels one circle as labelPlan says: a title chip in the cap of a large folder (accent dot, name and measure),
  // a centered name of one or two lines with its icon and measure, on a pill when the circle's children are drawn,
  // or the count of a "+N more" bubble with "more" under it. Chips and pills go to `under`.
  void addAtlasLabel(
      scene_style::Builder& b, const LayoutSettings& settings, const SceneItem& item, ViewSize view,
      const NodeColor& color, const LabelFont& font, RankedShapes& under
  );

  // Puts the shapes of `under` whose labels keep their place among the builder's labels, at most kMaxSceneLabels.
  void putKeptLabelShapes(scene_style::Builder& b, const RankedShapes& under);
} // namespace luciole::workbench
