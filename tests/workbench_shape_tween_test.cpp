#include "test_check.h"
#include "workbench/shape.h"
#include "workbench/shape_tween.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

// Counts heap bytes so a test can bound what a tween allocates.
namespace {
  std::size_t gAllocated = 0;
}

void* operator new(std::size_t size) {
  gAllocated += size;
  if (void* block = std::malloc(size == 0 ? 1 : size)) {
    return block;
  }
  throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  gAllocated += size;
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* block) noexcept { std::free(block); }

void operator delete(void* block, const std::nothrow_t& /*tag*/) noexcept { std::free(block); }

void operator delete(void* block, std::size_t /*size*/) noexcept { std::free(block); }

namespace {
  using namespace luciole::workbench;

  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
  constexpr float kEps = 1e-4F;

  Shape rect(NodeId node, float x, Rgba8 fill) {
    Shape shape;
    shape.id = {.node = node, .role = 0};
    shape.kind = ShapeKind::Rect;
    shape.rect = {.x = x, .y = 0, .width = 10, .height = 20, .radius = 0};
    shape.fill = fill;
    return shape;
  }

  Shape sector(NodeId node, float a0, float a1) {
    Shape shape;
    shape.id = {.node = node, .role = 0};
    shape.kind = ShapeKind::Sector;
    shape.sector = {.center = {.x = 50, .y = 50}, .r0 = 10, .r1 = 20, .a0 = a0, .a1 = a1};
    return shape;
  }

  Shape circle(NodeId node) {
    Shape shape;
    shape.id = {.node = node, .role = 0};
    shape.kind = ShapeKind::Circle;
    shape.circle = {.center = {.x = 5, .y = 5}, .radius = 4};
    return shape;
  }

  const Shape* find(const std::vector<Shape>& shapes, NodeId node, ShapeKind kind) {
    for (const auto& shape : shapes) {
      if (shape.id.node == node && shape.kind == kind) {
        return &shape;
      }
    }
    return nullptr;
  }

  constexpr Rgba8 kRed{.r = 255, .g = 0, .b = 0, .a = 255};
  constexpr Rgba8 kBlue{.r = 0, .g = 0, .b = 255, .a = 255};

  std::vector<Shape> before() { return {rect(1, 0, kRed), circle(2), sector(3, 350, 370), rect(4, 0, kRed)}; }

  std::vector<Shape> after() { return {rect(1, 100, kBlue), sector(3, 10, 30), circle(4), rect(5, 0, kBlue)}; }

  void testEndsAreExact() {
    const ShapeTween tween(before(), after());
    std::vector<Shape> out;
    tween.frame(0, out);
    TEST_CHECK(out == before());
    tween.frame(1, out);
    TEST_CHECK(out == after());
    tween.frame(-2, out);
    TEST_CHECK(out == before());
    tween.frame(7, out);
    TEST_CHECK(out == after());
    tween.frame(kNan, out);
    TEST_CHECK(out == before());
  }

  void testMiddle() {
    const ShapeTween tween(before(), after());
    std::vector<Shape> out;
    tween.frame(0.5F, out);
    TEST_CHECK(out.size() == 6);
    const auto* moved = find(out, 1, ShapeKind::Rect);
    TEST_CHECK(moved != nullptr);
    TEST_NEAR(moved->rect.x, 50.0F, kEps);
    TEST_NEAR(moved->opacity, 1.0F, kEps);
    // Halfway from red to blue in linear light is 0.5 of each, 188 once encoded; gamma space would give 128.
    TEST_CHECK(moved->fill.r == 188 && moved->fill.g == 0 && moved->fill.b == 188);
    const auto* leaving = find(out, 2, ShapeKind::Circle);
    TEST_CHECK(leaving != nullptr);
    TEST_NEAR(leaving->opacity, 0.5F, kEps);
    TEST_CHECK(leaving->circle == circle(2).circle);
    const auto* arriving = find(out, 5, ShapeKind::Rect);
    TEST_CHECK(arriving != nullptr);
    TEST_NEAR(arriving->opacity, 0.5F, kEps);
    // A shape whose kind changes cross-fades.
    const auto* oldKind = find(out, 4, ShapeKind::Rect);
    const auto* newKind = find(out, 4, ShapeKind::Circle);
    TEST_CHECK(oldKind != nullptr && newKind != nullptr);
    TEST_NEAR(oldKind->opacity, 0.5F, kEps);
    TEST_NEAR(newKind->opacity, 0.5F, kEps);
    tween.frame(0.25F, out);
    TEST_NEAR(find(out, 2, ShapeKind::Circle)->opacity, 0.75F, kEps);
    TEST_NEAR(find(out, 5, ShapeKind::Rect)->opacity, 0.25F, kEps);
    TEST_NEAR(find(out, 1, ShapeKind::Rect)->rect.x, 25.0F, kEps);
  }

