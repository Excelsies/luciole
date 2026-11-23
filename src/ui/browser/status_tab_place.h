#pragma once

#include "render/scene/node.h"
#include "ui/style.h"

#include <algorithm>

namespace luciole {
  // How much of an area the status tab may use: the area less its margin on one side and the room on the other for
  // the vertical scrollbar, and that room.
  struct StatusTabRoom {
    float maxWidth = 1.0F;
    float trailing = 0.0F;
  };
  inline StatusTabRoom statusTabRoom(float areaWidth) {
    const float inset = Style::spaceSm;
    const float trailing = inset + Style::scrollbarWidth + Style::scrollbarGap;
    return {.maxWidth = std::max(1.0F, areaWidth - inset - trailing), .trailing = trailing};
  }
  // A tab of `size` at the bottom edge of `area`, at its trailing side (the left side right to left).
  inline LayoutRect statusTabRect(const LayoutRect& area, LayoutSize size) {
    const auto room = statusTabRoom(area.width);
    const float width = std::min(size.width, room.maxWidth);
    return {
        area.x + (Style::rtl() ? room.trailing : area.width - room.trailing - width),
        area.y + area.height - size.height, width, size.height
    };
  }
} // namespace luciole
