#include "test_check.h"
#include "workbench/atlas_motion.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"

#include <limits>

namespace {
  using namespace luciole::workbench;

  constexpr double kExact = 1e-9;
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInfinity = std::numeric_limits<double>::infinity();

  void easingIsCubicInAndOut() {
    TEST_NEAR(easeInOutCubic(0), 0.0, kExact);
    TEST_NEAR(easeInOutCubic(0.25), 0.0625, kExact);
    TEST_NEAR(easeInOutCubic(0.5), 0.5, kExact);
    TEST_NEAR(easeInOutCubic(0.75), 0.9375, kExact);
    TEST_NEAR(easeInOutCubic(1), 1.0, kExact);
    TEST_NEAR(easeInOutCubic(-1), 0.0, kExact);
    TEST_NEAR(easeInOutCubic(3), 1.0, kExact);
    TEST_NEAR(easeInOutCubic(kNan), 0.0, kExact);
  }

  // Each frame applies 40% of what is left; a remainder under 0.004 goes at once.
  void zoomEasesOverFrames() {
    ZoomEase ease;
    TEST_CHECK(!ease.pending());
    TEST_NEAR(ease.take(kAtlasFrameMs), 0.0, kExact);
    ease.add(1.0, 10, 20);
    TEST_CHECK(ease.pending());
    TEST_NEAR(ease.x(), 10.0, kExact);
    TEST_NEAR(ease.y(), 20.0, kExact);
    TEST_NEAR(ease.take(kAtlasFrameMs), 0.4, kExact);
    TEST_NEAR(ease.take(kAtlasFrameMs), 0.24, kExact);
    // Two frames at once take what two single frames would.
    TEST_NEAR(ease.take(2 * kAtlasFrameMs), 0.2304, kExact);
    TEST_NEAR(ease.take(kInfinity), 0.1296, kExact);
    TEST_CHECK(!ease.pending());
    // A later turn moves the point, and turns add up.
    ease.add(0.5, 1, 2);
    ease.add(-0.25, 30, 40);
    TEST_NEAR(ease.x(), 30.0, kExact);
    TEST_NEAR(ease.y(), 40.0, kExact);
    TEST_NEAR(ease.take(kAtlasFrameMs), 0.1, kExact);
    ease.clear();
    TEST_CHECK(!ease.pending());
    ease.add(0.003, 0, 0);
    TEST_NEAR(ease.take(0), 0.003, kExact);
    TEST_CHECK(!ease.pending());
    // Values that cannot be used change nothing.
    ease.add(kNan, 5, 5);
    ease.add(kInfinity, 5, 5);
    TEST_CHECK(!ease.pending());
    ease.add(1, kNan, kInfinity);
    TEST_NEAR(ease.x(), 0.0, kExact);
    TEST_NEAR(ease.y(), 0.0, kExact);
    TEST_NEAR(ease.take(kNan), 0.0, kExact);
    TEST_NEAR(ease.take(-5), 0.0, kExact);
    TEST_NEAR(ease.take(kAtlasFrameMs), 0.4, kExact);
  }

  // 0.0022 of log zoom per px, at most 400 px of a turn; down zooms out.
  void wheelKeepsItsScale() {
    TEST_NEAR(wheelLogZoom(100), -0.22, kExact);
    TEST_NEAR(wheelLogZoom(-50), 0.11, kExact);
    TEST_NEAR(wheelLogZoom(1000), -0.88, kExact);
    TEST_NEAR(wheelLogZoom(-1000), 0.88, kExact);
    TEST_NEAR(wheelLogZoom(kNan), 0.0, kExact);
    TEST_NEAR(wheelLogZoom(kInfinity), 0.0, kExact);
  }

  void flightsZoomOnALogScaleWithEasing() {
    const Flight flight{
        .from = {.cx = 0, .cy = 0, .scale = 100}, .to = {.cx = 1, .cy = -2, .scale = 200}, .durationMs = 380
    };
    const auto start = flightCamera(flight, 0);
    TEST_NEAR(start.cx, 0.0, kExact);
    TEST_NEAR(start.scale, 100.0, kExact);
    TEST_CHECK(!flightDone(flight, 0));
    // Half way the eased progress is 0.5: the scale is the geometric mean, and the pan keeps screen motion even.
    const auto middle = flightCamera(flight, 190);
    TEST_NEAR(middle.scale, 141.42135623730951, 1e-9);
    TEST_NEAR(middle.cx, 0.58578643762690485, 1e-9);
    TEST_NEAR(middle.cy, -1.1715728752538097, 1e-9);
    const auto end = flightCamera(flight, 380);
    TEST_NEAR(end.cx, 1.0, kExact);
    TEST_NEAR(end.cy, -2.0, kExact);
    TEST_NEAR(end.scale, 200.0, kExact);
    TEST_CHECK(flightDone(flight, 380));
    TEST_CHECK(flightDone(flight, 9999));
    const Flight instant{.from = {.cx = 0, .cy = 0, .scale = 1}, .to = {.cx = 3, .cy = 0, .scale = 2}, .durationMs = 0};
    TEST_CHECK(flightDone(instant, 0));
    TEST_NEAR(flightCamera(instant, 0).cx, 3.0, kExact);
    const Flight broken{
        .from = {.cx = 0, .cy = 0, .scale = 1}, .to = {.cx = 3, .cy = 0, .scale = 2}, .durationMs = kNan
    };
    TEST_CHECK(flightDone(broken, 0));
    TEST_NEAR(flightCamera(broken, kNan).cx, 3.0, kExact);
  }

