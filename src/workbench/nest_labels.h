#pragma once

#include "workbench/color.h"
#include "workbench/measure.h"
#include "workbench/tree.h"
#include "workbench/treemap.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Text boxes of the Nest: tile names and measures, "+N more" tiles and ring segment names. Fitting text to a width
// takes the view's measure function.
namespace luciole::workbench {
  enum class NestTurn : std::uint8_t { None, Clockwise, CounterClockwise };

  // Name and measure of a tile. The name is semibold at fontSize, the detail regular at 11 px.
  struct NestTileLabel {
    std::string name;
    std::string detail; // second line, or the measure at the right end of a one-line label
    double fontSize = 0;
    double iconSize = 0;                         // 0 for no icon
    TreemapPoint icon;                           // top left of the icon
    FileCategory category = FileCategory::Other; // what the icon shows
    bool twoLines = false;
    TreemapPoint nameAt;    // left end of the name baseline
    double nameWidth = 0;   // room for the name when no measure sits beside it
    TreemapPoint detailAt;  // two lines: left end of the second baseline; one line: right end of the measure
    double detailWidth = 0; // two lines: room for the second line; one line: the measure needs width + 14 under it
  };

  // "+N more" centered at `center` (13 px semibold), else "+N", else nothing, whichever fits maxWidth first. A size
  // line at 11 px sits 18 px lower when `size` is not empty and a first line shows.
  struct NestMoreLabel {
    std::string text;
    std::string shortText;
    double maxWidth = 0;
    TreemapPoint center;
    std::string size;
  };

  // Name centered on a straight side of a ring segment, turned along the left and right sides. Ring 1 also offers the
  // measure, shown after three spaces only when the whole name and it fit maxWidth.
  struct NestRingLabel {
    std::string name;
    std::string measure;
    TreemapPoint center;
    NestTurn turn = NestTurn::None;
    double fontSize = 0;
    double maxWidth = 0;
  };

  // Width of a text in px.
  using NestTextMeasure = std::function<double(std::string_view)>;

  // Label of a tile; none when the tile is narrower than 44 px or shorter than 22 px.
  std::optional<NestTileLabel>
  nestTileLabel(const Tree& tree, const LayoutSettings& settings, NodeId id, TreemapRect rect);

  // The width a name has beside nothing in a tile of `rect`: what nestTileLabel gives nameWidth, or 0 when the tile
  // has no label.
  double nestTileNameRoom(TreemapRect rect);

  // Width of the narrowest text fitNestText can give `text`: its shortest cut or the whole text, the narrower; the
  // whole text when it has no cut. 0 for an empty text.
  double narrowestNestTextWidth(std::string_view text, const NestTextMeasure& measure);

  // Whether a one-line label shows its measure of the given width, and the room then left for the name.
  bool nestMeasureShown(const NestTileLabel& label, double measureWidth);
  double nestNameRoom(const NestTileLabel& label, double measureWidth);

  // Label of a "+N more" tile standing for `count` entries of `bytes` in all, the size line as stateText writes it for
  // `state`, the most doubtful state among the entries.
  NestMoreLabel nestMoreLabel(std::size_t count, double bytes, MeasureState state, TreemapRect rect);

  // `text` whole when it fits maxWidth, else cut in the middle to the most code points `measure` says fit, keeping at
  // least three on each side of the ellipsis and an extension of up to eight characters whole after it. Empty when
  // maxWidth is under 18 or no such cut fits. Calls `measure` at most once per halving step.
  std::string fitNestText(std::string_view text, double maxWidth, const NestTextMeasure& measure);
} // namespace luciole::workbench
