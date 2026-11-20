#pragma once

#include <cstddef>

namespace luciole {
  // The height of a table row, and of the edit bar above the grid, in text sizes.
  inline constexpr float kTableRowEm = 1.8F;

  // A cell of a table by row and column, in the grid or in the source as the context says.
  struct TableCell {
    std::size_t row = 0;
    std::size_t column = 0;
    bool operator==(const TableCell&) const = default;
  };
} // namespace luciole