  void fadesEaseOut() {
    TEST_NEAR(fadeOpacity(0, 260), 1.0, kExact);
    TEST_NEAR(fadeOpacity(65, 260), 0.9375, kExact);
    TEST_NEAR(fadeOpacity(130, 260), 0.5, kExact);
    TEST_NEAR(fadeOpacity(260, 260), 0.0, kExact);
    TEST_NEAR(fadeOpacity(500, 260), 0.0, kExact);
    TEST_NEAR(fadeOpacity(10, 0), 0.0, kExact);
    TEST_NEAR(fadeOpacity(kNan, 260), 1.0, kExact);
  }

  // A scene drawn through one camera, shown through another: the same world point lands where the new camera puts it.
  void reframingMovesCirclesAndLabels() {
    const ViewSize view{.width = 400, .height = 300};
    const Camera built{.cx = 0, .cy = 0, .scale = 100};
    const Camera now{.cx = 0.5, .cy = 0, .scale = 200};
    const auto frame = reframe(built, now, view);
    TEST_NEAR(frame.k, 2.0, kExact);
    TEST_NEAR(frame.dx, -300.0, kExact);
    TEST_NEAR(frame.dy, -150.0, kExact);
    SceneShapes scene;
    scene.shapes.push_back(circleShape({.node = 1, .role = 0}, {.x = 200, .y = 150}, 10));
    scene.shapes.push_back(rectShape({.node = 2, .role = 0}, 0, 0, 400, 300));
    scene.shapes.push_back(rectShape({.node = 3, .role = 0}, 180, 140, 40, 20, 9));
    ShapeLabel label;
    label.node = 1;
    label.text = "a";
    label.x = 190;
    label.y = 145;
    label.width = 20;
    label.height = 10;
    scene.labels.push_back(label);
    reframeShapes(scene, frame);
    TEST_NEAR(scene.shapes[0].circle.center.x, 100.0F, 1e-4F);
    TEST_NEAR(scene.shapes[0].circle.center.y, 150.0F, 1e-4F);
    TEST_NEAR(scene.shapes[0].circle.radius, 20.0F, 1e-4F);
    // Plain rectangles are the view's own (a rim that covers it), so they stay.
    TEST_CHECK(scene.shapes[1].rect.x == 0 && scene.shapes[1].rect.width == 400);
    // Rounded ones, the chips and pills under labels, move with their middle and keep their size.
    TEST_NEAR(scene.shapes[2].rect.x, 80.0F, 1e-4F);
    TEST_NEAR(scene.shapes[2].rect.y, 140.0F, 1e-4F);
    TEST_NEAR(scene.shapes[2].rect.width, 40.0F, 1e-4F);
    TEST_NEAR(scene.shapes[2].rect.height, 20.0F, 1e-4F);
    // Labels keep their size; their middle moves.
    TEST_NEAR(scene.labels[0].x, 90.0F, 1e-4F);
    TEST_NEAR(scene.labels[0].y, 145.0F, 1e-4F);
    TEST_NEAR(scene.labels[0].width, 20.0F, 1e-4F);
    const auto same = reframe(built, built, view);
    TEST_NEAR(same.k, 1.0, kExact);
    TEST_NEAR(same.dx, 0.0, kExact);
    TEST_NEAR(same.dy, 0.0, kExact);
    // A camera that cannot be used leaves the scene where it is.
    const auto broken = reframe(built, {.cx = kNan, .cy = 0, .scale = 1}, view);
    TEST_NEAR(broken.k, 1.0, kExact);
    TEST_NEAR(broken.dx, 0.0, kExact);
  }

  // A scene built with a margin of a quarter of each side may be reused until the view center has moved half that
  // far, or the zoom changed by more than 25%.
  void aReframedSceneGoesStale() {
    const ViewSize view{.width = 400, .height = 200};
    const Camera built{.cx = 0, .cy = 0, .scale = 100};
    TEST_CHECK(!reframeStale(reframe(built, built, view), view));
    TEST_CHECK(!reframeStale(reframe(built, {.cx = 0.49, .cy = 0, .scale = 100}, view), view));
    TEST_CHECK(reframeStale(reframe(built, {.cx = 0.51, .cy = 0, .scale = 100}, view), view));
    TEST_CHECK(!reframeStale(reframe(built, {.cx = 0, .cy = 0.24, .scale = 100}, view), view));
    TEST_CHECK(reframeStale(reframe(built, {.cx = 0, .cy = 0.26, .scale = 100}, view), view));
    TEST_CHECK(!reframeStale(reframe(built, {.cx = 0, .cy = 0, .scale = 124}, view), view));
    TEST_CHECK(reframeStale(reframe(built, {.cx = 0, .cy = 0, .scale = 126}, view), view));
    TEST_CHECK(!reframeStale(reframe(built, {.cx = 0, .cy = 0, .scale = 81}, view), view));
    TEST_CHECK(reframeStale(reframe(built, {.cx = 0, .cy = 0, .scale = 79}, view), view));
    TEST_CHECK(reframeStale({.k = kNan, .dx = 0, .dy = 0}, view));
  }
} // namespace

int main() {
  easingIsCubicInAndOut();
  zoomEasesOverFrames();
  wheelKeepsItsScale();
  flightsZoomOnALogScaleWithEasing();
  fadesEaseOut();
  reframingMovesCirclesAndLabels();
  aReframedSceneGoesStale();
  return 0;
}
