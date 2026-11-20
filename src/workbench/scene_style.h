#pragma once

#include "workbench/color.h"
#include "workbench/measure.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench/tree.h"

#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Styling and label ranking shared by the scene shape builders; private to them. Strengths are the alpha a role or
// accent is drawn with, or how far one color is mixed toward another; widths and radii are logical pixels.
namespace luciole::workbench::scene_style {
  inline constexpr double kDetailFont = 11;
  inline constexpr double kCaptionFont = 13;
  inline constexpr double kHatch = 0.4; // onSurfaceVariant stripes of a piece whose totals are not final

  // Nest
  inline constexpr double kNestTileRadius = 6;
  inline constexpr double kNestPeekTileRadius = 3;
  inline constexpr double kNestMoreRadius = 5;
  inline constexpr double kNestCardRadius = 9;
  inline constexpr double kNestPatchRadius = 2;
  inline constexpr double kNestPatchInset = 1;
  inline constexpr double kNestPatch = 0.17;       // accent of a texture patch
  inline constexpr double kNestTileEdge = 0.32;    // accent outline of a tile
  inline constexpr double kNestPeekTileEdge = 0.4; // accent outline of a peek tile
  inline constexpr double kNestHotEdge = 0.95;     // accent outline of a hovered tile
  inline constexpr double kNestHotTint = 0.09;     // a hovered tile toward onSurface
  inline constexpr double kNestPeekTint = 0.3;     // a peek tile from surfaceContainerHigh toward its accent
  inline constexpr double kNestMoreFill = 0.07;    // secondary
  inline constexpr double kNestMoreFillHot = 0.18;
  inline constexpr double kNestMoreEdge = 0.5;
  inline constexpr double kNestMoreEdgeHot = 0.9;
  inline constexpr double kNestCoreEdge = 0.4; // primary outline of the core inside rings
  inline constexpr double kNestCoreEdgeWidth = 1.5;
  inline constexpr double kNestThinRing = 7;    // a ring this thick or thinner has no outlines or labels
  inline constexpr double kNestThinFill = 0.5;  // accent of a thin ring's segment
  inline constexpr double kNestThinPath = 0.6;  // primary of a thin ring's on-path segment
  inline constexpr double kNestLinePath = 0.38; // primary of the shared line's on-path segment
  inline constexpr double kNestPathEdge = 0.9;  // primary outline of an on-path segment
  inline constexpr double kNestBridge = 0.55;   // primary
  inline constexpr double kNestHatch = 0.42;    // onSurfaceVariant stripes of a merged segment
  inline constexpr double kNestRingHotTint = 0.16;
  inline constexpr double kNestCardEdge = 0.45;  // primary
  inline constexpr double kNestFocusTint = 0.28; // toward tertiary
  inline constexpr double kNestFocusWidth = 2.5;
  inline constexpr double kNestCursorWidth = 2;
  inline constexpr double kNestCursorHalo = 0.22; // primary
  inline constexpr double kNestCursorHaloWidth = 5;

  // Iris
  inline constexpr double kIrisMainGap = 2; // cut between main ring sectors, measured at the middle radius
  inline constexpr double kIrisContextGap = 1.4;
  inline constexpr double kIrisBandGap = 1.2;
  inline constexpr double kIrisBackingGap = 1.4;
  inline constexpr double kIrisMarkGap = 1;
  inline constexpr double kIrisContextFill = 0.55;
  inline constexpr double kIrisContextHot = 0.85;
  inline constexpr double kIrisPathEdgeWidth = 1.4;
  inline constexpr double kIrisBacking = 0.78; // surface under the inner band
  inline constexpr double kIrisLeaderEnd = 0.3;
  inline constexpr double kIrisLeaderMiddle = 0.13;
  inline constexpr double kIrisLeaderDot = 0.55;
  inline constexpr double kIrisLeaderDotRadius = 1.6;
  inline constexpr double kIrisLeaderMarkedWidth = 1.6;
  inline constexpr double kIrisLeaderCursorWidth = 2;
  inline constexpr double kIrisLeaderMarkedDot = 3;
  inline constexpr double kIrisMarkWidth = 2;
  inline constexpr double kIrisRowRadius = 6;
  inline constexpr double kIrisStackRadius = 3;

