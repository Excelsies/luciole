#include "test_check.h"
#include "workbench/scene_zoom.h"

namespace {
  using namespace luciole::workbench;

  SceneShapes sample() {
    SceneShapes scene;
    Shape rect;
    rect.kind = ShapeKind::RoundedRect;
    rect.rect = {.x = 10, .y = 20, .width = 40, .height = 30, .radius = 4};
    rect.strokeWidth = 1.5F;
    scene.shapes.push_back(rect);
    Shape ring;
    ring.kind = ShapeKind::Sector;
    ring.sector = {.center = {.x = 100, .y = 80}, .r0 = 20, .r1 = 50, .a0 = 10, .a1 = 90};
    scene.shapes.push_back(ring);
    Shape line;
    line.kind = ShapeKind::Line;
    line.line = {.from = {.x = 0, .y = 4}, .to = {.x = 60, .y = 8}, .width = 3};
    scene.shapes.push_back(line);
    ShapeLabel label;
    label.text = "name";
    label.x = 12;
    label.y = 22;
    label.width = 36;
    label.height = 15;
    label.fontSize = 12;
    label.rotation = 90;
    label.icon.size = 16;
    label.icon.left = 2;
    label.icon.top = 3;
    label.trail = LabelTrail{};
    label.trail->text = "1 KiB";
    label.trail->gap = 4;
    label.trail->room = 30;
    label.trail->dx = 1;
    label.trail->dy = 2;
    label.trail->width = 20;
    scene.labels.push_back(label);
    return scene;
  }
} // namespace

int main() {
  // A factor of one leaves the scene as it is.
  {
    auto scene = sample();
    zoomScene(scene, 1.0);
    const auto base = sample();
    TEST_CHECK(scene.shapes == base.shapes);
    TEST_CHECK(scene.labels[0].x == base.labels[0].x && scene.labels[0].width == base.labels[0].width);
  }
  // Geometry scales, the text size, the angles and the stroke do not.
  {
    auto scene = sample();
    zoomScene(scene, 1.5);
    const auto& rect = scene.shapes[0].rect;
    TEST_CHECK(rect.x == 15 && rect.y == 30 && rect.width == 60 && rect.height == 45 && rect.radius == 6);
    TEST_CHECK(scene.shapes[0].strokeWidth == 1.5F);
    const auto& ring = scene.shapes[1].sector;
    TEST_CHECK(ring.center.x == 150 && ring.center.y == 120 && ring.r0 == 30 && ring.r1 == 75);
    TEST_CHECK(ring.a0 == 10 && ring.a1 == 90);
    const auto& line = scene.shapes[2].line;
    TEST_CHECK(line.to.x == 90 && line.to.y == 12 && line.width == 4.5F);
    const auto& label = scene.labels[0];
    TEST_CHECK(label.x == 18 && label.y == 33 && label.width == 54 && label.height == 22.5F);
    TEST_CHECK(label.fontSize == 12 && label.rotation == 90 && label.trail->fontSize == 11);
    TEST_CHECK(label.icon.size == 24 && label.icon.left == 3 && label.icon.top == 4.5F);
    TEST_CHECK(label.trail->gap == 6 && label.trail->room == 45 && label.trail->dx == 1.5F);
    TEST_CHECK(label.trail->dy == 3 && label.trail->width == 30);
  }
  // A factor that is not positive and finite changes nothing.
  {
    auto scene = sample();
    zoomScene(scene, 0.0);
    TEST_CHECK(scene.shapes == sample().shapes);
  }
  return 0;
}
