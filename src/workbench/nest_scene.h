#pragma once

#include "workbench/color.h"
#include "workbench/measure.h"
#include "workbench/nest_labels.h"
#include "workbench/tree.h"
#include "workbench/treemap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace luciole::workbench {
  struct NestSize {
    double w = 0;
    double h = 0;
  };

  // Sizes of the frame, tiles and peek card. The compact profile serves hosts under 640 wide or 420 tall.
  struct NestProfile {
    bool compact = false;
    std::span<const double> ringThickness; // nearest ring first
    double lineThickness = 0;              // one line shared by every ancestor beyond the rings
    double gap = 0;                        // between rings
    double coreGap = 0;                    // between the nearest ring and the core
    double pad = 0;                        // between the host edge and the outermost ring
    double coreRadius = 0;
    NestSize minTile;
    NestSize minPeekTile;
    NestSize minCore;
    NestSize peekMin;  // smallest card
    NestSize peekNeed; // smallest core that can hold a card
    double peekHead = 0;
  };

  // Compact when width < 640 or height < 420.
  NestProfile nestProfile(double width, double height);

  inline constexpr std::size_t kNestLineLevel = 6;
  inline constexpr int kNestPeekDwellMs = 350; // pointer rest on a folder tile before its peek opens
  inline constexpr int kNestPeekOpenMs = 140;

  enum class NestPieceKind : std::uint8_t {
    Tile,
    More,          // "+N more" tile of the core
    Segment,       // one entry on an ancestor ring
    MergedSegment, // entries too short for their own segment
    LineSegment,   // one far ancestor on the shared line, or all of them when they are too many for 6 px each
    PeekHead,
    PeekTile,
    PeekMore,
  };

  using NestLabel = std::variant<std::monostate, NestTileLabel, NestMoreLabel, NestRingLabel>;

  struct NestPiece {
    NestPieceKind kind = NestPieceKind::Tile;
    NodeId node = kNoNode;     // the entry; the folder for More, MergedSegment, PeekHead and PeekMore; the nearest
                               // ancestor for a shared LineSegment
    std::vector<NodeId> nodes; // More, MergedSegment, PeekMore and a shared LineSegment: the nodes they stand for
    TreemapRect rect;          // tiles; segments: box of a straight segment, else 4 px around its middle
    std::vector<TreemapPoint> outline; // segments, inset 1 px at each end (0.4 px when 10 px long or less)
    std::vector<TreemapPoint> bridge;  // on-path segment of a ring: strip reaching inward across the gap
    std::size_t ring = 0;              // segments: index into NestScene::rings
    double s0 = 0;
    double s1 = 0;
    bool onPath = false; // segment of the ancestor that leads to the current folder
    bool folder = false;
    NodeColor color;
    NestLabel label;
  };

  struct NestRing {
    std::size_t level = 0; // 1 is the parent's ring; kNestLineLevel for the shared line
    bool line = false;
    NodeId folder = kNoNode; // the ancestor whose entries the ring shows; none for the line
    double thickness = 0;
    double extension = 0; // gap to the next ring inward, or to the core
    Band band;
    std::size_t firstPiece = 0;
    std::size_t pieceCount = 0;
  };

  struct NestScene {
    double width = 0;
    double height = 0;
    NodeId folder = kNoNode;
    NestProfile profile;
    TreemapRect core;
    double coreRadius = 0;
    std::vector<NestRing> rings;   // outermost first
    std::vector<NestPiece> pieces; // tiles heaviest first, the "+N more" tile, then ring segments ring by ring
    bool empty = false;            // the folder has no visible entries
    NestTextMeasure nameWidth;     // widths of names as drawn, which the peek card lays out with too; may be empty
  };

  struct NestPeek {
    NodeId node = kNoNode;
    TreemapRect from; // the tile it opened from
    TreemapRect card;
    TreemapRect head;
    std::string name; // header name, 13 px semibold, left at nameAt, vertically centered
    TreemapPoint nameAt;
    double nameWidth = 0;
    std::string facts; // item count and measure, 11 px, right end at factsAt
    TreemapPoint factsAt;
    std::size_t count = 0;         // visible entries of the folder
    std::vector<NestPiece> pieces; // the head, then tiles, then "+N more"
  };

  struct NestPatch {
    TreemapRect rect;
    NodeColor color;
  };

  // Scene of tree.root for a host of width by height: rings for tree.ancestors (dropped outermost first until the core
  // meets its minimum), then the core treemap of at most 700 tiles plus one "+N more" tile. A host that is not finite
  // and at least 1 by 1 gives an empty scene. With `nameWidth` every tile is wide enough for the narrowest form of its
  // name, so an entry is folded only when its tile could not show its name; without it only the minimum size counts.
  NestScene buildNestScene(
      const Tree& tree, const LayoutSettings& settings, double width, double height, const ColorPalette& palette = {},
      NestTextMeasure nameWidth = {}
  );

  // Piece under (x, y): a core tile or "+N more" inside the core, else a ring segment.
  std::optional<std::size_t> nestHit(const NestScene& scene, double x, double y);

  // Piece standing for a node: its tile, the "+N more" tile holding it, or the ring segment of its nearest ancestor
  // that a ring shows. None for the current folder, its own ancestors without a segment, and strangers.
  std::optional<std::size_t> nestFocus(const Tree& tree, const NestScene& scene, NodeId id);

  // A peek may start its dwell over a core tile of a folder with at least one item.
  bool nestPeekAllowed(const Tree& tree, const NestScene& scene, std::size_t piece);

  // Card of a folder tile's entries, kept inside the core: at most 11 tiles weighted by the square root of their
  // weight, plus "+N more". None when the folder has no visible entries or the core is under the profile's need.
  std::optional<NestPeek> buildNestPeek(
      const Tree& tree, const LayoutSettings& settings, const NestScene& scene, std::size_t piece,
      const ColorPalette& palette = {}
  );

  // Piece of the peek under (x, y); anywhere on the card outside a tile is the head. None outside the card, which
  // is when an open peek closes.
  std::optional<std::size_t> nestPeekHit(const NestPeek& peek, double x, double y);
} // namespace luciole::workbench
