#pragma once

#include "workbench/atlas_text.h"
#include "workbench/flame_row.h"
#include "workbench/pane_layout.h"
#include "workbench/point.h"
#include "workbench/tree.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// The Atlas chips for the path and its zoom buttons.
namespace luciole::workbench {
  // Below 460 px on either side the chips and zoom buttons share one 26 px bar above the map.
  inline constexpr double kAtlasCompactBelow = 460;
  inline constexpr double kAtlasBarHeight = 26;
  // Room between a chip's edge and its text: in the column, in the bar, and before "+N levels".
  inline constexpr double kAtlasChipPad = 9;
  inline constexpr double kAtlasBarChipPad = 6;
  inline constexpr double kAtlasMoreLevelsPad = 4;

  enum class HudEntryKind : std::uint8_t { MoreLevels, Ancestor, Current };

  struct HudEntry {
    HudEntryKind kind = HudEntryKind::Ancestor;
    std::size_t levels = 0; // Ancestor: levels to go out; MoreLevels: levels not shown
    double maxWidth = 0;
    double fontSize = 0;
    double opacity = 1;
    std::size_t maxChars = 0; // middle ellipsis length for the name
  };

  struct HudLayout {
    bool compact = false;
    ViewSize view;
    Rect map;                    // the area left for the circles
    Rect chipArea;               // chips are clipped to it
    std::array<Rect, 3> buttons; // zoom in, zoom out, recenter
    std::vector<HudEntry> entries;
  };

  struct Extent {
    double width = 0;
    double height = 0;
  };

  // Chips for `ancestorCount` ancestors in display order (farthest first) and the button rectangles.
  HudLayout hudLayout(ViewSize view, std::size_t ancestorCount);

  // Rectangles for the entries given their natural sizes: a column from the top left, or in the compact bar
  // a row of 20 px chips that shrink together (not under 28 px) to fit beside the buttons.
  std::vector<Rect> placeHud(const HudLayout& layout, std::span<const Extent> natural);

  // The text of an entry: "+N levels", or the folder's name cut in the middle to maxChars, "/" for an unnamed top.
  // Empty for an ancestor `tree` does not have.
  std::string hudText(const Tree& tree, const HudEntry& entry);

  // Natural sizes of the entries for placeHud: the text measured with `font` (advance widths at 12 px) but at least
  // kAtlasLeastTextRoom, the padding each side and a pixel of border each side; 1.35 lines and 6 px tall, 20 px in
  // the compact bar.
  std::vector<Extent> hudExtents(const Tree& tree, const HudLayout& layout, const LabelFont& font);

  // The folder's path from the top of the snapshot, as a chip's tooltip gives it: names joined by '/', an unnamed top
  // reading as "/". Empty for an unknown node.
  std::string hudPath(const Tree& tree, NodeId node);
} // namespace luciole::workbench
