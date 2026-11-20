#include "test_check.h"
#include "workbench/shape.h"
#include "workbench/shape_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

namespace {
  using namespace luciole::workbench;

  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
  constexpr float kEps = 1e-4F;

  struct Bounds {
    float x0 = std::numeric_limits<float>::infinity();
    float y0 = std::numeric_limits<float>::infinity();
    float x1 = -std::numeric_limits<float>::infinity();
    float y1 = -std::numeric_limits<float>::infinity();
  };

  Bounds bounds(std::span<const ShapeVertex> vertices) {
    Bounds b;
    for (const auto& v : vertices) {
      b.x0 = std::min(b.x0, v.position[0]);
      b.y0 = std::min(b.y0, v.position[1]);
      b.x1 = std::max(b.x1, v.position[0]);
      b.y1 = std::max(b.y1, v.position[1]);
    }
    return b;
  }

  bool allFinite(const ShapeMesh& mesh) {
    return std::ranges::all_of(mesh.vertices, [](const ShapeVertex& v) {
      const std::array<std::span<const float>, 7> parts{
          v.position, v.local, v.params, v.fill, v.stroke, v.hatch, v.style,
      };
      return std::ranges::all_of(parts, [](auto part) {
        return std::ranges::all_of(part, [](float value) { return std::isfinite(value); });
      });
    });
  }

  float cross(ShapePoint a, ShapePoint b, ShapePoint p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
  }

  bool inTriangles(std::span<const ShapeVertex> vertices, ShapePoint p) {
    for (std::size_t i = 0; i + 2 < vertices.size(); i += 3) {
      const ShapePoint a{.x = vertices[i].position[0], .y = vertices[i].position[1]};
      const ShapePoint b{.x = vertices[i + 1].position[0], .y = vertices[i + 1].position[1]};
      const ShapePoint c{.x = vertices[i + 2].position[0], .y = vertices[i + 2].position[1]};
      const float d0 = cross(a, b, p);
      const float d1 = cross(b, c, p);
      const float d2 = cross(c, a, p);
      const bool negative = d0 < -kEps || d1 < -kEps || d2 < -kEps;
      const bool positive = d0 > kEps || d1 > kEps || d2 > kEps;
      if (!(negative && positive)) {
        return true;
      }
    }
    return false;
  }

  // Every point of a 0.5 px grid that the shape holds is under a triangle of its mesh.
  void checkCovers(const Shape& shape, const ShapeMesh& mesh, float x0, float y0, float x1, float y1) {
    for (float y = y0; y <= y1; y += 0.5F) {
      for (float x = x0; x <= x1; x += 0.5F) {
        if (shapeContains(shape, {.x = x, .y = y}, 0)) {
          TEST_CHECK(inTriangles(mesh.vertices, {.x = x, .y = y}));
        }
      }
    }
  }

  ShapeMesh mesh(std::span<const Shape> shapes, float scale) {
    ShapeMesh out;
    buildShapeMesh(shapes, scale, out);
    return out;
  }

  Shape filled(ShapeKind kind) {
    Shape shape;
    shape.kind = kind;
    shape.fill = {.r = 255, .g = 51, .b = 0, .a = 255};
    shape.opacity = 0.5F;
    return shape;
  }

  void testRectangle() {
    auto shape = filled(ShapeKind::Rect);
    shape.rect = {.x = 10, .y = 20, .width = 30, .height = 40, .radius = 0};
    shape.stroke = {.r = 0, .g = 0, .b = 255, .a = 255};
    shape.strokeWidth = 2;
    shape.pattern = ShapePattern::Hatched;
    const auto out = mesh(std::span(&shape, 1), 2);
    TEST_CHECK(out.vertices.size() == 6);
    TEST_CHECK(out.drawn == 1 && out.skipped == 0 && !out.truncated);
    const auto b = bounds(out.vertices);
    TEST_NEAR(b.x0, 9.5F, kEps);
    TEST_NEAR(b.y0, 19.5F, kEps);
    TEST_NEAR(b.x1, 40.5F, kEps);
    TEST_NEAR(b.y1, 60.5F, kEps);
    for (const auto& v : out.vertices) {
      TEST_CHECK(v.style[0] == static_cast<float>(MeshKind::Box));
      TEST_CHECK(v.style[1] == static_cast<float>(ShapePattern::Hatched));
      TEST_NEAR(v.style[2], 2.0F, kEps);
      TEST_NEAR(v.style[3], 0.5F, kEps);
      TEST_NEAR(v.params[0], 15.0F, kEps);
      TEST_NEAR(v.params[1], 20.0F, kEps);
      TEST_NEAR(v.params[2], 0.0F, kEps);
      TEST_NEAR(std::abs(v.local[0]), 15.5F, kEps);
      TEST_NEAR(std::abs(v.local[1]), 20.5F, kEps);
      TEST_NEAR(v.position[0], 25.0F + v.local[0], kEps);
      TEST_NEAR(v.position[1], 40.0F + v.local[1], kEps);
      TEST_NEAR(v.fill[0], 1.0F, kEps);
      TEST_NEAR(v.fill[1], 0.2F, kEps);
      TEST_NEAR(v.fill[3], 0.5F, kEps);
      TEST_NEAR(v.stroke[2], 1.0F, kEps);
      TEST_NEAR(v.stroke[3], 0.5F, kEps);
    }
    checkCovers(shape, out, 0, 10, 50, 70);
  }

