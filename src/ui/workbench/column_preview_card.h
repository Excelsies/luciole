#pragma once

#include "ui/controls/flex.h"
#include "ui/palette.h"

#include <memory>

namespace luciole {
  // The surface the Workbench preview sits on: its header, its page cards and the frames of its pictures round into
  // this role.
  inline constexpr ColorRole kColumnPreviewSurface = ColorRole::SurfaceContainerLow;

  // The side preview's text page: a card of Style::radiusXl corners and an OutlineVariant outline on
  // kColumnPreviewSurface, inset by Style::spaceSm above and below and by `paddingH` at the sides, clipping what it
  // holds. Every card of the Workbench preview is made here, so it changes with the side preview's in one place.
  std::unique_ptr<Flex> makePageCard(float paddingH);
} // namespace luciole
