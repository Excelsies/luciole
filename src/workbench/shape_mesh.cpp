#include "workbench/shape_mesh.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace luciole::workbench {
  namespace {
    constexpr float kPi = std::numbers::pi_v<float>;
    constexpr float kRadian = kPi / 180.0F;
    constexpr float kMaxSlopeStretch = 8; // a fringe measured vertically grows at most this much on steep edges

    // Attribute values every vertex of one shape shares.
    struct Common {
      std::array<float, 4> params{};
      std::array<float, 4> fill{};
      std::array<float, 4> stroke{};
      std::array<float, 4> hatch{};
      std::array<float, 4> style{};
    };

    // Origin and turn of a shape's local frame in the part.
    struct Frame {
      float x = 0;
      float y = 0;
      float cos = 1;
      float sin = 0;
    };

    std::array<float, 4> unitColor(Rgba8 color, float opacity) {
      return {color.r / 255.0F, color.g / 255.0F, color.b / 255.0F, color.a / 255.0F * opacity};
    }

    bool visible(const Shape& shape) {
      if (shape.kind == ShapeKind::Line) {
        return shape.fill.a > 0;
      }
      const bool stroked = std::isfinite(shape.strokeWidth) && shape.strokeWidth > 0;
      const bool hatched = shape.pattern == ShapePattern::Hatched && shape.hatch.value_or(shape.stroke).a > 0;
      return shape.fill.a > 0 || (shape.stroke.a > 0 && stroked) || hatched;
    }

    // The softness the shader reads: at least one device pixel, 0 for a sharp edge.
    float softness(const Shape& shape, float aa) {
      const float soft = shape.softness;
      return std::isfinite(soft) && soft > 0 && shape.kind != ShapeKind::Ribbon ? std::max(soft, aa) : 0.0F;
    }

    std::size_t ribbonSegments(const ShapeRibbon& ribbon, float scale) {
      const float left = std::max(ribbon.top[0].x, ribbon.bottom[0].x);
      const float right = std::min(ribbon.top[3].x, ribbon.bottom[3].x);
      const float wanted = std::ceil((right - left) * scale / kRibbonSegmentPixels);
      return std::isfinite(wanted) ? static_cast<std::size_t>(std::clamp(wanted, 2.0F, float{kRibbonMaxSegments}))
                                   : kRibbonMaxSegments;
    }

    void pushVertex(std::vector<ShapeVertex>& out, const Common& common, float x, float y, float u, float v) {
      out.push_back(
          {.position = {x, y},
           .local = {u, v},
           .params = common.params,
           .fill = common.fill,
           .stroke = common.stroke,
           .hatch = common.hatch,
           .style = common.style}
      );
    }

    // Quad over local [lx0, lx1] x [ly0, ly1], as two triangles.
    void pushQuad(
        std::vector<ShapeVertex>& out, const Common& common, const Frame& f, float lx0, float ly0, float lx1, float ly1
    ) {
      const auto corner = [&](float lx, float ly) {
        pushVertex(out, common, f.x + lx * f.cos - ly * f.sin, f.y + lx * f.sin + ly * f.cos, lx, ly);
      };
      corner(lx0, ly0);
      corner(lx1, ly0);
      corner(lx1, ly1);
      corner(lx0, ly0);
      corner(lx1, ly1);
      corner(lx0, ly1);
    }

    void pushBox(std::vector<ShapeVertex>& out, Common common, const Frame& frame, float hw, float hh, float radius) {
      const float soft = common.params[3];
      const float grow = common.style[3] + soft;
      common.params = {hw, hh, radius, soft};
      pushQuad(out, common, frame, -hw - grow, -hh - grow, hw + grow, hh + grow);
    }

    void pushSector(std::vector<ShapeVertex>& out, Common common, const ShapeSector& sector) {
      const float soft = common.params[3];
      const float grow = common.style[3] + soft;
      const float r0 = std::max(sector.r0, 0.0F);
      const float r1 = sector.r1;
      const float span = std::min(sector.a1 - sector.a0, 360.0F);
      const float half = span >= 360.0F ? kPi : span / 2 * kRadian;
      const float middle = (sector.a0 + span / 2) * kRadian;
      const Frame frame{.x = sector.center.x, .y = sector.center.y, .cos = std::cos(middle), .sin = std::sin(middle)};
      const float side = half >= kPi / 2 ? r1 : r1 * std::sin(half);
      const float bottom = std::max(-r1 * std::cos(half), -r0 * std::cos(half));
      common.params = {r0, r1, half, soft};
      pushQuad(out, common, frame, -side - grow, -r1 - grow, side + grow, bottom + grow);
    }

    struct Column {
      float x = 0;
      std::array<float, 4> y{}; // outer top, inner top, inner bottom, outer bottom
      float body = 1;           // coverage on the inner edges
    };

    // Columns that round to one x have no slope to measure; they take the widest fringe.
    float slopeStretch(float dy, float dx) {
      if (!(dx > 0)) {
        return kMaxSlopeStretch;
      }
      return std::min(std::sqrt(1 + (dy / dx) * (dy / dx)), kMaxSlopeStretch);
    }

    void pushRibbon(std::vector<ShapeVertex>& out, Common common, const ShapeRibbon& ribbon, std::size_t segments) {
      const float aa = common.style[3];
      const float left = std::max(ribbon.top[0].x, ribbon.bottom[0].x);
      const float right = std::min(ribbon.top[3].x, ribbon.bottom[3].x);
      std::array<RibbonSpan, kRibbonMaxSegments + 1> spans{};
      std::array<float, kRibbonMaxSegments + 1> xs{};
      for (std::size_t k = 0; k <= segments; ++k) {
        xs[k] = k == segments ? right : left + (right - left) * static_cast<float>(k) / static_cast<float>(segments);
        spans[k] = ribbonSpanAt(ribbon, xs[k]).value_or(RibbonSpan{});
      }
      std::array<Column, kRibbonMaxSegments + 1> columns{};
      for (std::size_t k = 0; k <= segments; ++k) {
        const std::size_t a = k == 0 ? 0 : k - 1;
        const std::size_t b = k == segments ? segments : k + 1;
        const float dx = xs[b] - xs[a];
        const float fringeTop = aa / 2 * slopeStretch(spans[b].top - spans[a].top, dx);
        const float fringeBottom = aa / 2 * slopeStretch(spans[b].bottom - spans[a].bottom, dx);
        const float top = spans[k].top;
        const float bottom = spans[k].bottom;
        auto& column = columns[k];
        column.x = xs[k];
        if (bottom - top >= fringeTop + fringeBottom) {
          column.y = {top - fringeTop, top + fringeTop, bottom - fringeBottom, bottom + fringeBottom};
        } else {
          const float middle = (top + bottom) / 2;
          column.y = {top - fringeTop, middle, middle, bottom + fringeBottom};
          column.body = std::clamp((bottom - top) / aa, 0.0F, 1.0F);
        }
      }
      common.params = {};
      common.style[2] = 0;
      for (std::size_t k = 0; k < segments; ++k) {
        const auto& c0 = columns[k];
        const auto& c1 = columns[k + 1];
        for (std::size_t band = 0; band < 3; ++band) {
          const float u0 = band == 0 ? 0 : c0.body;
          const float u1 = band == 2 ? 0 : c0.body;
          const float w0 = band == 0 ? 0 : c1.body;
          const float w1 = band == 2 ? 0 : c1.body;
          pushVertex(out, common, c0.x, c0.y[band], u0, 0);
          pushVertex(out, common, c1.x, c1.y[band], w0, 0);
          pushVertex(out, common, c1.x, c1.y[band + 1], w1, 0);
          pushVertex(out, common, c0.x, c0.y[band], u0, 0);
          pushVertex(out, common, c1.x, c1.y[band + 1], w1, 0);
          pushVertex(out, common, c0.x, c0.y[band + 1], u1, 0);
        }
      }
    }

    void pushShape(std::vector<ShapeVertex>& out, const Shape& shape, float aa, std::size_t segments) {
      const float opacity = std::min(shape.opacity, 1.0F);
      const bool stroked = std::isfinite(shape.strokeWidth) && shape.strokeWidth > 0;
      Common common{
          .params = {0, 0, 0, softness(shape, aa)},
          .fill = unitColor(shape.fill, opacity),
          .stroke = unitColor(shape.stroke, opacity),
          .hatch = unitColor(shape.hatch.value_or(shape.stroke), opacity),
          .style = {0, static_cast<float>(shape.pattern), stroked ? shape.strokeWidth : 0.0F, aa},
      };
      switch (shape.kind) {
      case ShapeKind::Rect:
      case ShapeKind::RoundedRect: {
        const auto& r = shape.rect;
        const float hw = r.width / 2;
        const float hh = r.height / 2;
        const float radius = shape.kind == ShapeKind::Rect ? 0.0F : std::clamp(r.radius, 0.0F, std::min(hw, hh));
        pushBox(out, common, {.x = r.x + hw, .y = r.y + hh}, hw, hh, radius);
        break;
      }
      case ShapeKind::Circle: {
        const auto& c = shape.circle;
        pushBox(out, common, {.x = c.center.x, .y = c.center.y}, c.radius, c.radius, c.radius);
        break;
      }
      case ShapeKind::Line: {
        const auto& l = shape.line;
        const float dx = l.to.x - l.from.x;
        const float dy = l.to.y - l.from.y;
        const float length = std::hypot(dx, dy);
        common.style[2] = 0;
        const Frame frame{.x = l.from.x + dx / 2, .y = l.from.y + dy / 2, .cos = dx / length, .sin = dy / length};
        pushBox(out, common, frame, length / 2, l.width / 2, 0);
        break;
      }
      case ShapeKind::Sector:
        common.style[0] = static_cast<float>(MeshKind::Sector);
        pushSector(out, common, shape.sector);
        break;
      case ShapeKind::Ribbon:
        common.style[0] = static_cast<float>(MeshKind::Strip);
        pushRibbon(out, common, shape.ribbon, segments);
        break;
      }
    }
  } // namespace

  void buildShapeMesh(std::span<const Shape> shapes, float pixelScale, ShapeMesh& mesh) {
    mesh.vertices.clear();
    mesh.drawn = 0;
    mesh.skipped = 0;
    mesh.truncated = false;
    if (!std::isfinite(pixelScale) || pixelScale <= 0) {
      return;
    }
    const float aa = 1.0F / pixelScale;
    std::vector<std::size_t> order(shapes.size());
    std::ranges::iota(order, std::size_t{0});
    std::ranges::stable_sort(order, {}, [&shapes](std::size_t i) { return shapes[i].order; });
    for (const auto index : order) {
      const auto& shape = shapes[index];
      if (!shapeValid(shape) || !(shape.opacity > 0) || !visible(shape)) {
        ++mesh.skipped;
        continue;
      }
      const std::size_t segments = shape.kind == ShapeKind::Ribbon ? ribbonSegments(shape.ribbon, pixelScale) : 0;
      const std::size_t count = shape.kind == ShapeKind::Ribbon ? segments * 3 * kQuadVertices : kQuadVertices;
      if (mesh.vertices.size() + count > kMaxMeshVertices) {
        mesh.truncated = true;
        break;
      }
      pushShape(mesh.vertices, shape, aa, segments);
      ++mesh.drawn;
    }
  }
} // namespace luciole::workbench
