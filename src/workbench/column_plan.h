#pragma once

#include "model/details_columns.h"
#include "model/directory_options.h"
#include "workbench/measure.h"
#include "workbench/pane_layout.h"
#include "workbench/tree.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace luciole::workbench {
  inline constexpr std::array<double, 4> kAncestorStripWidths{44, 34, 26, 20}; // nearest first; farther ones are 20
  inline constexpr double kStackStripWidth = 24;                               // the "+N levels" band
  inline constexpr double kStripGap = 1;

  // Width of the `count` nearest named bands, with the gap after each.
  constexpr double stripBandsWidth(std::size_t count) {
    const std::size_t first = std::min(count, kAncestorStripWidths.size());
    double width = 0;
    for (std::size_t i = 0; i < first; ++i) { // at most four
      width += kAncestorStripWidths[i] + kStripGap;
    }
    return width + (static_cast<double>(count - first) * (kAncestorStripWidths.back() + kStripGap));
  }

  // The strip pane holds one, four or twelve named bands and the "+N levels" band.
  inline constexpr double kStripsMinWidth = stripBandsWidth(1) + kStackStripWidth + kStripGap;
  inline constexpr double kStripsDefaultWidth = stripBandsWidth(4) + kStackStripWidth + kStripGap;
  inline constexpr double kStripsMaxWidth = stripBandsWidth(12) + kStackStripWidth + kStripGap;
  // The most bands the widest pane shows: thirteen named ones fit it without folding.
  inline constexpr std::size_t kMaxStrips = 13;
  static_assert(stripBandsWidth(kMaxStrips) <= kStripsMaxWidth && stripBandsWidth(kMaxStrips + 1) > kStripsMaxWidth);
  static_assert(kStripsMinWidth == kMinWorkbenchStripsWidth && kStripsMaxWidth == kMaxWorkbenchStripsWidth);

  inline constexpr std::size_t kColumnsAhead = 3;
  inline constexpr std::array<double, kColumnsAhead> kDefaultColumnWidths{260, 190, 140};
  inline constexpr double kColumnMin = 140;
  inline constexpr double kColumnMax = 640;
  inline constexpr double kListFloor = 320; // the list's minimum width
  static_assert(kCenterMinWidth >= kListFloor + kStripsMinWidth);
  inline constexpr double kListPreferred = 470; // Name, Size and Modified, kept before a second column
  inline constexpr double kListMax = 1272;
  inline constexpr std::array<double, kColumnsAhead> kColumnRowHeights{26, 24, 22};
  inline constexpr double kColumnHeaderHeight = 36;

  enum class StripKind : std::uint8_t { Ancestor, Stack };

  // An ancestor strip shows `level` (1 is the nearest); a stack strip, the "+N levels" band, holds levels `level` to
  // `lastLevel`.
  struct Strip {
    StripKind kind = StripKind::Ancestor;
    std::size_t level = 0;
    std::size_t lastLevel = 0;
    double width = 0;
    double x = 0;
    bool operator==(const Strip&) const = default;
  };

  // Widths of the columns ahead, and the narrowest list the user dragged to (320 up to 470, rounded), if any.
  struct ColumnWidths {
    std::array<double, kColumnsAhead> columns = kDefaultColumnWidths;
    std::optional<double> list;
    bool operator==(const ColumnWidths&) const = default;
  };

  struct PlanOptions {
    bool strips = true;
    ColumnWidths widths;
    double listMin = kListFloor;
    std::optional<std::size_t> maxColumns; // a drag holds the column count it started with
    std::optional<double> stripsWidth;     // the strip pane's saved width; none is the default
    double textScale = 1;                  // the system font's size over its default: the bands grow with it
  };

  struct ListSpan {
    double x = 0;
    double width = 0;
  };

  struct PlannedColumn {
    std::size_t index = 0;
    double x = 0;
    double width = 0;
  };

  // The strip pane, then the list, then the columns, left to right with a 1 px gap after each strip and column. The
  // strips sit against the list; the pane's room beyond them stays empty.
  struct ColumnPlan {
    std::vector<Strip> strips; // farthest first
    ListSpan list;
    std::vector<PlannedColumn> columns;
    double margin = 0;      // left offset that centers the group on a very wide view
    double stripsWidth = 0; // the strip pane; 0 without strips
    double stripsMax = 0;   // the widest the strip pane may be made while the list keeps its minimum; 0 without strips
    double stripsDefault = 0; // the pane's default width at the plan's text scale; 0 without strips
    double stripsMin = 0;     // the pane's least width at that scale; 0 without strips
    double stripsCeiling = 0; // the pane's greatest width at that scale, whatever the room; 0 without strips
    double spare = 0;         // what the list can still give up before it reaches its minimum
  };

  // Captured once at pointer down; every frame of the drag reads only this.
  struct ColumnDrag {
    std::size_t column = 0;
    double startWidth = 0;
    double startX = 0;
    ColumnWidths widths;
    double spare = 0;
  };

  // Captured when the strip pane's divider is pressed, or before a key moves it: the pane's drawn width, its edge
  // beside the list, the widest it may be made, and the saved width a cancel goes back to.
  struct StripsDrag {
    double startWidth = 0;
    double startX = 0;
    double max = 0;
    std::optional<double> saved;
    double wanted = 0; // the saved width, or the default without one, held to the range: where a key step starts
    double min = kStripsMinWidth; // the pane's range at the plan's text scale
    double ceiling = kStripsMaxWidth;
  };

  // What the three columns show: `file` alone when a file is the subject, else folders in a, b and c, where b or
  // c may be a file a previous column's row points at.
  struct ColumnChain {
    NodeId a = kNoNode;
    NodeId file = kNoNode;
    NodeId b = kNoNode;
    NodeId c = kNoNode;
    bool operator==(const ColumnChain&) const = default;
  };

  struct ChainFocus {
    NodeId cursor = kNoNode;
    NodeId pointer = kNoNode; // a resting pointer wins over the cursor
    NodeId steerB = kNoNode;  // used only when it is a child of a
    NodeId steerC = kNoNode;  // used only when it is a child of b
  };

  // Each column width within 140..640 is rounded, anything else is its default; the list width is kept only when
  // it is at least 320 and under 470, then rounded.
  ColumnWidths normalizeWidths(const ColumnWidths& widths);

  // The bands, the pane's least width and its default grow with `textScale` (a value that is not finite and positive
  // reads as 1), each band rounded to whole pixels; the saved widths stay in pixels.
  // Strips for `count` ancestors in a pane `paneWidth` px wide, farthest first, with x from the pane's far edge and
  // the nearest against the list. All of them when their named bands fit, else the most named bands, at least one,
  // that fit beside one stack strip for the levels beyond them. A width outside 70 to 321 reads as the nearest
  // limit, NaN as 70.
  std::vector<Strip> ancestorStrips(std::size_t count, double paneWidth, double textScale = 1);

  // The pane's least width at a text scale; kStripsMinWidth at 1. A scale that is not finite and positive reads as 1.
  double stripsMinWidthAt(double textScale);

  // The strip pane's width in a center `viewWidth` px wide: `width`, or the default when it is none or not finite,
  // rounded and held within 70 to 321, and narrower where the list would fall under `listMin`, down to 70.
  double stripsPaneWidth(std::optional<double> width, double viewWidth, double listMin, double textScale = 1);

  // A saved strip pane width: rounded when it is from 70 to 321 after rounding, else none, the default.
  std::optional<double> normalizeStripsWidth(std::optional<double> width);

  // The strip pane width a drag or a key asks for, rounded and held within 70 and `max`, itself at most 321; a `max`
  // under 70 or NaN reads as 70. Empty for a NaN width, which changes nothing.
  std::optional<double>
  resizeStrips(double width, double max, double least = kStripsMinWidth, double ceiling = kStripsMaxWidth);

  // A drag of the strip pane's divider from `plan`, whose pane was drawn from the saved width `saved`.
  StripsDrag startStripsDrag(const ColumnPlan& plan, std::optional<double> saved);

  // One frame of the drag, or a key: the start width plus the pointer travel, where moving away from the strips
  // widens, through resizeStrips().
  std::optional<double> dragStrips(const StripsDrag& drag, double pointerX);

  // One key step: `edge` is the divider's edge moved by the step. The saved width, not the drawn one, plus the step,
  // rounded and held within 70 and 321; empty for a NaN edge.
  std::optional<double> stepStrips(const StripsDrag& drag, double edge);

  // How many columns fit in `room`: the first leaves the list `listMin`, each later one leaves it `floor`.
  std::size_t columnCount(double room, const std::array<double, kColumnsAhead>& widths, double listMin, double floor);

  // The list width a second or third column must leave: 470, or the narrower width the user chose, never under
  // listMin.
  double listFloor(const ColumnWidths& widths, double listMin);

  ColumnPlan planColumns(double viewWidth, std::size_t ancestors, const PlanOptions& options = {});

  // At most 640, and no wider than the list's minimum allows; empty for a column the plan does not draw.
  std::optional<double> maxColumnWidth(const ColumnPlan& plan, std::size_t column);

  // A column set to `width` (a key step, a reset), clamped; an undrawn column or a NaN width changes nothing.
  ColumnWidths
  resizeColumn(const ColumnWidths& widths, const ColumnPlan& plan, std::size_t column, double width, double listMin);

  // Empty for a column the plan does not draw.
  std::optional<ColumnDrag>
  startColumnDrag(const ColumnPlan& plan, std::size_t column, double pointerX, const ColumnWidths& widths);

  // One frame of a drag: the start width plus the pointer travel, where moving left widens.
  ColumnWidths dragColumn(const ColumnDrag& drag, double pointerX, double listMin);

  // Text size of a column: smaller than the list's and never under 11.
  double columnFont(double listFont, std::size_t column);

  // The list's columns in a viewport `viewport` px wide: Name, and the `chosen` columns that fit whole beside it at
  // their `widths`, each clamped to its limits. The others are left out in turn: those after Modified from the last,
  // then Type, Modified and Size. Name takes the room that is left, at least its minimum. A viewport that is not
  // finite and positive shows Name alone.
  DetailsColumns
  fitListColumns(double viewport, const DetailsColumnVisibility& chosen, const DetailsColumnWidths& widths);

  // The list's column header in a viewport `viewport` px wide. Workbench shows the columns fitListColumns() leaves
  // and takes no width drags; the other modes show the `chosen` columns at their `widths`, which may scroll sideways,
  // and List view takes drags on the header.
  struct ListHeader {
    DetailsColumns columns;
    bool resizable = false;
  };
  ListHeader
  listHeader(ViewMode mode, double viewport, const DetailsColumnVisibility& chosen, const DetailsColumnWidths& widths);

  // Whether `column` is chosen but not among the `shown` columns of a Workbench list: the fit left it out. Never
  // true in another mode, which shows every chosen column.
  bool columnLeftOut(
      ViewMode mode, const DetailsColumns& shown, const DetailsColumnVisibility& chosen, DetailsColumn column
  );

  // The current folder is tree.root. `remembered` holds the child last visited in any folder; the first one whose
  // parent is the folder is used when it is a visible folder. Without a pointer or cursor the chain follows the
  // remembered children, else the heaviest folder at each level.
  ColumnChain columnChain(
      const Tree& tree, const LayoutSettings& settings, const ChainFocus& focus, std::span<const NodeId> remembered
  );
} // namespace luciole::workbench
