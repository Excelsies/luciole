#pragma once

#include "workbench/point.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

// Ring diagram math. Angles are degrees clockwise from 12 o'clock, y grows downward, lengths are pixels.
namespace luciole::workbench::ring {
  inline constexpr double kRadian = std::numbers::pi / 180.0;
  inline constexpr double kRowHeight = 24.0;
  inline constexpr double kStackRow = 15.0;
  inline constexpr std::size_t kStackRows = 5;
  inline constexpr std::size_t kContextRings = 4;
  inline constexpr std::array<double, kContextRings> kContextThickness{22.0, 12.0, 7.0, 4.0}; // nearest first
  inline constexpr double kContextGap = 3.0;
  inline constexpr std::array<double, kContextRings> kCompactThickness{12.0, 12.0, 12.0, 5.0};
  inline constexpr double kCompactGap = 2.0;
  inline constexpr double kCompactStackRow = 14.0;
  inline constexpr double kReadoutHeight = 22.0;
  inline constexpr double kMoreDegrees = 24.0;
  inline constexpr double kCompactMoreDegrees = 30.0;
  inline constexpr double kColumnSlack = 30.0; // how far past the half turn an arc may sit in the other column
  inline constexpr double kMaxHostSize = 16384.0;

  using workbench::Point;

  struct Arc {
    double a0 = 0;
    double a1 = 0;
    bool operator==(const Arc&) const = default;
  };

  struct Band {
    double r0 = 0;
    double r1 = 0;
    bool operator==(const Band&) const = default;
  };

  struct Column {
    double x0 = 0;
    double x1 = 0;
  };

  // Metrics of one dial. A full dial reserves all four context rings; a compact one only the rings that exist.
  struct Dial {
    double width = 0;
    double height = 0;
    Point center;
    double rTotal = 0; // outer edge of the farthest context ring
    double rOut = 0;   // main ring
    double rIn = 0;
    double rHub = 0;
    double rBase = 0; // where leaders leave the dial
    Column right;     // label column, or the legend of a compact dial
    Column left;
    double yMin = 0; // bounds of row centers
    double yMax = 0;
    std::size_t rowsFit = 0;       // label rows per column
    double lane = 0;               // room for leader lanes outside rBase
    double stackBottom = 0;        // bottom of the ancestor name stack
    std::vector<double> thickness; // context rings, nearest first
    double gap = 0;
    std::size_t stackRows = 0;
    double stackRow = 0;
    double readoutHeight = 0;
    bool compact = false;
    bool legend = false;
  };

  // Main ring arcs: `angles` in angular order, the last one the "+N more" arc when `more` is set.
  // The first `split` arcs belong to the right column.
  struct ArcFit {
    std::size_t shown = 0;
    bool more = false;
    std::vector<double> angles;
    std::size_t split = 0;
  };

  // An arc of a ring: one entry, or the merged remainder of the small ones.
  struct RingSegment {
    std::size_t entry = 0; // index into the weights; unused when merged
    bool merged = false;
    Arc arc;
  };

  struct RingSplit {
    std::vector<RingSegment> segments;
    std::vector<std::size_t> merged; // entry indexes inside the merged segment
  };

  // Angle reduced to 0..360.
  double wrapDegrees(double degrees);

  Point polar(Point center, double radius, double degrees);

  // Angles for weights that sum to `total`, none under `minDegrees` while every weight can have it. Negative or
  // non-finite weights read as 0; no positive weight, or no room for the minimum, gives an equal split. A
  // non-finite or non-positive total gives zeros. At most six fixing passes.
  std::vector<double> arcAngles(std::span<const double> weights, double minDegrees, double total = 360.0);

  // Running start of each angle, beginning at `from`.
  std::vector<double> arcStarts(std::span<const double> angles, double from = 0.0);

  // Arcs for the label columns of a full dial, weights heaviest first. Beyond 2 * rowsFit entries the shown arcs
  // share 360 - moreDegrees and the "+N more" arc takes moreDegrees. The split between the columns is the most
  // balanced one that keeps each column within rowsFit rows and no arc more than `slack` past the half turn;
  // fewer arcs are shown when no split works.
  ArcFit fitCount(
      std::span<const double> weights, std::size_t rowsFit, double minDegrees, double moreDegrees = kMoreDegrees,
      double slack = kColumnSlack
  );

  // Arcs for a compact dial: the `count` heaviest (at least 1) share 360 - moreDegrees, the rest is one
  // "+N more" arc. Every arc goes to the right column.
  ArcFit fitCompact(
      std::span<const double> weights, std::size_t count, double minDegrees, double moreDegrees = kCompactMoreDegrees
  );

  // One context ring: entries under `mergeDegrees` merge into one trailing segment, never the on-path entry, and
  // the ring turns so the on-path segment is centered at 0. Empty for no positive weight or a path out of range.
  RingSplit ringSegments(std::span<const double> weights, std::size_t pathIndex, double mergeDegrees);

  // Shares of `weights` filling `span` exactly, entries under `minDegrees` merged into one trailing segment.
  // Empty for no positive weight or an empty span.
  std::vector<RingSegment> compose(std::span<const double> weights, Arc span, double minDegrees);

  // Whether `degrees` falls in `arc` of a ring whose first arc starts at `origin`.
  bool arcHolds(Arc arc, double origin, double degrees);

  // Arc under an angle, for consecutive arcs that span the full turn.
  std::optional<std::size_t> locate(std::span<const Arc> arcs, double degrees);

  // Full dial with two label columns, or none for a non-finite or non-positive host. Sizes above kMaxHostSize
  // are clamped.
  std::optional<Dial> fullDial(double width, double height);

  // Compact dial for a small host with only `rings` context rings (at most 4): names go inside the arcs, a
  // readout line sits at the bottom, and a wide host gets a legend at the right.
  std::optional<Dial> compactDial(double width, double height, std::size_t rings = kContextRings);

  // Compact below 860 by 460; back to full only at 900 by 490, so a dragged pane cannot flip the layout.
  bool chooseCompact(double width, double height, std::optional<bool> wasCompact = std::nullopt);

  // Whether `point` is inside the annular sector, at least `pad` from its edges.
  bool inSector(Point center, Arc arc, Band band, Point point, double pad = 1.5);

  // Center of a horizontal label of width by height inside a main ring arc: the middle of the arc first, then
  // four steps along the radius. None when no try fits.
  std::optional<Point> labelSpot(const Dial& dial, Arc arc, double width, double height);

  // Context ring bands outside the main ring, nearest first.
  std::vector<Band>
  ringBands(double rOut, std::span<const double> thickness = kContextThickness, double gap = kContextGap);

  // Row centers for rows in top to bottom order: the least-squares placement near `ideal` that keeps the order,
  // rows at least rowHeight apart and the bounds yMin..yMax.
  std::vector<double> placeRows(std::span<const double> ideal, double rowHeight, double yMin, double yMax);

  // Where a row wants to sit: level with its arc.
  double rowIdeal(const Dial& dial, double degrees);
} // namespace luciole::workbench::ring
