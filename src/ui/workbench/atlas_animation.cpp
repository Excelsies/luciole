#include "ui/workbench/atlas_animation.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace luciole {
  namespace {
    constexpr double kLongestStepMs = 100;
  } // namespace

  void AtlasAnimation::wake() {
    if (!moving()) {
      m_tick.reset();
    }
  }

  void AtlasAnimation::zoom(double logZoom, double x, double y) {
    wake();
    m_flight.reset();
    m_zoom.add(logZoom, x, y);
  }

  void AtlasAnimation::fly(const workbench::Camera& from, const workbench::Camera& to, double ms) {
    wake();
    m_zoom.clear();
    m_flight = FlightRun{.flight = {.from = from, .to = to, .durationMs = ms}, .elapsedMs = 0};
  }

  void AtlasAnimation::fade(workbench::SceneShapes picture, double ms) {
    wake();
    m_fade = FadeRun{.picture = std::move(picture), .durationMs = ms, .elapsedMs = 0};
  }

  void AtlasAnimation::stop() {
    m_zoom.clear();
    m_flight.reset();
  }

  void AtlasAnimation::dropFade() { m_fade.reset(); }

  AtlasAnimation::Step AtlasAnimation::step(std::chrono::steady_clock::time_point now, bool still) {
    double elapsed = workbench::kAtlasFrameMs;
    if (m_tick) {
      elapsed = std::clamp(std::chrono::duration<double, std::milli>(now - *m_tick).count(), 0.0, kLongestStepMs);
    }
    m_tick = now;
    if (still) {
      elapsed = std::numeric_limits<double>::infinity();
    }
    Step step;
    if (m_zoom.pending()) {
      step.x = m_zoom.x();
      step.y = m_zoom.y();
      step.logZoom = m_zoom.take(still ? elapsed : workbench::kAtlasFrameMs);
    }
    if (m_flight) {
      m_flight->elapsedMs += elapsed;
      step.camera = workbench::flightCamera(m_flight->flight, m_flight->elapsedMs);
      if (workbench::flightDone(m_flight->flight, m_flight->elapsedMs)) {
        m_flight.reset();
      }
    }
    if (m_fade) {
      m_fade->elapsedMs += elapsed;
      if (!(m_fade->elapsedMs < m_fade->durationMs)) {
        m_fade.reset();
      }
    }
    if (!moving()) {
      m_tick.reset();
    }
    return step;
  }

  double AtlasAnimation::fadeOpacity() const {
    return m_fade ? workbench::fadeOpacity(m_fade->elapsedMs, m_fade->durationMs) : 0.0;
  }
} // namespace luciole
