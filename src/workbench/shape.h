#pragma once

#include "workbench/point.h"
#include "workbench/tree.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// One drawable piece of a part, in part-local logical pixels with y growing downward.
namespace luciole::workbench {
  enum class ShapeKind : std::uint8_t { Rect, RoundedRect, Sector, Circle, Ribbon, Line };

  // Hatched lays diagonal stripes of the hatch color over the fill. DashedOutline draws the stroke in dashes.
  enum class ShapePattern : std::uint8_t { Solid, Hatched, DashedOutline };

  // Gamma-encoded sRGB with straight alpha.
  struct Rgba8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
    bool operator==(const Rgba8&) const = default;
  };

  struct ShapePoint {
    float x = 0;
    float y = 0;
    bool operator==(const ShapePoint&) const = default;
  };

  // The node a shape stands for, and its role among that node's shapes.
  struct ShapeId {
    NodeId node = kNoNode;
    std::uint16_t role = 0;
    bool operator==(const ShapeId&) const = default;
  };

  // Rect and RoundedRect. The radius is clamped to half the shorter side.
  struct ShapeRect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    float radius = 0;
    bool operator==(const ShapeRect&) const = default;
  };

  // Annular sector. Angles are degrees clockwise from 12 o'clock with a0 <= a1; a span of 360 or more is a whole
  // ring.
  struct ShapeSector {
    ShapePoint center;
    float r0 = 0;
    float r1 = 0;
    float a0 = 0;
    float a1 = 0;
    bool operator==(const ShapeSector&) const = default;
  };

  struct ShapeCircle {
    ShapePoint center;
    float radius = 0;
    bool operator==(const ShapeCircle&) const = default;
  };

  // Band between two cubic curves, both given left to right from the left vertical edge to the right one. The x
  // of each curve's control points should not decrease, so each curve has one y per x.
  struct ShapeRibbon {
    std::array<ShapePoint, 4> top{};
    std::array<ShapePoint, 4> bottom{};
    bool operator==(const ShapeRibbon&) const = default;
  };

  // Straight stroke with butt ends, drawn in the fill color.
  struct ShapeLine {
    ShapePoint from;
    ShapePoint to;
    float width = 0;
    bool operator==(const ShapeLine&) const = default;
  };

  // Only the geometry member that matches `kind` is read.
  struct Shape {
    ShapeId id;
    ShapeKind kind = ShapeKind::Rect;
    ShapePattern pattern = ShapePattern::Solid;
    std::int32_t order = 0; // higher draws on top; equal orders draw in list order
    float opacity = 1;
    Rgba8 fill;
    Rgba8 stroke;          // drawn inside the edge; Ribbon and Line have none
    float strokeWidth = 0; // no stroke at 0
    // Color of the Hatched stripes; the stroke color when unset.
    std::optional<Rgba8> hatch;
    // Every edge, the stroke's inner one too, fades over this many px each way instead of one device pixel, at
    // least one device pixel when positive. 0, negative or not finite is a sharp edge; Ribbon has no soft edge. The
    // fade outside the geometry takes no hits.
    float softness = 0;
    bool hittable = true;
    ShapeRect rect;
    ShapeSector sector;
    ShapeCircle circle;
    ShapeRibbon ribbon;
    ShapeLine line;
    bool operator==(const Shape&) const = default;
  };

  // Top and bottom edge of a ribbon at one x.
  struct RibbonSpan {
    float top = 0;
    float bottom = 0;
  };

  // Steps a curve search takes; float precision is reached well before.
  inline constexpr int kRibbonSearchSteps = 32;

  // Shapes from layout values, rounded to float. A rectangle with a positive radius is a RoundedRect.
  Shape rectShape(ShapeId id, double x, double y, double width, double height, double radius = 0);
  Shape sectorShape(ShapeId id, Point center, double r0, double r1, double a0, double a1);
  Shape circleShape(ShapeId id, Point center, double radius);
  Shape lineShape(ShapeId id, Point from, Point to, double width);

  // Whether the geometry of the shape's kind is finite and has positive size: a width, height, radius, band or
  // line length above 0, angles that do not run backward, and ribbon edges whose right end is right of the left.
  bool shapeValid(const Shape& shape);

  // Edges at x, from a search of at most kRibbonSearchSteps steps per curve; none outside the ribbon's x range or
  // for an invalid ribbon. A bottom above the top reads as the top.
  std::optional<RibbonSpan> ribbonSpanAt(const ShapeRibbon& ribbon, float x);

  // Whether the point lies in the shape or within `tolerance` of it. The tolerance is a distance for every kind
  // but Ribbon, where it widens the band vertically and horizontally. A tolerance that is not a finite
  // non-negative number reads as 0. Edges count as inside. An invalid shape or a non-finite point holds nothing.
  bool shapeContains(const Shape& shape, ShapePoint point, float tolerance);

  // The topmost hittable shape with opacity above 0 that holds the point: highest order, then latest in the list.
  // One pass over the shapes.
  std::optional<std::size_t> hitShape(std::span<const Shape> shapes, ShapePoint point, float tolerance);
} // namespace luciole::workbench