  void testRoundedRectangleAndCircle() {
    auto box = filled(ShapeKind::RoundedRect);
    box.rect = {.x = 0, .y = 0, .width = 40, .height = 20, .radius = 50};
    auto out = mesh(std::span(&box, 1), 1);
    TEST_CHECK(out.vertices.size() == 6);
    TEST_NEAR(out.vertices[0].params[2], 10.0F, kEps); // clamped to half the shorter side
    auto disc = filled(ShapeKind::Circle);
    disc.circle = {.center = {.x = 50, .y = 50}, .radius = 10};
    out = mesh(std::span(&disc, 1), 1);
    TEST_CHECK(out.vertices.size() == 6);
    const auto b = bounds(out.vertices);
    TEST_NEAR(b.x0, 39.0F, kEps);
    TEST_NEAR(b.x1, 61.0F, kEps);
    for (const auto& v : out.vertices) {
      TEST_CHECK(v.style[0] == static_cast<float>(MeshKind::Box));
      TEST_NEAR(v.params[0], 10.0F, kEps);
      TEST_NEAR(v.params[1], 10.0F, kEps);
      TEST_NEAR(v.params[2], 10.0F, kEps);
    }
    checkCovers(disc, out, 35, 35, 65, 65);
  }

  void testSector() {
    auto shape = filled(ShapeKind::Sector);
    shape.sector = {.center = {.x = 100, .y = 100}, .r0 = 20, .r1 = 40, .a0 = 0, .a1 = 90};
    const auto out = mesh(std::span(&shape, 1), 1);
    TEST_CHECK(out.vertices.size() == 6);
    const float m = std::numbers::pi_v<float> / 4; // the sector's middle, 45 degrees
    for (const auto& v : out.vertices) {
      TEST_CHECK(v.style[0] == static_cast<float>(MeshKind::Sector));
      TEST_NEAR(v.params[0], 20.0F, kEps);
      TEST_NEAR(v.params[1], 40.0F, kEps);
      TEST_NEAR(v.params[2], m, kEps);
      // The local frame points its middle up: turning it by 45 degrees gives the part position.
      const float x = v.local[0] * std::cos(m) - v.local[1] * std::sin(m);
      const float y = v.local[0] * std::sin(m) + v.local[1] * std::cos(m);
      TEST_NEAR(v.position[0], 100.0F + x, 1e-3F);
      TEST_NEAR(v.position[1], 100.0F + y, 1e-3F);
    }
    checkCovers(shape, out, 50, 50, 150, 150);
    // A full turn becomes a whole ring with a half span of pi.
    shape.sector.a1 = 400;
    const auto ring = mesh(std::span(&shape, 1), 1);
    TEST_CHECK(ring.vertices.size() == 6);
    TEST_NEAR(ring.vertices[0].params[2], std::numbers::pi_v<float>, kEps);
    checkCovers(shape, ring, 50, 50, 150, 150);
    // A sector wider than a half turn.
    shape.sector.a1 = 270;
    shape.sector.r0 = 0;
    checkCovers(shape, mesh(std::span(&shape, 1), 1), 50, 50, 150, 150);
  }

