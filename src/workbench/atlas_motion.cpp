#include "workbench/atlas_motion.h"

#include <algorithm>
#include <cmath>

namespace luciole::workbench {
  namespace {
    constexpr double kWheelLimit = 400;   // px of one turn that count
    constexpr double kWheelZoom = 0.0022; // log zoom per px
    constexpr double kKeep = 0.6;         // share of the pending zoom left after one frame
    constexpr double kZoomFloor = 0.004;  // a smaller remainder is applied at once
    constexpr double kStaleZoom = 1.25;

    double finiteOr(double value, double fallback) { return std::isfinite(value) ? value : fallback; }
  } // namespace

  double easeInOutCubic(double t) {
    const double u = std::isnan(t) ? 0.0 : std::clamp(t, 0.0, 1.0);
    return u < 0.5 ? 4 * u * u * u : 1 - std::pow(-2 * u + 2, 3) / 2;
  }

  double wheelLogZoom(double delta) {
    if (!std::isfinite(delta)) {
      return 0;
    }
    return -std::clamp(delta, -kWheelLimit, kWheelLimit) * kWheelZoom;
  }

  void ZoomEase::add(double logZoom, double x, double y) {
    if (!std::isfinite(logZoom)) {
      return;
    }
    m_left += logZoom;
    m_x = finiteOr(x, 0);
    m_y = finiteOr(y, 0);
  }

  double ZoomEase::take(double elapsedMs) {
    const double elapsed = std::isnan(elapsedMs) ? 0.0 : std::max(0.0, elapsedMs);
    const double left = m_left * std::pow(kKeep, elapsed / kAtlasFrameMs);
    const double part = std::abs(m_left) < kZoomFloor ? m_left : m_left - left;
    m_left -= part;
    return part;
  }

  void ZoomEase::clear() { m_left = 0; }

  bool flightDone(const Flight& flight, double elapsedMs) {
    return !(flight.durationMs > 0) || std::isnan(elapsedMs) || elapsedMs >= flight.durationMs;
  }

  Camera flightCamera(const Flight& flight, double elapsedMs) {
    if (flightDone(flight, elapsedMs)) {
      return flight.to;
    }
    return interpolate(flight.from, flight.to, easeInOutCubic(elapsedMs / flight.durationMs));
  }

  double fadeOpacity(double elapsedMs, double durationMs) {
    if (!(durationMs > 0)) {
      return 0;
    }
    if (std::isnan(elapsedMs)) {
      return 1;
    }
    return 1 - easeInOutCubic(elapsedMs / durationMs);
  }

  Reframe reframe(const Camera& built, const Camera& now, ViewSize view) {
    if (!usableCamera(built) || !usableCamera(now) || !std::isfinite(view.width) || !std::isfinite(view.height)) {
      return {};
    }
    const double k = now.scale / built.scale;
    return {
        .k = k,
        .dx = view.width / 2 + (built.cx - now.cx) * now.scale - k * view.width / 2,
        .dy = view.height / 2 + (built.cy - now.cy) * now.scale - k * view.height / 2,
    };
  }

  void reframeShapes(SceneShapes& scene, const Reframe& frame) {
    const auto k = static_cast<float>(frame.k);
    const auto dx = static_cast<float>(frame.dx);
    const auto dy = static_cast<float>(frame.dy);
    for (auto& shape : scene.shapes) {
      if (shape.kind == ShapeKind::Circle) {
        shape.circle.center = {.x = dx + k * shape.circle.center.x, .y = dy + k * shape.circle.center.y};
        shape.circle.radius *= k;
      } else if (shape.kind == ShapeKind::RoundedRect) {
        auto& rect = shape.rect;
        rect.x = dx + k * (rect.x + rect.width / 2) - rect.width / 2;
        rect.y = dy + k * (rect.y + rect.height / 2) - rect.height / 2;
      }
    }
    for (auto& label : scene.labels) {
      const float middleX = dx + k * (label.x + label.width / 2);
      const float middleY = dy + k * (label.y + label.height / 2);
      label.x = middleX - label.width / 2;
      label.y = middleY - label.height / 2;
    }
  }

  bool reframeStale(const Reframe& frame, ViewSize view) {
    if (!std::isfinite(frame.k) || !(frame.k > 0) || !std::isfinite(frame.dx) || !std::isfinite(frame.dy)) {
      return true;
    }
    // Where the view center of the built scene lands now, relative to the view center.
    const double shiftX = frame.dx + (frame.k - 1) * view.width / 2;
    const double shiftY = frame.dy + (frame.k - 1) * view.height / 2;
    return std::abs(shiftX) > kAtlasMarginShare * view.width / 2
        || std::abs(shiftY) > kAtlasMarginShare * view.height / 2
        || std::abs(std::log(frame.k)) > std::log(kStaleZoom);
  }
} // namespace luciole::workbench
