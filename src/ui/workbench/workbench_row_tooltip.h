#pragma once

#include <cstddef>
#include <optional>

class InputArea;
class VirtualGridView;

namespace luciole {
  // Anchors the tooltip `area` gives to row `row` of `grid` as the grid shows it, clipped to its viewport, so the
  // tooltip sits by that row however far the area reaches. None, or a row out of view, anchors it to the whole area.
  void anchorRowTooltip(InputArea& area, const VirtualGridView& grid, std::optional<std::size_t> row);
} // namespace luciole