  void testRibbon() {
    auto shape = filled(ShapeKind::Ribbon);
    shape.ribbon.top = {{{.x = 0, .y = 0}, {.x = 50, .y = 0}, {.x = 50, .y = 40}, {.x = 100, .y = 40}}};
    shape.ribbon.bottom = {{{.x = 0, .y = 20}, {.x = 50, .y = 20}, {.x = 50, .y = 60}, {.x = 100, .y = 60}}};
    const auto out = mesh(std::span(&shape, 1), 1);
    // 100 px at 8 px per segment: 13 segments of three quads.
    TEST_CHECK(out.vertices.size() == 13 * 18);
    const auto b = bounds(out.vertices);
    TEST_NEAR(b.x0, 0.0F, kEps);
    TEST_NEAR(b.x1, 100.0F, kEps);
    TEST_CHECK(b.y0 < 0 && b.y0 > -1);
    TEST_CHECK(b.y1 > 60 && b.y1 < 61);
    for (const auto& v : out.vertices) {
      TEST_CHECK(v.style[0] == static_cast<float>(MeshKind::Strip));
      TEST_CHECK(v.local[0] == 0 || v.local[0] == 1);
    }
    checkCovers(shape, out, 0, -2, 100, 62);
    // The segment count stops at the cap however wide the ribbon is.
    for (auto& p : shape.ribbon.top) {
      p.x *= 10;
    }
    for (auto& p : shape.ribbon.bottom) {
      p.x *= 10;
    }
    TEST_CHECK(mesh(std::span(&shape, 1), 4).vertices.size() == kRibbonMaxSegments * 18);
    TEST_CHECK(kMaxShapeVertices == kRibbonMaxSegments * 18);
    // A ribbon thinner than a pixel keeps its middle at a fraction of full coverage.
    shape.ribbon.bottom = shape.ribbon.top;
    for (auto& p : shape.ribbon.bottom) {
      p.y += 0.25F;
    }
    const auto thin = mesh(std::span(&shape, 1), 1);
    const auto peak = std::ranges::max(thin.vertices, {}, [](const ShapeVertex& v) { return v.local[0]; });
    TEST_NEAR(peak.local[0], 0.25F, 1e-3F);
  }

  void testLine() {
    auto shape = filled(ShapeKind::Line);
    shape.line = {.from = {.x = 0, .y = 0}, .to = {.x = 100, .y = 0}, .width = 4};
    shape.strokeWidth = 3;
    auto out = mesh(std::span(&shape, 1), 2);
    TEST_CHECK(out.vertices.size() == 6);
    const auto b = bounds(out.vertices);
    TEST_NEAR(b.x0, -0.5F, kEps);
    TEST_NEAR(b.x1, 100.5F, kEps);
    TEST_NEAR(b.y0, -2.5F, kEps);
    TEST_NEAR(b.y1, 2.5F, kEps);
    TEST_NEAR(out.vertices[0].params[0], 50.0F, kEps);
    TEST_NEAR(out.vertices[0].params[1], 2.0F, kEps);
    TEST_NEAR(out.vertices[0].style[2], 0.0F, kEps); // lines have no stroke
    shape.line.to = {.x = 30, .y = 40};
    out = mesh(std::span(&shape, 1), 1);
    checkCovers(shape, out, -5, -5, 35, 45);
  }

