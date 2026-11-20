#include "test_check.h"
#include "workbench/shape.h"

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace {
  using namespace luciole::workbench;

  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
  constexpr float kInf = std::numeric_limits<float>::infinity();

  Shape rect(float x, float y, float width, float height) {
    Shape shape;
    shape.kind = ShapeKind::Rect;
    shape.rect = {.x = x, .y = y, .width = width, .height = height, .radius = 0};
    return shape;
  }

  Shape rounded(float x, float y, float width, float height, float radius) {
    auto shape = rect(x, y, width, height);
    shape.kind = ShapeKind::RoundedRect;
    shape.rect.radius = radius;
    return shape;
  }

  Shape circle(float x, float y, float radius) {
    Shape shape;
    shape.kind = ShapeKind::Circle;
    shape.circle = {.center = {.x = x, .y = y}, .radius = radius};
    return shape;
  }

  // Annular sector around (100, 100); angles clockwise from 12 o'clock.
  Shape sector(float r0, float r1, float a0, float a1) {
    Shape shape;
    shape.kind = ShapeKind::Sector;
    shape.sector = {.center = {.x = 100, .y = 100}, .r0 = r0, .r1 = r1, .a0 = a0, .a1 = a1};
    return shape;
  }

  // From x 0 to 100: the top edge falls from y 0 to y 40, the bottom from y 20 to y 60, both eased.
  Shape ribbon() {
    Shape shape;
    shape.kind = ShapeKind::Ribbon;
    shape.ribbon.top = {{{.x = 0, .y = 0}, {.x = 50, .y = 0}, {.x = 50, .y = 40}, {.x = 100, .y = 40}}};
    shape.ribbon.bottom = {{{.x = 0, .y = 20}, {.x = 50, .y = 20}, {.x = 50, .y = 60}, {.x = 100, .y = 60}}};
    return shape;
  }

  Shape line(ShapePoint from, ShapePoint to, float width) {
    Shape shape;
    shape.kind = ShapeKind::Line;
    shape.line = {.from = from, .to = to, .width = width};
    return shape;
  }

  bool at(const Shape& shape, float x, float y, float tolerance = 0) {
    return shapeContains(shape, {.x = x, .y = y}, tolerance);
  }

  void testRectangleEdgesAndTolerance() {
    const auto box = rect(10, 20, 30, 40);
    TEST_CHECK(at(box, 25, 40));
    TEST_CHECK(at(box, 10, 20));
    TEST_CHECK(at(box, 40, 60));
    TEST_CHECK(!at(box, 9.9F, 30));
    TEST_CHECK(!at(box, 40.1F, 30));
    TEST_CHECK(at(box, 8.5F, 30, 1.5F));
    TEST_CHECK(!at(box, 8.4F, 30, 1.5F));
    // Beyond a corner the tolerance is a distance: (7, 17) is 4.24 px from (10, 20).
    TEST_CHECK(!at(box, 7, 17, 4));
    TEST_CHECK(at(box, 7, 17, 4.3F));
  }

  void testRoundedRectangleCorners() {
    const auto box = rounded(0, 0, 40, 20, 10);
    TEST_CHECK(at(box, 20, 10));
    TEST_CHECK(at(box, 0, 10));
    // The corner arc centers on (10, 10): (3, 3) is 9.9 px away, (2, 2) is 11.3 px.
    TEST_CHECK(at(box, 3, 3));
    TEST_CHECK(!at(box, 2, 2));
    TEST_CHECK(at(box, 2, 2, 1.4F));
    TEST_CHECK(!at(box, 2, 2, 1.2F));
    // A radius over half the shorter side reads as half of it.
    const auto pill = rounded(0, 0, 40, 20, 50);
    TEST_CHECK(at(pill, 3, 3));
    TEST_CHECK(at(pill, 20, 0));
    TEST_CHECK(!at(pill, 1, 1));
  }

  void testCircle() {
    const auto disc = circle(50, 50, 10);
    TEST_CHECK(at(disc, 50, 50));
    TEST_CHECK(at(disc, 60, 50));
    TEST_CHECK(at(disc, 56, 58));
    TEST_CHECK(!at(disc, 57.1F, 57.1F));
    TEST_CHECK(at(disc, 62, 50, 2));
    TEST_CHECK(!at(disc, 62.1F, 50, 2));
    TEST_CHECK(!at(circle(0, 0, 0), 0, 0));
  }

  void testSector() {
    // A quarter ring from 12 o'clock to 3 o'clock between radius 20 and 40.
    const auto quarter = sector(20, 40, 0, 90);
    TEST_CHECK(at(quarter, 100, 70));  // 30 px straight up
    TEST_CHECK(at(quarter, 130, 100)); // 30 px right
    TEST_CHECK(at(quarter, 121, 79));  // 45 degrees, 29.7 px out
    TEST_CHECK(!at(quarter, 70, 100)); // 9 o'clock
    TEST_CHECK(!at(quarter, 100, 130));
    TEST_CHECK(!at(quarter, 100, 85)); // inside the hole
    TEST_CHECK(at(quarter, 100, 80));  // on the inner edge
    TEST_CHECK(at(quarter, 100, 60));  // on the outer edge
    TEST_CHECK(!at(quarter, 100, 59));
    // Half a degree inside the 3 o'clock edge at radius 30, and half a degree past it.
    TEST_CHECK(at(quarter, 129.9989F, 99.7382F));
    TEST_CHECK(!at(quarter, 129.9989F, 100.2618F));
    TEST_CHECK(at(quarter, 100, 59, 1));
    // 2 px left of the 12 o'clock edge: outside, then inside with 2 px of tolerance.
    TEST_CHECK(!at(quarter, 98, 70));
    TEST_CHECK(at(quarter, 98, 70, 2));
    TEST_CHECK(!at(quarter, 98, 70, 1.9F));
    // An arc that crosses 12 o'clock, given with a negative start.
    const auto across = sector(20, 40, -30, 30);
    TEST_CHECK(at(across, 100, 70));
    TEST_CHECK(at(across, 90, 72));
    TEST_CHECK(!at(across, 130, 100));
    // A full turn or more is a whole ring.
    const auto ring = sector(20, 40, 10, 400);
    TEST_CHECK(at(ring, 70, 100));
    TEST_CHECK(at(ring, 100, 130));
    TEST_CHECK(!at(ring, 100, 100));
    // A sector wider than a half turn holds its back side too.
    const auto wide = sector(0, 40, 0, 270);
    TEST_CHECK(at(wide, 100, 120));
    TEST_CHECK(!at(wide, 80, 90));
    // Reversed angles and an empty band hold nothing.
    TEST_CHECK(!at(sector(20, 40, 90, 0), 130, 100));
    TEST_CHECK(!at(sector(40, 20, 0, 90), 130, 100));
  }

  void testRibbon() {
    const auto band = ribbon();
    // At x 50 both curves are halfway down: top 20, bottom 40.
    TEST_CHECK(at(band, 50, 30));
    TEST_CHECK(at(band, 50, 20.01F));
    TEST_CHECK(!at(band, 50, 19.9F));
    TEST_CHECK(!at(band, 50, 40.1F));
    TEST_CHECK(at(band, 0, 0));
    TEST_CHECK(at(band, 100, 60));
    TEST_CHECK(!at(band, 100.5F, 50));
    TEST_CHECK(at(band, 101, 50, 1));
    TEST_CHECK(at(band, 50, 18.6F, 1.5F));
    TEST_CHECK(!at(band, 50, 18.4F, 1.5F));
    const auto span = ribbonSpanAt(band.ribbon, 50);
    TEST_CHECK(span.has_value());
    TEST_NEAR(span->top, 20.0F, 1e-3F);
    TEST_NEAR(span->bottom, 40.0F, 1e-3F);
    TEST_CHECK(!ribbonSpanAt(band.ribbon, 120).has_value());
    TEST_CHECK(!ribbonSpanAt(band.ribbon, kNan).has_value());
    auto reversed = band;
    reversed.ribbon.top[3].x = -10;
    TEST_CHECK(!at(reversed, 0, 5));
  }

  void testLine() {
    const auto horizontal = line({.x = 0, .y = 0}, {.x = 100, .y = 0}, 4);
    TEST_CHECK(at(horizontal, 50, 2));
    TEST_CHECK(!at(horizontal, 50, 2.1F));
    TEST_CHECK(at(horizontal, 50, 3, 1));
    TEST_CHECK(!at(horizontal, 101, 0)); // butt ends
    TEST_CHECK(at(horizontal, 101, 0, 1));
    const auto diagonal = line({.x = 0, .y = 0}, {.x = 10, .y = 10}, 2);
    TEST_CHECK(at(diagonal, 5, 5));
    TEST_CHECK(at(diagonal, 5.7F, 5));
    TEST_CHECK(!at(diagonal, 6, 4));
    TEST_CHECK(!at(line({.x = 3, .y = 3}, {.x = 3, .y = 3}, 4), 3, 3));
  }

  void testBadInputsHoldNothing() {
    TEST_CHECK(!at(rect(0, 0, 10, 10), kNan, 5));
    TEST_CHECK(!at(rect(0, 0, 10, 10), 5, kInf));
    TEST_CHECK(!at(rect(0, 0, kNan, 10), 5, 5));
    TEST_CHECK(!at(rect(0, 0, -10, 10), -5, 5));
    TEST_CHECK(!at(rect(0, 0, 0, 10), 0, 5));
    // A tolerance that is negative, NaN or infinite reads as 0.
    TEST_CHECK(!at(rect(0, 0, 10, 10), 11, 5, -3));
    TEST_CHECK(!at(rect(0, 0, 10, 10), 11, 5, kNan));
    TEST_CHECK(!at(rect(0, 0, 10, 10), 11, 5, kInf));
    TEST_CHECK(at(rect(0, 0, 10, 10), 10, 5, kNan));
    TEST_CHECK(!at(circle(kInf, 0, 10), 0, 0));
    TEST_CHECK(!shapeValid(sector(20, kNan, 0, 90)));
    TEST_CHECK(shapeValid(sector(20, 40, 0, 90)));
  }

  void testTopmostShape() {
    std::vector<Shape> shapes;
    shapes.push_back(rect(0, 0, 100, 100));
    shapes.push_back(rect(20, 20, 20, 20));
    shapes.push_back(rect(30, 30, 20, 20));
    shapes[0].id = {.node = 7, .role = 0};
    // Later in the list wins among equal orders.
    TEST_CHECK(hitShape(shapes, {.x = 35, .y = 35}, 0) == std::optional<std::size_t>(2));
    TEST_CHECK(hitShape(shapes, {.x = 25, .y = 25}, 0) == std::optional<std::size_t>(1));
    TEST_CHECK(hitShape(shapes, {.x = 5, .y = 5}, 0) == std::optional<std::size_t>(0));
    // A higher order wins over list position.
    shapes[1].order = 5;
    TEST_CHECK(hitShape(shapes, {.x = 35, .y = 35}, 0) == std::optional<std::size_t>(1));
    // Shapes that are not hit targets, or fully faded, are passed over.
    shapes[1].hittable = false;
    TEST_CHECK(hitShape(shapes, {.x = 35, .y = 35}, 0) == std::optional<std::size_t>(2));
    shapes[2].opacity = 0;
    TEST_CHECK(hitShape(shapes, {.x = 35, .y = 35}, 0) == std::optional<std::size_t>(0));
    TEST_CHECK(!hitShape(shapes, {.x = 105, .y = 50}, 0).has_value());
    TEST_CHECK(hitShape(shapes, {.x = 105, .y = 50}, 5) == std::optional<std::size_t>(0));
    TEST_CHECK(!hitShape({}, {.x = 0, .y = 0}, 0).has_value());
    TEST_CHECK(!hitShape(shapes, {.x = kNan, .y = 0}, 0).has_value());
  }
  void testConstructors() {
    const ShapeId id{.node = 3, .role = 7};
    const auto plain = rectShape(id, 1, 2, 3, 4);
    TEST_CHECK(plain.id == id && plain.kind == ShapeKind::Rect);
    TEST_CHECK((plain.rect == ShapeRect{.x = 1, .y = 2, .width = 3, .height = 4, .radius = 0}));
    TEST_CHECK(rectShape(id, 1, 2, 3, 4, 0.5).kind == ShapeKind::RoundedRect);
    const auto ring = sectorShape(id, {.x = 5, .y = 6}, 10, 20, -30, 30);
    TEST_CHECK(ring.kind == ShapeKind::Sector);
    TEST_CHECK((ring.sector == ShapeSector{.center = {.x = 5, .y = 6}, .r0 = 10, .r1 = 20, .a0 = -30, .a1 = 30}));
    const auto disc = circleShape(id, {.x = 1, .y = 1}, 2);
    TEST_CHECK(disc.kind == ShapeKind::Circle && disc.circle.radius == 2);
    const auto stroke = lineShape(id, {.x = 0, .y = 0}, {.x = 4, .y = 0}, 1);
    TEST_CHECK(stroke.kind == ShapeKind::Line && stroke.line.to.x == 4 && stroke.line.width == 1);
  }
  // A new shape has no hatch color and no soft edge; a soft edge draws past the geometry but takes no hits there.
  void testSoftEdgeTakesNoHits() {
    const Shape plain;
    TEST_CHECK(!plain.hatch.has_value() && plain.softness == 0);
    auto soft = rounded(10, 10, 40, 20, 4);
    soft.softness = 8;
    TEST_CHECK(shapeValid(soft));
    TEST_CHECK(at(soft, 30, 20) && at(soft, 50, 20));
    TEST_CHECK(!at(soft, 53, 20) && !at(soft, 30, 34));
    const std::vector<Shape> shapes{rect(0, 0, 100, 100), soft};
    TEST_CHECK(hitShape(shapes, {.x = 53, .y = 20}, 0) == std::optional<std::size_t>(0));
    TEST_CHECK(hitShape(shapes, {.x = 30, .y = 20}, 0) == std::optional<std::size_t>(1));
  }
} // namespace

int main() {
  testConstructors();
  testRectangleEdgesAndTolerance();
  testRoundedRectangleCorners();
  testCircle();
  testSector();
  testRibbon();
  testLine();
  testBadInputsHoldNothing();
  testTopmostShape();
  testSoftEdgeTakesNoHits();
  return 0;
}
