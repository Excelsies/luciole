#pragma once

#include <cstddef>

namespace luciole {
  // The height of a table row, and of the edit bar above the grid, in text sizes.
  inline constexpr float kTableRowEm = 1.8F;

  // How far the cells of a grid scroll along each axis, in a viewport of `viewW` by `viewH` for content of `contentW`
  // by `contentH`: each bar lies over the cells, so the content reaches one bar width past its end on the axis of the
  // other bar while that bar shows. A bar shows only where the reach passes the viewport by more than half a pixel,
  // as the scrollbar itself decides.
  struct TableReach {
    float x = 0;
    float y = 0;
  };

  inline TableReach tableReach(float contentW, float contentH, float viewW, float viewH, float bar) {
    constexpr float kSlop = 0.5F;
    bool vertical = contentH > viewH + kSlop;
    bool horizontal = contentW > viewW + kSlop;
    if (vertical && !horizontal) {
      horizontal = contentW + bar > viewW + kSlop;
    } else if (horizontal && !vertical) {
      vertical = contentH + bar > viewH + kSlop;
    }
    return {.x = contentW + (vertical ? bar : 0.0F), .y = contentH + (horizontal ? bar : 0.0F)};
  }

  // A cell of a table by row and column, in the grid or in the source as the context says.
  struct TableCell {
    std::size_t row = 0;
    std::size_t column = 0;
    bool operator==(const TableCell&) const = default;
  };
} // namespace luciole