  void testDegenerateShapesAreSkipped() {
    std::vector<Shape> shapes;
    auto add = [&shapes](ShapeKind kind, auto&& edit) {
      auto shape = filled(kind);
      edit(shape);
      shapes.push_back(shape);
    };
    add(ShapeKind::Rect, [](Shape& s) { s.rect = {.x = 0, .y = 0, .width = 0, .height = 5, .radius = 0}; });
    add(ShapeKind::Rect, [](Shape& s) { s.rect = {.x = 0, .y = 0, .width = -3, .height = 5, .radius = 0}; });
    add(ShapeKind::Rect, [](Shape& s) { s.rect = {.x = kNan, .y = 0, .width = 3, .height = 5, .radius = 0}; });
    add(ShapeKind::Circle, [](Shape& s) { s.circle = {.center = {.x = 1, .y = 1}, .radius = 0}; });
    add(ShapeKind::Sector, [](Shape& s) { s.sector = {.center = {}, .r0 = 1, .r1 = 5, .a0 = 90, .a1 = 10}; });
    add(ShapeKind::Sector, [](Shape& s) { s.sector = {.center = {}, .r0 = 5, .r1 = 5, .a0 = 0, .a1 = 10}; });
    add(ShapeKind::Line, [](Shape& s) { s.line = {.from = {.x = 2, .y = 2}, .to = {.x = 2, .y = 2}, .width = 1}; });
    add(ShapeKind::Ribbon, [](Shape& s) { s.ribbon.top[0].x = kNan; });
    add(ShapeKind::Rect, [](Shape& s) {
      s.rect = {.x = 0, .y = 0, .width = 3, .height = 5, .radius = 0};
      s.opacity = 0;
    });
    add(ShapeKind::Rect, [](Shape& s) {
      s.rect = {.x = 0, .y = 0, .width = 3, .height = 5, .radius = 0};
      s.fill.a = 0;
    });
    const auto out = mesh(shapes, 1);
    TEST_CHECK(out.vertices.empty());
    TEST_CHECK(out.skipped == shapes.size());
    // A negative inner radius is clamped to 0 and an opacity over 1 to 1.
    auto sector = filled(ShapeKind::Sector);
    sector.sector = {.center = {}, .r0 = -4, .r1 = 5, .a0 = 0, .a1 = 10};
    sector.opacity = 3;
    const auto clamped = mesh(std::span(&sector, 1), 1);
    TEST_CHECK(clamped.vertices.size() == 6);
    TEST_NEAR(clamped.vertices[0].params[0], 0.0F, kEps);
    TEST_NEAR(clamped.vertices[0].fill[3], 1.0F, kEps);
    TEST_CHECK(allFinite(clamped));
    // A ribbon one float step wide still meshes to finite vertices.
    for (const float left : {100.0F, 1e6F}) {
      const float right = std::nextafter(left, 2 * left);
      auto thin = filled(ShapeKind::Ribbon);
      thin.ribbon.top = {{{.x = left, .y = 0}, {.x = left, .y = 0}, {.x = right, .y = 0}, {.x = right, .y = 0}}};
      thin.ribbon.bottom = {{{.x = left, .y = 5}, {.x = left, .y = 5}, {.x = right, .y = 5}, {.x = right, .y = 5}}};
      TEST_CHECK(shapeValid(thin));
      for (const float scale : {1.0F, 2.0F}) {
        const auto thinMesh = mesh(std::span(&thin, 1), scale);
        TEST_CHECK(!thinMesh.vertices.empty());
        TEST_CHECK(allFinite(thinMesh));
      }
    }
    // A pixel scale that is not positive and finite gives nothing.
    TEST_CHECK(mesh(std::span(&sector, 1), 0).vertices.empty());
    TEST_CHECK(mesh(std::span(&sector, 1), kNan).vertices.empty());
  }

  void testCapAndOrderAndDeterminism() {
    std::vector<Shape> shapes(11000, filled(ShapeKind::Rect));
    for (std::size_t i = 0; i < shapes.size(); ++i) {
      shapes[i].rect = {.x = static_cast<float>(i % 100), .y = 0, .width = 1, .height = 1, .radius = 0};
    }
    const auto out = mesh(shapes, 1.5F);
    TEST_CHECK(out.truncated);
    TEST_CHECK(out.vertices.size() <= kMaxMeshVertices);
    TEST_CHECK(out.drawn == kMaxMeshVertices / 6);
    TEST_CHECK(allFinite(out));
    // Equal input gives equal output, value for value.
    const auto again = mesh(shapes, 1.5F);
    TEST_CHECK(again.vertices.size() == out.vertices.size());
    for (std::size_t i = 0; i < out.vertices.size(); ++i) {
      const auto& a = out.vertices[i];
      const auto& b = again.vertices[i];
      TEST_CHECK(a.position == b.position && a.local == b.local && a.params == b.params);
      TEST_CHECK(a.fill == b.fill && a.stroke == b.stroke && a.hatch == b.hatch && a.style == b.style);
    }
    // Higher orders come later; equal orders keep list order.
    std::vector<Shape> layered(3, filled(ShapeKind::Rect));
    for (std::size_t i = 0; i < layered.size(); ++i) {
      layered[i].rect = {.x = static_cast<float>(i * 10), .y = 0, .width = 5, .height = 5, .radius = 0};
    }
    layered[0].order = 2;
    const auto ordered = mesh(layered, 1);
    TEST_CHECK(ordered.vertices.size() == 18);
    const auto centerX = [&ordered](std::size_t vertex) {
      return ordered.vertices[vertex].position[0] - ordered.vertices[vertex].local[0];
    };
    TEST_NEAR(centerX(0), 12.5F, kEps);
    TEST_NEAR(centerX(6), 22.5F, kEps);
    TEST_NEAR(centerX(12), 2.5F, kEps);
  }
  // Stripes take the hatch color when one is set and the stroke color otherwise.
  void testHatchColor() {
    auto shape = filled(ShapeKind::Rect);
    shape.rect = {.x = 0, .y = 0, .width = 10, .height = 10, .radius = 0};
    shape.stroke = {.r = 0, .g = 0, .b = 255, .a = 255};
    shape.pattern = ShapePattern::Hatched;
    auto out = mesh(std::span(&shape, 1), 1);
    TEST_CHECK(out.vertices.size() == 6);
    TEST_NEAR(out.vertices[0].hatch[2], 1.0F, kEps);
    TEST_NEAR(out.vertices[0].hatch[3], 0.5F, kEps); // times the shape's opacity
    shape.hatch = Rgba8{.r = 51, .g = 102, .b = 0, .a = 204};
    out = mesh(std::span(&shape, 1), 1);
    for (const auto& v : out.vertices) {
      TEST_NEAR(v.hatch[0], 0.2F, kEps);
      TEST_NEAR(v.hatch[1], 0.4F, kEps);
      TEST_NEAR(v.hatch[2], 0.0F, kEps);
      TEST_NEAR(v.hatch[3], 0.4F, kEps);
      TEST_NEAR(v.stroke[2], 1.0F, kEps);
    }
    // Stripes in a hatch color show on a shape with no fill and no stroke.
    shape.fill.a = 0;
    shape.stroke.a = 0;
    TEST_CHECK(mesh(std::span(&shape, 1), 1).drawn == 1);
  }

