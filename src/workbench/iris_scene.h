#pragma once

#include "workbench/color.h"
#include "workbench/flame_row.h"
#include "workbench/iris_overlay.h"
#include "workbench/measure.h"
#include "workbench/point.h"
#include "workbench/ring_geometry.h"
#include "workbench/ring_leaders.h"
#include "workbench/tree.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The ring diagram of the current folder as plain pieces: sectors, label rows, arc labels, hub and ancestor stack.
namespace luciole::workbench::iris {
  inline constexpr double kMinArcDegrees = 1.5;
  inline constexpr double kContextMergeDegrees = 2.0;
  inline constexpr double kCompactSegmentLength = 12.0; // shortest context segment of a compact dial, px
  inline constexpr double kArcLabelHeight = 13.0;
  inline constexpr std::size_t kArcLabelMaxChars = 40;
  inline constexpr double kInnerBandMinDegrees = 12.0;
  inline constexpr std::size_t kFilterNoteChars = 200;

  enum class SectorKind : std::uint8_t { Entry, More, Rest };

  struct Sector {
    SectorKind kind = SectorKind::Entry;
    NodeId node = kNoNode;      // the entry; kNoNode for More and Rest
    std::vector<NodeId> merged; // entries inside a More or Rest sector of a ring, heaviest first
    ring::Arc arc;
    bool onPath = false; // context segment on the way to the current folder
    NodeColor color;     // all zero for More and Rest
  };

  // Level 0 is the main ring, level 1 the parent's entries, and so on outward.
  struct SectorRing {
    std::size_t level = 0;
    NodeId folder = kNoNode;
    ring::Band band;
    std::vector<Sector> sectors;
  };

  struct LabelRow {
    std::size_t sector = 0; // index into the main ring sectors
    ring::Side side = ring::Side::Right;
    double degrees = 0; // middle of the arc
    double y = 0;       // row center
    bool shown = true;  // false: a keyboard-only row of a compact dial without a legend
    Rect box;
    double nameWidth = 0; // room for the name
    std::string name;     // entry name, or "+N more"
    std::string metric;
    ring::Polyline leader; // empty in a legend
  };

  enum class RowMode : std::uint8_t { Straight, Lanes, Legend };

  // Rows in visual order, top to bottom. The right column takes arcs clockwise from 12 o'clock.
  struct LabelColumns {
    std::vector<LabelRow> right;
    std::vector<LabelRow> left;
    RowMode mode = RowMode::Straight;
  };

  // A name inside a main ring arc of a compact dial.
  struct ArcLabel {
    std::size_t sector = 0;
    std::string text;
    ring::Point center;
    double width = 0;
    double height = kArcLabelHeight;
    Rgb ink;
  };

  struct SceneInput {
    double width = 0;
    double height = 0;
    bool compact = false; // from ring::chooseCompact
    LayoutSettings settings;
    std::string_view filter;   // typed filter text, named in the empty-folder note
    double labelAdvance = 6.0; // width of one code point of an 11 px arc label; labels need it positive, finite
    ColorPalette palette;
    std::vector<NodeId> trail;         // folders below the root the hub offers as chips, nearest first
    std::optional<LabelFont> font;     // advances of the label font at TextMeasure::kBaseSize px
    std::optional<LabelFont> boldFont; // the same for semibold text
  };

  struct Scene {
    ring::Dial dial;
    NodeId folder = kNoNode;
    SectorRing main;
    std::vector<SectorRing> rings; // context rings, nearest first, at most four
    LabelColumns rows;
    std::vector<ArcLabel> arcLabels; // compact dial only
    Hub hub;
    AncestorStack stack;
    std::optional<Readout> readout; // compact dial only
    std::size_t entryCount = 0;     // visible entries of the folder
    LayoutSettings settings;
  };

  // Where the keyboard cursor of a dial rests: on the piece of ShapeMarks::cursor, or on the main ring's "+N more",
  // which stands for no single entry.
  enum class CursorStop : std::uint8_t { Marks, More };

  // Scene of tree.root, its entries and up to four ancestor rings from tree.ancestors. None for a host that cannot
  // be laid out or an unknown root.
  std::optional<Scene> buildScene(const Tree& tree, const SceneInput& input);

  // Arcs a compact dial shows: min(8, rowsFit - 1) with a legend, else 7, 6 or 5 by the main ring radius.
  std::size_t compactArcCount(const ring::Dial& dial);

  // The sentence the hub shows when a folder has no visible entries; empty when it has some. A folder still being
  // measured is loading.
  std::string hubNote(const Tree& tree, NodeId folder, std::size_t entryCount, std::string_view filter);

  // Children of a folder arc drawn along its inner edge.
  struct InnerBand {
    ring::Arc arc;
    ring::Band backing;
    ring::Band band;
    std::vector<Sector> sectors; // Rest for the merged tail, without its entries
  };

  // Band for a folder arc of the main ring at least kInnerBandMinDegrees wide with visible children; none
  // otherwise.
  std::optional<InnerBand> innerBand(
      const Tree& tree, const Scene& scene, NodeId node, const LayoutSettings& settings,
      const ColorPalette& palette = {}
  );
} // namespace luciole::workbench::iris
