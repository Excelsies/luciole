#pragma once

#include "workbench/flame_row.h"
#include "workbench/point.h"
#include "workbench/ring_geometry.h"
#include "workbench/tree.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The text around the ring diagram: the hub's lines and trail chips, the ancestor stack and the readout line.
namespace luciole::workbench::iris {
  inline constexpr double kHubLineHeight = 1.2; // a hub line is this many font sizes tall
  inline constexpr double kChipHeight = 17.0;
  inline constexpr double kChipPad = 9.0;  // padding and border beside a chip's text
  inline constexpr double kStackPad = 6.0; // padding beside an ancestor name
  inline constexpr double kReadoutFont = 11.0;
  // Semibold text without a measured font runs this many times as wide as regular text.
  inline constexpr double kHubSemiboldWidth = 1.1;
  inline constexpr double kNoteClearance = 2.0; // the note keeps this far inside the hub's border

  // Widths of label text: the label font's advances at kBaseSize px when known, else `advance` per code point
  // at 11 px. Semibold text uses `boldFont` the same way, else the regular width times kHubSemiboldWidth. Widths
  // scale linearly with the size.
  struct TextMeasure {
    static constexpr double kBaseSize = 12;
    std::optional<LabelFont> font;
    std::optional<LabelFont> boldFont;
    double advance = 6.0;

    double width(std::string_view text, double size, bool bold = false) const;
    // `text` cut in the middle to the most code points that fit `room`; an ellipsis alone when none do, and
    // empty for no room or a size that is not positive.
    std::string fit(std::string_view text, double room, double size, bool bold = false) const;
  };

  enum class HubInk : std::uint8_t { Name, Total, Items, Note };

  // One line of the hub; its box is as wide as the text and centered on the dial.
  struct HubLine {
    HubInk ink = HubInk::Name;
    std::string text;
    Rect box;
    double fontSize = 0;
  };

  struct TrailChip {
    std::size_t index = 0; // into the trail
    bool gap = false;      // entries before this one are left out
    NodeId node = kNoNode;
    std::string text; // the fitted name, after an ellipsis when `gap`
    Rect box;         // the chip itself
    double fontSize = 0;
  };

  struct Hub {
    ring::Point center;
    double radius = 0;
    double fontSize = 0;
    double nameWidth = 0;
    double chipWidth = 0;
    double chipFontSize = 0;
    bool tiny = false; // too small for text: a compact readout carries the name
    bool showItems = false;
    bool showTrail = false;
    bool canGoUp = false;
    std::string name;
    std::string total;
    std::string items;
    std::string note;             // why a folder shows no arcs; empty when it shows some
    std::vector<HubLine> lines;   // top to bottom; none for a tiny hub
    std::vector<TrailChip> chips; // below the lines
  };

  struct StackRow {
    NodeId node = kNoNode;
    std::size_t levels = 0; // how far up a click goes
    std::string text;       // fitted to the stack's text width
    double opacity = 1;
    Rect box; // the row's hover background: its text and padding, centered on the stack
  };

  // Ancestor names above the dial, nearest lowest; farther ones collapse into one line.
  struct AncestorStack {
    Rect box;
    double fontSize = 0;
    double textWidth = 0;       // widest text a row may have
    std::vector<StackRow> rows; // top to bottom
  };

  // The compact dial's line at the bottom, naming what is hovered or under the cursor.
  struct Readout {
    Rect box;
    double fontSize = kReadoutFont;
  };

  // Chips of a trail: all of a short one, else the first two and the last.
  std::vector<TrailChip> trailChips(std::size_t trailLength);

  // The hub of `folder` on `dial`: name, total, item count and `note` as lines, then a chip for each shown entry
  // of `trail`, all centered on the dial. The note wraps at its spaces onto as many lines as it needs within the
  // hub's body. When a word is wider than the body or a line would leave the hub's disc, the note shrinks, down to
  // 8 px, and only a word still wider than the body is cut. A tiny hub has no lines and no chips; a hub without room
  // for the trail has no chips. Unknown trail ids are left out.
  Hub hubLayout(
      const Tree& tree, const ring::Dial& dial, NodeId folder, std::span<const NodeId> trail, std::string note,
      const TextMeasure& measure
  );

  // Ancestors of tree.root, the nearest at the bottom; when more than the slots hold, the top row stands for the
  // farthest ones and goes to the top ancestor.
  AncestorStack ancestorStack(const Tree& tree, const ring::Dial& dial, const TextMeasure& measure);

  // The readout of a compact dial; none for a full one.
  std::optional<Readout> readout(const ring::Dial& dial);
} // namespace luciole::workbench::iris