  // A soft shape carries its softness, at least one device pixel, and its quad grows by it.
  void testSoftness() {
    auto shape = filled(ShapeKind::RoundedRect);
    shape.rect = {.x = 10, .y = 20, .width = 30, .height = 40, .radius = 4};
    shape.softness = 6;
    auto out = mesh(std::span(&shape, 1), 2);
    TEST_CHECK(out.vertices.size() == 6);
    auto b = bounds(out.vertices);
    TEST_NEAR(b.x0, 3.5F, kEps);
    TEST_NEAR(b.y1, 66.5F, kEps);
    for (const auto& v : out.vertices) {
      TEST_NEAR(v.params[3], 6.0F, kEps);
    }
    shape.softness = 0.1F;
    TEST_NEAR(mesh(std::span(&shape, 1), 2).vertices[0].params[3], 0.5F, kEps);
    for (const float bad : {0.0F, -3.0F, kNan, std::numeric_limits<float>::infinity()}) {
      shape.softness = bad;
      out = mesh(std::span(&shape, 1), 1);
      TEST_CHECK(out.vertices.size() == 6);
      TEST_NEAR(out.vertices[0].params[3], 0.0F, kEps);
      b = bounds(out.vertices);
      TEST_NEAR(b.x0, 9.0F, kEps);
    }
    auto sector = filled(ShapeKind::Sector);
    sector.sector = {.center = {.x = 100, .y = 100}, .r0 = 20, .r1 = 40, .a0 = 0, .a1 = 360};
    sector.softness = 5;
    out = mesh(std::span(&sector, 1), 1);
    b = bounds(out.vertices);
    TEST_NEAR(b.x0, 54.0F, kEps);
    TEST_NEAR(out.vertices[0].params[3], 5.0F, kEps);
    // A ribbon has no soft edge.
    auto band = filled(ShapeKind::Ribbon);
    band.ribbon.top = {{{.x = 0, .y = 0}, {.x = 50, .y = 0}, {.x = 50, .y = 0}, {.x = 100, .y = 0}}};
    band.ribbon.bottom = {{{.x = 0, .y = 20}, {.x = 50, .y = 20}, {.x = 50, .y = 20}, {.x = 100, .y = 20}}};
    band.softness = 5;
    out = mesh(std::span(&band, 1), 1);
    TEST_CHECK(std::ranges::all_of(out.vertices, [](const ShapeVertex& v) { return v.params[3] == 0; }));
    TEST_CHECK(bounds(out.vertices).y0 > -1);
  }
} // namespace

int main() {
  testRectangle();
  testRoundedRectangleAndCircle();
  testSector();
  testRibbon();
  testLine();
  testDegenerateShapesAreSkipped();
  testCapAndOrderAndDeterminism();
  testHatchColor();
  testSoftness();
  return 0;
}
