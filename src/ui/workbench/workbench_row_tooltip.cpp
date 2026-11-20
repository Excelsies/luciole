#include "ui/workbench/workbench_row_tooltip.h"

#include "render/scene/input_area.h"
#include "ui/controls/virtual_grid_view.h"

namespace luciole {
  void anchorRowTooltip(InputArea& area, const VirtualGridView& grid, std::optional<std::size_t> row) {
    const auto rect = row ? grid.itemRect(*row) : std::nullopt;
    if (!rect) {
      area.clearTooltipAnchorInsets();
      return;
    }
    float x = 0;
    float y = 0;
    Node::absolutePosition(&area, x, y);
    area.setTooltipAnchorInsets({
        .top = rect->y - y,
        .right = x + area.width() - rect->x - rect->width,
        .bottom = y + area.height() - rect->y - rect->height,
        .left = rect->x - x,
    });
  }
} // namespace luciole
