#pragma once

#include "workbench/atlas_hud.h"
#include "workbench/atlas_scene.h"
#include "workbench/color.h"
#include "workbench/flame_scene.h"
#include "workbench/flow_layout.h"
#include "workbench/iris_scene.h"
#include "workbench/ladder_plan.h"
#include "workbench/measure.h"
#include "workbench/nest_scene.h"
#include "workbench/point.h"
#include "workbench/shape.h"
#include "workbench/tree.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Shapes and labels of each Workbench scene, ready for one shape batch and a pool of text nodes.
namespace luciole::workbench {
  // What a shape is to the node in its id. ShapeId::role holds the SceneRole in its high byte and, for a piece drawn
  // as several shapes, the part index in its low byte.
  enum class SceneRole : std::uint8_t {
    Piece,      // an entry: tile, ring segment, sector, bubble, cell, bar segment or record
    More,       // a merged piece; its node is the folder whose entries it holds
    Path,       // the current folder's own bar where it also shows as an entry (flame graph)
    Far,        // a cell of the far line (flame graph)
    Files,      // the files bar of a column (flow); the part is the column
    Ribbon,     // the ribbon into a record (flow)
    MoreRibbon, // the ribbon into a More record (flow)
    Hub,        // the hub (ring diagram)
    Row,        // a label row: its hit area and its raised background (ring diagram)
    Leader,     // a leader line, one shape per polyline step (ring diagram)
    Stem,       // an ancestor pill (flow)
    Link,       // the line after an ancestor pill (flow)
    Bridge,     // the strip under an on-path ring segment (nest)
    Head,       // the head of a peek card (nest)
    Detail,     // drawing that belongs to a piece and takes no hits: patch, stripe, grain, separator, outline, dot
    Mark,       // an outline or tint over a piece for the cursor, the linked node or the pointer; takes no hits
    Stack,      // a row of the ancestor stack (ring diagram)
    Chip,       // an ancestor chip over the Atlas, whose current folder chip takes no hits, or an Iris trail chip,
                // which holds no node and carries its index as the part
    Button,     // a zoom button over the Atlas; the part is its index in HudLayout::buttons
  };

  std::uint16_t shapeRole(SceneRole role, std::size_t part = 0);
  SceneRole sceneRole(std::uint16_t role);

  // States a view shows. `hovered` is the id of the shape under the pointer in this part; its part index is ignored
  // except on a files bar, where it names the column, and on a button, and a hovered label row marks its sector.
  // `linked` and `cursor` are nodes; each marks the piece that stands for it: in Nest, Atlas and flow the scene's own
  // focus lookup, elsewhere the piece of that node or the merged piece holding it.
  struct ShapeMarks {
    std::optional<ShapeId> hovered;
    NodeId linked = kNoNode;
    NodeId cursor = kNoNode;
  };

  enum class LabelAlign : std::uint8_t { Start, Center, End };

  // The icon of a file category drawn with a label, in a square `size` px wide whose top left lies `left` and `top`
  // px from the label box's top left. No icon at size 0. It moves and fades with its label and is never turned.
  struct LabelIcon {
    FileCategory category = FileCategory::Other;
    float size = 0;
    float left = 0;
    float top = 0;
    Rgba8 color{};
  };

  // How a text that does not fit its box gives way.
  enum class LabelFit : std::uint8_t {
    Shorten, // a name: cut in the middle, keeping a few characters on each side and the extension, else left out
    Whole,   // a count or a measure: its short text, else left out; never cut
  };

  enum class LabelWeight : std::uint8_t { Regular, Semibold };

  enum class TrailPlace : std::uint8_t {
    End,   // at the right end of the label's own line, the label's text ending `gap` before it
    Below, // a line of its own under an unturned label, aligned as the label is
  };

  // A whole text, mostly a measure, that shows only while its label shows and fits whole. At the end of the line
  // it shows when it and `gap` take less than `room`; the label then shows without it when both do not fit.
  struct LabelTrail {
    std::string text;
    TrailPlace place = TrailPlace::End;
    float fontSize = 11;
    Rgba8 color{};
    float gap = 0;   // End
    float room = 0;  // End
    float dx = 0;    // Below: the line's box left from the label's box left
    float dy = 0;    // Below: the line's middle from the label's middle
    float width = 0; // Below: the line's box width
  };

  // A line of text for a text node: horizontally aligned in the box, vertically centered in it, turned by
  // `rotation` degrees clockwise about the box center.
  struct ShapeLabel {
    NodeId node = kNoNode;
    std::string text;
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    LabelAlign align = LabelAlign::Start;
    float fontSize = 12;
    float rotation = 0;
    Rgba8 color;
    LabelIcon icon{};
    LabelFit fit = LabelFit::Shorten;
    LabelWeight weight = LabelWeight::Regular;
    // Whole: shown when `text` does not fit.
    std::string shortText = {}; // NOLINT(readability-redundant-member-init): labels use designated initializers.
    std::optional<LabelTrail> trail = std::nullopt;
  };

