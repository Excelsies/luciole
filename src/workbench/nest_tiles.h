#pragma once

#include "workbench/nest_scene.h"
#include "workbench/treemap.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

// The treemap of a folder's entries in the Nest core and the card.
namespace luciole::workbench {
  // Tiles for the first entries plus one "+N more" tile for the rest; `more` stands for weights[tiles.size()..].
  struct NestTileLayout {
    std::vector<TreemapRect> tiles;
    std::optional<TreemapRect> more;
  };

  // Shares of the area for `weights` (first `floors.size()` of them paired with `floors`, the rest with 0): every entry
  // gets at least its floor and the rest keep their proportions; the shares add up to 1. Empty when the floors alone
  // fill the area, a floor is negative or not finite, or a weight is not positive and finite. Ends within
  // weights.size() + 1 passes.
  std::vector<double> floorShares(std::span<const double> weights, std::span<const double> floors);

  // The first weights.size() weights laid out in `rect` with every tile at least `min` big, else empty. `names[i]` is
  // the room the name of entry i needs (0 for none, and for the entries past names.size()): its tile, inset by 1 px,
  // must leave that much. Small entries get the least area and the larger ones share the rest in proportion; the rows
  // are squarified for tiles as wide as the minimum is.
  std::vector<TreemapRect>
  minimumTiles(std::span<const double> weights, TreemapRect rect, NestSize min, std::span<const double> names = {});

  // Lays out the heaviest-first `weights`: the most entries (at most maxShown) for which they and a "+N more" tile
  // all meet `min` and show their names (see minimumTiles), the rest folded. An entry is folded only when its tile
  // could not meet them, so the lightest go first. When not even one entry fits beside "+N more", the heaviest goes
  // above a "+N more" strip, or "+N more" covers the rectangle alone. Empty when the rectangle is not finite or
  // under `min`.
  NestTileLayout layoutNestTiles(
      std::span<const double> weights, TreemapRect rect, NestSize min, std::size_t maxShown,
      std::span<const double> names = {}
  );
} // namespace luciole::workbench
