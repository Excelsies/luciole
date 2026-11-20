#pragma once

#include "workbench/nest_scene.h"
#include "workbench/treemap.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

// Where the Nest's keyboard cursor can rest and where an arrow key takes it.
namespace luciole::workbench {
  // Piece indexes of the cursor's stops: the core tiles in piece order, then the "+N more" tile.
  std::vector<std::size_t> nestStops(const NestScene& scene);

  // The box nearest to boxes[*from] in direction (dx, dy), favoring boxes straight ahead over those to the side. The
  // first box when `from` is none or out of range; `from` itself when no box lies ahead; none for no boxes.
  std::optional<std::size_t>
  nestNearest(std::span<const TreemapRect> boxes, std::optional<std::size_t> from, double dx, double dy);
} // namespace luciole::workbench
