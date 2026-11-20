#pragma once

#include "workbench/flame_bands.h"
#include "workbench/flame_row.h"
#include "workbench/flame_strata.h"
#include "workbench/measure.h"
#include "workbench/point.h"
#include "workbench/tree.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

// The flame graph as drawn: rows of items with their fills and labels decided, and what lies under a point.
namespace luciole::workbench {
  // Room between a name and the value drawn at the right end of its box.
  inline constexpr double kFlameValueGap = 8;

  struct FlameBox {
    double x = 0;
    double w = 0;
    double r = 0; // corner radius
  };

  struct FlameSceneRow {
    FlameRow row;
    std::vector<FlameItem> items; // sorted by x
  };

  struct FlameScene {
    double x0 = 0;
    double x1 = 0;
    FlameBands bands;
    std::vector<FlameSceneRow> rows; // aligned with bands.rows
    bool empty = false;              // no folder in the first stratum ahead
    bool truncated = false;
    std::optional<Rect> aheadBox; // the span of the strata ahead; none when the band has no row for them
  };

  struct FlameHit {
    std::size_t row = 0;
    std::size_t item = 0;
  };

  // The item of a row sorted by x that contains x. NaN finds none.
  std::optional<std::size_t> flameItemAt(std::span<const FlameItem> items, double x);

  // The child of `cwd` that `node` lies under, or kNoNode when node is not below cwd.
  NodeId flameFirstLevel(const Tree& tree, NodeId node, NodeId cwd);

  // Drawn box of a piece: whole-pixel edges, a 1 px gap to the next one from 6 px wide (none on the bar).
  FlameBox flameBox(double x, double w, double h, bool bar);

  // The flame graph of the tree's current folder in a band `width` by `bandHeight` px, with labels decided.
  FlameScene flameScene(
      const Tree& tree, const LayoutSettings& settings, double width, double bandHeight, const FlameFonts& fonts
  );

  std::optional<FlameHit> flameHitAt(const FlameScene& scene, double x, double y);
} // namespace luciole::workbench