  // Atlas
  inline constexpr double kAtlasAncestorFill = 0.9;  // surfaceContainerLow, times the level's strength
  inline constexpr double kAtlasAncestorEdge = 0.55; // outlineVariant, times the level's strength
  inline constexpr double kAtlasAncestorFalloff = 0.6;
  inline constexpr double kAtlasSelfFill = 0.55; // surfaceContainer
  inline constexpr double kAtlasSelfEdge = 0.5;  // secondary
  inline constexpr double kAtlasSelfEdgeWidth = 1.5;
  inline constexpr double kAtlasMoreFill = 0.35; // surfaceContainerHighest
  inline constexpr double kAtlasMoreEdge = 0.65; // onSurfaceVariant
  inline constexpr double kAtlasMoreEdgeWidth = 1.25;
  inline constexpr double kAtlasFolderFill = 0.5;
  inline constexpr double kAtlasFolderEdge = 0.85;
  inline constexpr double kAtlasFolderEdgeWidth = 1.5;
  inline constexpr double kAtlasSmallEdgeWidth = 1; // for a circle of 10 px or less
  inline constexpr double kAtlasFileFill = 0.96;
  inline constexpr double kAtlasInsetMost = 2; // a drawn circle sits this far inside its radius, at most
  inline constexpr double kAtlasInsetShare = 0.04;
  inline constexpr double kAtlasMarkFrom = 0.2; // circles fainter than this show no state
  inline constexpr double kAtlasHotFill = 0.07; // onSurface
  inline constexpr double kAtlasHotEdge = 0.85;
  inline constexpr double kAtlasHotWidth = 2;
  inline constexpr double kAtlasFocusFill = 0.1; // tertiary
  inline constexpr double kAtlasFocusEdge = 0.95;
  inline constexpr double kAtlasFocusWidth = 2.5;
  inline constexpr double kAtlasFocusGrow = 3;
  inline constexpr double kAtlasCursorWidth = 3;
  inline constexpr double kAtlasCursorGrow = 4;
  inline constexpr double kAtlasCursorLeast = 9;
  inline constexpr double kHudChipFill = 0.78; // surface under a chip or button
  inline constexpr double kHudChipRadius = 6;
  inline constexpr double kHudButtonRadius = 9;
  inline constexpr double kHudIconSize = 16;
  inline constexpr double kHudIconWidth = 2.0 * kHudIconSize / 24; // a 2 unit stroke of a 24 unit icon

  // Flame graph
  inline constexpr double kFlameAccentFill = 0.9; // a thin pinned segment from surface toward its accent
  inline constexpr double kFlameBarRadius = 4;
  inline constexpr double kFlamePinWidth = 1.5;
  inline constexpr double kFlameHotWidth = 1.5;
  inline constexpr double kFlameCursorWidth = 2;
  inline constexpr double kFlameSubTint = 0.5; // a focused folder's subtree toward its accent
  inline constexpr double kFlameDim = 0.45;    // strata ahead off the focused chain
  inline constexpr double kFlameGrainStep = 5;
  inline constexpr double kFlameBarIcon = 34; // room left of the bar's name for the folder icon

  // Ladder. A far bar fades toward surfaceContainerLow, the ribbon pane's background, rather than turning
  // translucent, so the parts of a bar that overlap stay opaque.
  inline constexpr double kLadderFillToSurface = 0.3; // a segment from its accent toward surface
  inline constexpr double kLadderRadius = 3;
  inline constexpr double kLadderHotTint = 0.2; // toward onSurface
  inline constexpr double kLadderFocusTint = 0.25;
  inline constexpr double kLadderFocusWidth = 2;
  inline constexpr double kLadderPathWidth = 2;
  inline constexpr double kLadderFarPathWidth = 1; // from the fourth bar on
  inline constexpr std::size_t kLadderWidePathBars = 3;
  inline constexpr double kLadderName = 0.8; // onSurfaceVariant of a gutter name