  void testAnglesTakeTheShortWay() {
    // 350..370 to 10..30 turns 20 degrees forward, not 340 back.
    const ShapeTween forward(before(), after());
    std::vector<Shape> out;
    forward.frame(0.5F, out);
    const auto* turned = find(out, 3, ShapeKind::Sector);
    TEST_CHECK(turned != nullptr);
    TEST_NEAR(std::remainder(turned->sector.a0, 360.0F), 0.0F, kEps);
    TEST_NEAR(turned->sector.a1 - turned->sector.a0, 20.0F, kEps);
    // 10..30 to 340..400: back 30 degrees while the span grows from 20 to 60.
    const std::vector<Shape> from{sector(3, 10, 30)};
    const std::vector<Shape> to{sector(3, 340, 400)};
    const ShapeTween back(from, to);
    back.frame(0.5F, out);
    TEST_NEAR(std::remainder(out[0].sector.a0 - 355.0F, 360.0F), 0.0F, kEps);
    TEST_NEAR(out[0].sector.a1 - out[0].sector.a0, 40.0F, kEps);
  }

  void testRepeatedIdsPairInOrder() {
    const std::vector<Shape> from{rect(9, 0, kRed), rect(9, 10, kRed)};
    const std::vector<Shape> to{rect(9, 100, kRed), rect(9, 200, kRed)};
    const ShapeTween tween(from, to);
    std::vector<Shape> out;
    tween.frame(0.5F, out);
    TEST_CHECK(out.size() == 2);
    TEST_NEAR(out[0].rect.x, 50.0F, kEps);
    TEST_NEAR(out[1].rect.x, 105.0F, kEps);
  }

