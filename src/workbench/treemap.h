#pragma once

#include "workbench/tree.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace luciole::workbench {
  struct TreemapRect {
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
    bool operator==(const TreemapRect&) const = default;
  };

  struct TreemapPoint {
    double x = 0;
    double y = 0;
    bool operator==(const TreemapPoint&) const = default;
  };

  // `rect` moved in by `by` on every side; a negative `by` grows it.
  inline TreemapRect inset(const TreemapRect& rect, double by) {
    return {.x = rect.x + by, .y = rect.y + by, .w = rect.w - 2 * by, .h = rect.h - 2 * by};
  }

  // Squarified treemap rows. Weights are heaviest first; the rectangles come back in the same order and tile `rect`.
  // Empty when there are no weights, a weight is not positive and finite, `rect` has no finite positive area, or the
  // area per unit of weight is not finite and positive.
  std::vector<TreemapRect> squarify(std::span<const double> weights, TreemapRect rect);

  inline constexpr std::size_t kBandPieces = 8;

  // A band of `thickness` inside `rect`, measured along its centerline: a rounded rectangle that starts at the left
  // end of the top side and runs clockwise. Pieces: 0 top, 1 top right arc, 2 right, 3 bottom right arc, 4 bottom,
  // 5 bottom left arc, 6 left, 7 top left arc. Even pieces are straight.
  struct Band {
    TreemapRect rect;
    double thickness = 0;
    double cornerRadius = 0;  // radius of the centerline arcs
    double halfStraightX = 0; // half the length of the top and bottom sides
    double halfStraightY = 0; // half the length of the left and right sides
    double centerX = 0;
    double centerY = 0;
    std::array<double, kBandPieces> lengths{};
    std::array<double, kBandPieces> starts{};
    double perimeter = 0;
  };

  // A point on the centerline with the outward unit normal.
  struct BandPoint {
    double x = 0;
    double y = 0;
    double normalX = 0;
    double normalY = 0;
  };

  // None unless the rectangle, thickness and radius are finite, and width, height and thickness are positive.
  std::optional<Band> band(TreemapRect rect, double thickness, double outerRadius);

  // Piece holding distance `s`; distances before the start read as piece 0, NaN as piece 7.
  std::size_t bandPiece(const Band& band, double s);

  // Point at distance `s` along the centerline, wrapped to the perimeter. A non-finite `s` or an empty perimeter
  // reads as distance 0.
  BandPoint pointAt(const Band& band, double s);

  // Distance along the centerline of the band point under (x, y); none when the point is not on the band or the band
  // has no perimeter.
  std::optional<double> locate(const Band& band, double x, double y);

  // Outline between distances s0 < s1: outer edge forward, then inner edge back. Arcs are sampled in at most 64
  // steps each. Empty for a reversed, empty or non-finite stretch.
  std::vector<TreemapPoint> segmentPolygon(const Band& band, double s0, double s1);

  struct BandItem {
    NodeId key = kNoNode;
    double share = 0;
  };

  struct PartitionOptions {
    double minLength = 0;
    std::optional<NodeId> pinKey; // keeps its own segment of at least max(pinMinimum, minLength)
    double pinMinimum = 0;
  };

  // One stretch [s0, s1] of the band: one entry, or the merged tail of entries shorter than the minimum.
  struct BandSegment {
    double s0 = 0;
    double s1 = 0;
    NodeId key = kNoNode;
    bool merged = false;
    std::vector<NodeId> keys; // merged entries, in item order
  };

  // Splits `length` among items (heaviest first) by share. Every item gets at least minLength; items that would be
  // shorter merge into one trailing segment, except the pinned key. A single short item is not merged. Segments
  // cover [0, length] exactly once; empty for no items or a length that is not positive and finite.
  std::vector<BandSegment> partition(std::span<const BandItem> items, double length, const PartitionOptions& options);

  // Moves each inner boundary that falls inside a corner arc to the nearer end of that arc, or else the farther one,
  // when both neighbors keep at least minLength. Coverage stays exact.
  std::vector<BandSegment> snapToStraights(const Band& band, std::vector<BandSegment> segments, double minLength);
} // namespace luciole::workbench
