#pragma once

#include "workbench/color.h"
#include "workbench/measure.h"
#include "workbench/nest_scene.h"
#include "workbench/tree.h"
#include "workbench/treemap.h"

#include <cstddef>
#include <span>
#include <vector>

// The Nest frame: one ring per ancestor around the core, the farthest ones sharing one line.
namespace luciole::workbench {
  struct NestRingPlan {
    std::size_t level = 0; // 1 is the parent's ring; kNestLineLevel for the shared line
    double thickness = 0;
    std::size_t from = 0; // the line: index of the first ancestor it stands for
  };

  // Rings outermost first for `depth` ancestors; ancestors beyond the profile's rings share one line.
  std::vector<NestRingPlan> nestRingPlan(std::size_t depth, const NestProfile& profile);

  // Outer rectangle and centerline corner radius of each ring, and the core left inside them.
  struct NestFrame {
    std::vector<TreemapRect> rects; // aligned with the plan
    std::vector<double> radii;      // aligned with the plan
    TreemapRect core;
  };

  NestFrame nestFrame(std::span<const NestRingPlan> plan, double width, double height, const NestProfile& profile);

  // Appends a ring with its segments to `scene` for each plan entry that has a frame rectangle, a band with area and
  // the ancestors it names. `ancestors` is nearest first; ring 1 leads to scene.folder.
  void addNestRings(
      const Tree& tree, const LayoutSettings& settings, const ColorPalette& palette, std::span<const NodeId> ancestors,
      std::span<const NestRingPlan> plan, const NestFrame& frame, NestScene& scene
  );
} // namespace luciole::workbench