  // Labels a scene gives at most; the pieces with the largest area keep theirs.
  inline constexpr std::size_t kMaxSceneLabels = 160;

  struct SceneShapes {
    std::vector<Shape> shapes;
    std::vector<ShapeLabel> labels; // largest pieces first
  };

  // Colors come from the palette and the nodes' colors. A piece whose measure is partial or a lower bound is hatched.
  // Shapes of the Detail and Mark roles draw over the piece they belong to and never take hits; where a drawn shape is
  // smaller than its piece, a transparent shape of the piece's own size takes the hits.

  // Ring segments as the rectangles and corner sectors of their band, ends inset as the scene's outlines are, with
  // a bridge under an on-path segment and outlines along the scene's outline; then core tiles and "+N more" as
  // rounded rectangles, the core's frame, and the cursor's ring.
  SceneShapes
  nestShapes(const Tree& tree, const NestScene& scene, const ShapeMarks& marks, const ColorPalette& palette);

  // As above, with the texture patches of this scene (nestTexturePatches) drawn on their tiles. A texture of another
  // size than the scene's pieces draws none.
  SceneShapes nestShapes(
      const Tree& tree, const NestScene& scene, std::span<const std::vector<NestPatch>> texture,
      const ShapeMarks& marks, const ColorPalette& palette
  );

  // The open peek card: its head with the folder's name and facts, its tiles and "+N more", and the cursor's ring.
  SceneShapes
  nestPeekShapes(const Tree& tree, const NestPeek& peek, const ShapeMarks& marks, const ColorPalette& palette);

  // Main and context ring sectors with gaps between them, the inner band of the pointed or cursor folder, the hub
  // with its lines and trail chips, label rows with their dots, icons and leaders, the ancestor stack, a compact
  // dial's readout, and the cursor's and the linked node's outlines. `stop` puts the cursor on "+N more".
  SceneShapes irisShapes(
      const Tree& tree, const iris::Scene& scene, const ShapeMarks& marks, const ColorPalette& palette,
      iris::CursorStop stop = iris::CursorStop::Marks
  );

  // The children of a folder arc along the inner edge of the main ring, over a quiet backing, as irisShapes draws
  // them over the rings. They take no hits.
  std::vector<Shape>
  irisInnerBandShapes(const iris::Scene& scene, const iris::InnerBand& band, const ColorPalette& palette);

  // Circles in the scene's painter order, a huge rim filling the view: translucent folders with an accent outline
  // over outlined ancestors, so nested folders stay apart. Then the hovered, linked and cursor outlines. Only
  // clickable items are hit targets, each a circle of the item's own radius. Labels are measured with `font`, the
  // label font's advance widths at kAtlasFontBase, so title chips and the pills behind names over children fit them.
  SceneShapes atlasShapes(
      const Tree& tree, const LayoutSettings& settings, const AtlasScene& scene, const ShapeMarks& marks,
      const ColorPalette& palette, const LabelFont& font
  );

  // The chips of the path and the zoom buttons over the Atlas, in `placed`, the rectangles placeHud gave for
  // layout's entries. An ancestor chip is a hit target with its node; the current folder's chip is not.
  SceneShapes atlasHudShapes(
      const Tree& tree, const HudLayout& layout, std::span<const Rect> placed, const ShapeMarks& marks,
      const ColorPalette& palette
  );

  // One rounded rectangle per item, as flameBox draws it, filled from the surface toward the node's color; merged
  // pieces quiet with a vertical grain, the current folder's bar plain. A hovered or linked folder ahead lights its
  // chain and its subtree and dims the other strata ahead.
  SceneShapes flameShapes(
      const Tree& tree, const LayoutSettings& settings, const FlameScene& scene, const ShapeMarks& marks,
      const ColorPalette& palette
  );

  // One rectangle per bar segment and remainder, faded by the bar's level: segments apart by a line of surface
  // color, bar ends rounded, the remainder hatched, the on-path segment outlined, the linked one tinted and outlined
  // in place of its parting line.
  SceneShapes ladderShapes(
      const Tree& tree, const LayoutSettings& settings, const LadderScene& scene, const ShapeMarks& marks,
      const ColorPalette& palette
  );

  // Links, ribbons, record bars and ancestor pills, then labels. Ribbons and records light up on the way to and
  // below the hovered or linked record and dim elsewhere. A folder with nothing to draw gets a note beside its bar.
  // Bars, their labels and the pills are hit targets, ribbons are not; a label answers over the width of its text,
  // measured with `font`, the label font's advance widths at 12 px.
  SceneShapes flowShapes(
      const Tree& tree, const LayoutSettings& settings, const FlowLayout& layout, const ShapeMarks& marks,
      const ColorPalette& palette, const LabelFont& font
  );
} // namespace luciole::workbench
