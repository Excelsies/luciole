#pragma once

#include "ui/controls/flex.h"

#include <memory>
#include <string_view>

class Label;

namespace luciole {
  // The status badge the side preview shows in its Details heading: a pill of SurfaceContainerHigh, spaceSm
  // padding, a caption label in OnSurfaceVariant, or Error for a problem.
  struct StatusBadge {
    Flex* pill = nullptr;
    Label* label = nullptr;
  };

  // The badge's height at the caption size, in logical pixels, as the side preview draws it.
  inline constexpr float kStatusBadgeHeight = 20.0F;

  // A hidden badge; `out` receives its parts.
  std::unique_ptr<Flex> makeStatusBadge(StatusBadge& out);
  // Shows `text` at `textSize`, the height scaled with it, or hides the badge for empty text.
  void showStatusBadge(const StatusBadge& badge, std::string_view text, bool problem, float textSize);
} // namespace luciole
