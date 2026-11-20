#pragma once

#include "workbench/atlas_camera.h"
#include "workbench/pane_layout.h"
#include "workbench/scene_shapes.h"

// Timing of the Atlas's own motion (eased zoom, camera flights, cross-fades) and how a scene drawn through one camera
// is shown through another without being built again.
namespace luciole::workbench {
  inline constexpr double kAtlasFrameMs = 1000.0 / 60;
  // A scene is built this share of each side beyond the view, so a pan shows no empty edge before the next build.
  inline constexpr double kAtlasMarginShare = 0.25;

  // Cubic ease in and out; t is clamped to 0..1 and NaN reads as 0.
  double easeInOutCubic(double t);

  // Log zoom of a wheel turn of `delta` px, positive down: 0.0022 per px of at most 400 px, zooming in for a turn up.
  // 0 for a delta that is not finite.
  double wheelLogZoom(double delta);

  // Zoom still to apply, on a log scale, about one view point. Wheel turns, zoom keys and buttons add to it; each
  // 1/60 s takes 40% of what is left, and a remainder under 0.004 goes at once.
  class ZoomEase {
  public:
    // Adds `logZoom` about (x, y). A zoom that is not finite is ignored; a point that is not finite reads as 0.
    void add(double logZoom, double x, double y);
    // The log zoom to apply for `elapsedMs` more; an infinite time takes all, one that is not a number or negative
    // reads as 0.
    double take(double elapsedMs);
    void clear();
    bool pending() const noexcept { return m_left != 0; }
    double x() const noexcept { return m_x; }
    double y() const noexcept { return m_y; }

  private:
    double m_left = 0;
    double m_x = 0;
    double m_y = 0;
  };

  // A camera flight: zoom on a log scale and pan so motion on screen is even, along an eased progress.
  struct Flight {
    Camera from;
    Camera to;
    double durationMs = 0;
  };

  // The camera `elapsedMs` into the flight; `to` once it is done.
  Camera flightCamera(const Flight& flight, double elapsedMs);
  // True from durationMs on, and at once for a duration or time that is not a number or not positive.
  bool flightDone(const Flight& flight, double elapsedMs);

  // Opacity of the old picture `elapsedMs` into a cross-fade of `durationMs`; 0 once it is over or for a duration
  // that is not positive, and 1 for a time that is not a number.
  double fadeOpacity(double elapsedMs, double durationMs);

  // How a scene drawn through one camera maps into the view through another: p_now = d + k * p_built per axis.
  struct Reframe {
    double k = 1;
    double dx = 0;
    double dy = 0;
  };

  // The map from `built` to `now` in a view of `view`; the identity when either camera cannot be used.
  Reframe reframe(const Camera& built, const Camera& now, ViewSize view);

  // Moves circles, and the middles of rounded rectangles and labels, which keep their size: chips and pills stay sized
  // to their text. Plain rectangles stay, as in an Atlas scene they are the view's own: a rim that covers it.
  void reframeShapes(SceneShapes& scene, const Reframe& frame);

  // True when a scene built with kAtlasMarginShare of margin must be built again: the view center moved more than
  // half the margin on either axis, or the zoom changed by more than 25%; also for a map that is not finite.
  bool reframeStale(const Reframe& frame, ViewSize view);
} // namespace luciole::workbench