  void testAllocationBound() {
    constexpr std::size_t kCount = 1000;
    std::vector<Shape> from;
    std::vector<Shape> to;
    from.reserve(kCount);
    to.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
      from.push_back(rect(static_cast<NodeId>(i), 0, kRed));
      to.push_back(rect(static_cast<NodeId>(i + kCount / 2), 10, kBlue));
    }
    const std::size_t start = gAllocated;
    const ShapeTween tween(from, to);
    const std::size_t built = gAllocated - start;
    // The two lists, the pairing and sort scratch: a few words per shape beyond the copies.
    TEST_CHECK(built <= 2 * kCount * sizeof(Shape) + 8 * 2 * kCount * sizeof(std::size_t));
    TEST_CHECK(tween.capacity() == 2 * kCount);
    std::vector<Shape> out;
    out.reserve(tween.capacity());
    const std::size_t warm = gAllocated;
    for (const float progress : {0.0F, 0.3F, 0.7F, 1.0F}) {
      tween.frame(progress, out);
      TEST_CHECK(out.size() <= tween.capacity());
    }
    TEST_CHECK(gAllocated == warm);
    tween.frame(0.3F, out);
    TEST_CHECK(out.size() == kCount + kCount / 2);
  }

  void testEveryKindMoves() {
    auto disc = circle(1);
    auto bigger = circle(1);
    bigger.circle = {.center = {.x = 15, .y = 25}, .radius = 8};
    Shape band;
    band.id = {.node = 2, .role = 0};
    band.kind = ShapeKind::Ribbon;
    band.ribbon.top = {{{.x = 0, .y = 0}, {.x = 1, .y = 0}, {.x = 2, .y = 0}, {.x = 3, .y = 0}}};
    band.ribbon.bottom = {{{.x = 0, .y = 4}, {.x = 1, .y = 4}, {.x = 2, .y = 4}, {.x = 3, .y = 4}}};
    auto lower = band;
    for (auto& p : lower.ribbon.bottom) {
      p.y = 8;
    }
    Shape stroke;
    stroke.id = {.node = 3, .role = 0};
    stroke.kind = ShapeKind::Line;
    stroke.line = {.from = {.x = 0, .y = 0}, .to = {.x = 10, .y = 0}, .width = 1};
    auto longer = stroke;
    longer.line.to.x = 30;
    longer.line.width = 3;
    // A stroke that appears grows in its final color.
    auto outlined = rect(4, 0, kRed);
    outlined.stroke = kBlue;
    outlined.strokeWidth = 2;
    const std::vector<Shape> from{disc, band, stroke, rect(4, 0, kRed)};
    const std::vector<Shape> to{bigger, lower, longer, outlined};
    const ShapeTween tween(from, to);
    std::vector<Shape> out;
    tween.frame(0.5F, out);
    TEST_CHECK(out.size() == 4);
    TEST_NEAR(out[0].circle.center.x, 10.0F, kEps);
    TEST_NEAR(out[0].circle.radius, 6.0F, kEps);
    TEST_NEAR(out[1].ribbon.bottom[2].y, 6.0F, kEps);
    TEST_NEAR(out[1].ribbon.top[2].y, 0.0F, kEps);
    TEST_NEAR(out[2].line.to.x, 20.0F, kEps);
    TEST_NEAR(out[2].line.width, 2.0F, kEps);
    TEST_NEAR(out[3].strokeWidth, 1.0F, kEps);
    TEST_CHECK(out[3].stroke == kBlue);
  }

  // A shape leaving the scene, alone or as the old kind of a cross-fade, takes no hits while it fades.
  void testFadingOutIsNotHit() {
    const std::vector<Shape> from{rect(7, 0, kRed), rect(8, 50, kRed), rect(9, 100, kRed)};
    auto changed = circle(9);
    changed.circle = {.center = {.x = 105, .y = 10}, .radius = 4};
    const std::vector<Shape> to{rect(8, 50, kRed), changed};
    const ShapeTween tween(from, to);
    std::vector<Shape> out;
    tween.frame(0.95F, out);
    TEST_CHECK(!hitShape(out, {.x = 5, .y = 5}, 0).has_value());
    const auto kept = hitShape(out, {.x = 55, .y = 5}, 0);
    TEST_CHECK(kept.has_value() && out[*kept].id.node == 8);
    const auto swapped = hitShape(out, {.x = 105, .y = 10}, 0);
    TEST_CHECK(swapped.has_value() && out[*swapped].kind == ShapeKind::Circle);
    for (const auto& shape : out) {
      TEST_CHECK(shape.hittable == (shape.id.node == 8 || shape.kind == ShapeKind::Circle));
    }
    // The first list is returned unchanged at 0.
    tween.frame(0, out);
    TEST_CHECK(hitShape(out, {.x = 5, .y = 5}, 0).has_value());
  }

  void testEmptyLists() {
    const ShapeTween none({}, {});
    std::vector<Shape> out{rect(1, 0, kRed)};
    none.frame(0.5F, out);
    TEST_CHECK(out.empty());
  }
  // A hatch color moves in linear light, from the stroke color of a side that has none; softness moves along.
  void testHatchAndSoftnessMove() {
    auto a = rect(1, 0, kRed);
    a.stroke = kRed;
    auto b = rect(1, 0, kRed);
    b.hatch = kBlue;
    b.softness = 8;
    const ShapeTween tween(std::vector{a}, std::vector{b});
    std::vector<Shape> out;
    tween.frame(0.5F, out);
    TEST_CHECK(out.size() == 1 && out[0].hatch.has_value());
    TEST_CHECK(out[0].hatch->r == 188 && out[0].hatch->b == 188);
    TEST_NEAR(out[0].softness, 4.0F, kEps);
    // Without a hatch color on either side there is none between.
    const ShapeTween plain(std::vector{a}, std::vector{a});
    plain.frame(0.5F, out);
    TEST_CHECK(!out[0].hatch.has_value() && out[0].softness == 0);
  }
} // namespace

int main() {
  testEndsAreExact();
  testMiddle();
  testAnglesTakeTheShortWay();
  testRepeatedIdsPairInOrder();
  testAllocationBound();
  testEveryKindMoves();
  testFadingOutIsNotHit();
  testEmptyLists();
  testHatchAndSoftnessMove();
  return 0;
}
