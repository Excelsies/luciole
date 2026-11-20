#include "workbench/shape.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <numbers>

namespace luciole::workbench {
  namespace {
    constexpr double kRadian = std::numbers::pi / 180.0;
    constexpr double kEdgeSlack = 1e-4; // rounding room so points on an edge count as inside

    float f(double value) { return static_cast<float>(value); }

    ShapePoint point(Point p) { return {.x = f(p.x), .y = f(p.y)}; }

    bool finite(ShapePoint point) { return std::isfinite(point.x) && std::isfinite(point.y); }

    bool finiteAll(std::initializer_list<float> values) {
      return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
    }

    double cubic(double p0, double p1, double p2, double p3, double t) {
      const double u = 1.0 - t;
      return u * u * u * p0 + 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t * p3;
    }

    // The curve's y where its x reaches `x`, by bisection on the curve parameter.
    double curveY(const std::array<ShapePoint, 4>& c, double x) {
      double lo = 0;
      double hi = 1;
      for (int step = 0; step < kRibbonSearchSteps; ++step) {
        const double t = (lo + hi) / 2;
        (cubic(c[0].x, c[1].x, c[2].x, c[3].x, t) < x ? lo : hi) = t;
      }
      return cubic(c[0].y, c[1].y, c[2].y, c[3].y, (lo + hi) / 2);
    }

    double ribbonLeft(const ShapeRibbon& ribbon) { return std::max(ribbon.top[0].x, ribbon.bottom[0].x); }

    double ribbonRight(const ShapeRibbon& ribbon) { return std::min(ribbon.top[3].x, ribbon.bottom[3].x); }

    // Signed distance from (px, py), relative to a box center, to a box of half sizes hw, hh and corner radius r.
    double boxDistance(double px, double py, double hw, double hh, double r) {
      const double qx = std::abs(px) - hw + r;
      const double qy = std::abs(py) - hh + r;
      return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
    }

    double rectDistance(const ShapeRect& rect, bool rounded, ShapePoint point) {
      const double hw = rect.width / 2.0;
      const double hh = rect.height / 2.0;
      const double r = rounded ? std::clamp(static_cast<double>(rect.radius), 0.0, std::min(hw, hh)) : 0.0;
      return boxDistance(point.x - (rect.x + hw), point.y - (rect.y + hh), hw, hh, r);
    }

    double sectorDistance(const ShapeSector& sector, ShapePoint point) {
      const double dx = static_cast<double>(point.x) - sector.center.x;
      const double dy = static_cast<double>(point.y) - sector.center.y;
      const double r0 = std::max(static_cast<double>(sector.r0), 0.0);
      const double r1 = sector.r1;
      const double rho = std::hypot(dx, dy);
      const double radial = std::max(r0 - rho, rho - r1);
      const double span = static_cast<double>(sector.a1) - sector.a0;
      const double rel = std::fmod(std::fmod(std::atan2(dx, -dy) / kRadian - sector.a0, 360.0) + 360.0, 360.0);
      if (span >= 360.0 || rel <= span) {
        return radial;
      }
      // Outside the wedge the nearest point lies on one of the two straight edges.
      double best = std::numeric_limits<double>::infinity();
      for (const double degrees : {static_cast<double>(sector.a0), static_cast<double>(sector.a1)}) {
        const double ux = std::sin(degrees * kRadian);
        const double uy = -std::cos(degrees * kRadian);
        const double along = std::clamp(dx * ux + dy * uy, r0, r1);
        best = std::min(best, std::hypot(dx - along * ux, dy - along * uy));
      }
      return best;
    }

    double lineDistance(const ShapeLine& line, ShapePoint point) {
      const double dx = static_cast<double>(line.to.x) - line.from.x;
      const double dy = static_cast<double>(line.to.y) - line.from.y;
      const double length = std::hypot(dx, dy);
      const double px = static_cast<double>(point.x) - (line.from.x + dx / 2);
      const double py = static_cast<double>(point.y) - (line.from.y + dy / 2);
      const double along = (px * dx + py * dy) / length;
      const double across = (py * dx - px * dy) / length;
      return boxDistance(along, across, length / 2, line.width / 2.0, 0.0);
    }

    bool ribbonContains(const ShapeRibbon& ribbon, ShapePoint point, double tolerance) {
      const double left = ribbonLeft(ribbon);
      const double right = ribbonRight(ribbon);
      if (point.x < left - tolerance || point.x > right + tolerance) {
        return false;
      }
      const auto span = ribbonSpanAt(ribbon, static_cast<float>(std::clamp(static_cast<double>(point.x), left, right)));
      return span && point.y >= span->top - tolerance && point.y <= span->bottom + tolerance;
    }
  } // namespace

