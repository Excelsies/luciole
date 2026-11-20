#pragma once

#include "workbench/atlas_camera.h"
#include "workbench/atlas_motion.h"
#include "workbench/scene_shapes.h"

#include <chrono>
#include <optional>

namespace luciole {
  // The Atlas's own motion: the eased zoom, a camera flight and a cross-fade, stepped once per frame. The zoom takes
  // one frame's share each frame. Flights and fades follow the clock: a frame advances them by the time since the
  // last one, at most 100 ms, and the first frame after a rest by 1/60 s.
  class AtlasAnimation {
  public:
    // What one frame does: the log zoom to apply about (x, y) in the map, and the camera of a running flight.
    struct Step {
      double logZoom = 0;
      double x = 0;
      double y = 0;
      std::optional<workbench::Camera> camera;
    };

    bool moving() const noexcept { return m_zoom.pending() || m_flight || m_fade; }
    bool flying() const noexcept { return m_flight.has_value(); }

    // Adds to the eased zoom; a flight stops.
    void zoom(double logZoom, double x, double y);
    // Flies from `from` to `to` in `ms`; the pending zoom is dropped.
    void fly(const workbench::Camera& from, const workbench::Camera& to, double ms);
    // Fades `picture`, the map as it was, out over `ms`.
    void fade(workbench::SceneShapes picture, double ms);
    // Drops the pending zoom and the flight, as a drag or a new snapshot takes the camera.
    void stop();
    void dropFade();

    // Advances to `now`; with `still` (motion turned off) everything ends at once.
    Step step(std::chrono::steady_clock::time_point now, bool still);

    // The old picture and its opacity now; null when no fade runs.
    const workbench::SceneShapes* fading() const noexcept { return m_fade ? &m_fade->picture : nullptr; }
    double fadeOpacity() const;

  private:
    struct FlightRun {
      workbench::Flight flight;
      double elapsedMs = 0;
    };
    struct FadeRun {
      workbench::SceneShapes picture;
      double durationMs = 0;
      double elapsedMs = 0;
    };

    void wake();

    workbench::ZoomEase m_zoom;
    std::optional<FlightRun> m_flight;
    std::optional<FadeRun> m_fade;
    std::optional<std::chrono::steady_clock::time_point> m_tick;
  };
} // namespace luciole
