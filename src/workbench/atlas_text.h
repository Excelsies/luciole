#pragma once

#include "workbench/flame_row.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

// Fitting the Atlas's label text with measured widths.
namespace luciole::workbench {
  // The size the label font's advance widths are given at; other sizes scale linearly.
  inline constexpr double kAtlasFontBase = 12;
  // Text boxes are at least this wide, as narrower labels are not drawn.
  inline constexpr double kAtlasLeastTextRoom = 18;

  // Width of `text` at `size` px; 0 for a size that is not positive and finite.
  double atlasTextWidth(std::string_view text, const LabelFont& font, double size);

  // `text` fitted to `maxWidth` as fitNestText fits it: whole, else cut in the middle keeping three code points on
  // each side and the extension, else empty.
  std::string fitAtlasText(std::string_view text, double maxWidth, const LabelFont& font, double size);

  // `name` split in two after the '-', '_', '.' or ' ' that makes the wider line narrowest, else in the middle. Only
  // separators that leave at least three code points before them and two after count.
  std::array<std::string, 2> splitAtlasName(std::string_view name, const LabelFont& font, double size);

  // The name rows centered in a circle of radius `r`, for a block `total` px tall with rows `rowHeight` px tall: one
  // line when it fits; for a circle of 40 px or more and a name of 8 code points or more, two lines when both fit
  // whole, or both cut when the circle is 56 px or more; else one cut line. Empty when nothing fits.
  std::vector<std::string>
  atlasNameRows(std::string_view name, double r, double total, double rowHeight, const LabelFont& font, double size);
} // namespace luciole::workbench