  // Flow
  inline constexpr double kFlowRadius = 3;
  inline constexpr double kFlowStripe = 0.85; // accent stripe at a bar's left edge
  inline constexpr double kFlowStripeWidth = 2.5;
  inline constexpr double kFlowStripeInset = 1.5;
  inline constexpr double kFlowQuietEdge = 0.8;  // onSurfaceVariant outline of a files or More bar
  inline constexpr double kFlowFilesHatch = 0.3; // onSurfaceVariant
  inline constexpr double kFlowRibbonTint = 0.2; // a ribbon from the record's fill toward its accent
  inline constexpr double kFlowRibbon = 0.4;
  inline constexpr double kFlowRibbonMuted = 0.15;
  inline constexpr double kFlowRibbonLit = 0.82;
  inline constexpr double kFlowRibbonDim = 0.1;
  inline constexpr double kFlowDim = 0.32;      // records and pills off the lit path
  inline constexpr double kFlowLabelDim = 0.42; // their labels
  inline constexpr double kFlowLinkDim = 0.5;
  inline constexpr double kFlowLinkWidth = 2;
  inline constexpr double kFlowRingWidth = 1.5;
  inline constexpr double kFlowCursorWidth = 1.75;
  inline constexpr double kFlowCursorGrow = 2.5;
  inline constexpr double kFlowNoteRule = 0.5; // outlineVariant of the dashed rule toward the note
  inline constexpr double kFlowNoteHint = 0.8; // onSurfaceVariant of the note's second line

  // Gamma-encoded color with `alpha` (0 to 1) as its straight alpha.
  Rgba8 tone(Rgb color, double alpha);
  Rgba8 opaque(Rgb color);

  ShapeId sid(NodeId node, SceneRole role, std::size_t part = 0);

  // A transparent rectangle that only takes hits.
  Shape hitArea(ShapeId id, double x, double y, double width, double height);

  // A copy that only takes hits: fill and stroke transparent.
  Shape hitOnly(Shape shape);

  // An outline of `width` centered `grow` px outside the box (inside for a negative grow), its path rounded by
  // `radius`. Transparent inside and takes no hits.
  Shape ringAround(ShapeId id, const ShapeRect& box, double grow, double radius, Rgba8 color, double width);

  // A circle outline of `width` centered on radius `radius`. Transparent inside and takes no hits.
  Shape ringCircle(ShapeId id, ShapePoint center, double radius, Rgba8 color, double width);

  // True when `mark` is `piece` or one of the nodes merged into it.
  bool marked(NodeId mark, NodeId piece, std::span<const NodeId> merged = {});

  // True for a node whose totals are not final: still measuring, or a lower bound.
  bool hatched(const Tree& tree, NodeId id);

  std::string nodeName(const Tree& tree, NodeId id);

  struct Builder {
    const Tree& tree;
    const ShapeMarks& marks;
    const ColorPalette& palette;
    SceneShapes out;
    std::vector<std::pair<double, ShapeLabel>> labels;

    // True when the pointer is over the piece `id` belongs to: the part of a files bar or a button names it and
    // must match; other parts split one piece and are ignored.
    bool hovered(ShapeId id) const;

    Shape& put(Shape shape);

    // A straight line in `color` that takes no hits.
    void line(ShapeId id, Point from, Point to, Rgba8 color, double width);

    // One line of text, vertically centered on `middle`, ranked by the area of its piece. None for empty text or no
    // room.
    ShapeLabel* label(
        NodeId node, std::string text, double x, double middle, double width, Rgb color, double area,
        LabelAlign align = LabelAlign::Start, double size = 12
    );

    // The shapes and the labels of the largest pieces, at most kMaxSceneLabels.
    SceneShapes finish();
  };

  // Node colors with one color range per parent folder.
  struct Colors {
    const Tree& tree;
    const LayoutSettings& settings;
    const ColorPalette& palette;
    std::unordered_map<NodeId, ColorRange> ranges;

    NodeColor of(NodeId id);
  };
} // namespace luciole::workbench::scene_style