  Shape rectShape(ShapeId id, double x, double y, double width, double height, double radius) {
    Shape shape;
    shape.id = id;
    shape.kind = radius > 0 ? ShapeKind::RoundedRect : ShapeKind::Rect;
    shape.rect = {.x = f(x), .y = f(y), .width = f(width), .height = f(height), .radius = f(radius)};
    return shape;
  }

  Shape sectorShape(ShapeId id, Point center, double r0, double r1, double a0, double a1) {
    Shape shape;
    shape.id = id;
    shape.kind = ShapeKind::Sector;
    shape.sector = {.center = point(center), .r0 = f(r0), .r1 = f(r1), .a0 = f(a0), .a1 = f(a1)};
    return shape;
  }

  Shape circleShape(ShapeId id, Point center, double radius) {
    Shape shape;
    shape.id = id;
    shape.kind = ShapeKind::Circle;
    shape.circle = {.center = point(center), .radius = f(radius)};
    return shape;
  }

  Shape lineShape(ShapeId id, Point from, Point to, double width) {
    Shape shape;
    shape.id = id;
    shape.kind = ShapeKind::Line;
    shape.line = {.from = point(from), .to = point(to), .width = f(width)};
    return shape;
  }

  bool shapeValid(const Shape& shape) {
    switch (shape.kind) {
    case ShapeKind::Rect:
    case ShapeKind::RoundedRect: {
      const auto& r = shape.rect;
      return finiteAll({r.x, r.y, r.width, r.height, r.radius}) && r.width > 0 && r.height > 0;
    }
    case ShapeKind::Sector: {
      const auto& s = shape.sector;
      return finite(s.center) && finiteAll({s.r0, s.r1, s.a0, s.a1}) && s.r1 > 0 && s.r1 > s.r0 && s.a1 > s.a0;
    }
    case ShapeKind::Circle:
      return finite(shape.circle.center) && std::isfinite(shape.circle.radius) && shape.circle.radius > 0;
    case ShapeKind::Ribbon: {
      const auto& r = shape.ribbon;
      return std::ranges::all_of(r.top, finite)
          && std::ranges::all_of(r.bottom, finite)
          && ribbonRight(r) > ribbonLeft(r);
    }
    case ShapeKind::Line: {
      const auto& l = shape.line;
      return finite(l.from) && finite(l.to) && std::isfinite(l.width) && l.width > 0 && l.from != l.to;
    }
    }
    return false;
  }

  std::optional<RibbonSpan> ribbonSpanAt(const ShapeRibbon& ribbon, float x) {
    Shape probe;
    probe.kind = ShapeKind::Ribbon;
    probe.ribbon = ribbon;
    if (!std::isfinite(x) || !shapeValid(probe) || x < ribbonLeft(ribbon) || x > ribbonRight(ribbon)) {
      return std::nullopt;
    }
    const double top = curveY(ribbon.top, x);
    const double bottom = std::max(top, curveY(ribbon.bottom, x));
    return RibbonSpan{.top = static_cast<float>(top), .bottom = static_cast<float>(bottom)};
  }

  bool shapeContains(const Shape& shape, ShapePoint point, float tolerance) {
    if (!finite(point) || !shapeValid(shape)) {
      return false;
    }
    const double reach = (std::isfinite(tolerance) && tolerance > 0 ? tolerance : 0.0) + kEdgeSlack;
    switch (shape.kind) {
    case ShapeKind::Rect:
    case ShapeKind::RoundedRect:
      return rectDistance(shape.rect, shape.kind == ShapeKind::RoundedRect, point) <= reach;
    case ShapeKind::Sector:
      return sectorDistance(shape.sector, point) <= reach;
    case ShapeKind::Circle:
      return std::hypot(point.x - shape.circle.center.x, point.y - shape.circle.center.y)
          <= shape.circle.radius + reach;
    case ShapeKind::Ribbon:
      return ribbonContains(shape.ribbon, point, reach);
    case ShapeKind::Line:
      return lineDistance(shape.line, point) <= reach;
    }
    return false;
  }

  std::optional<std::size_t> hitShape(std::span<const Shape> shapes, ShapePoint point, float tolerance) {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < shapes.size(); ++i) {
      const auto& shape = shapes[i];
      if (!shape.hittable || !(shape.opacity > 0) || (best && shape.order < shapes[*best].order)) {
        continue;
      }
      if (shapeContains(shape, point, tolerance)) {
        best = i;
      }
    }
    return best;
  }
} // namespace luciole::workbench
