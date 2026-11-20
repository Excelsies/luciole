#include "workbench/ring_field.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace luciole::workbench {
  namespace {
    constexpr float kPulseRise = 18; // approach rates toward the target, per second
    constexpr float kPulseFall = 4;
    constexpr float kOrbitSpeed = 0.12F; // yaw, radians per second
    constexpr float kPitchSway = 0.3F;   // radians either side of the resting pitch
    constexpr float kSwayRate = 0.09F;   // radians of sway phase per second
    constexpr float kRestPitch = 0.42F;
    constexpr float kFitMargin = 1.32F;
    constexpr float kNear = 0.1F;
    constexpr float kFarPast = 2; // the far plane's distance past the center, beyond the swollen sphere
    constexpr float kTwoPi = 2 * std::numbers::pi_v<float>;
    // A clock past this many seconds starts again, keeping float precision; a multiple of the sway period.
    constexpr float kSwayWrap = kTwoPi / kSwayRate * 8;
  } // namespace

  double ringStart(std::span<const float> samples, std::size_t window) {
    if (window == 0 || samples.size() < 2 * window) {
      return 0;
    }
    // Bounded by the span's size less one window.
    for (std::size_t i = 1; i < samples.size() - window; ++i) {
      const float before = samples[i - 1];
      const float at = samples[i];
      if (before < 0 && at >= 0) {
        return static_cast<double>(i - 1) + (static_cast<double>(before) / (static_cast<double>(before) - at));
      }
    }
    return 0;
  }

  float resampleRing(std::span<const float> samples, double start, std::size_t window, std::span<float> out) {
    if (samples.empty() || out.empty()) {
      std::ranges::fill(out, 0.0F);
      return 0;
    }
    const std::size_t last = samples.size() - 1;
    const double step = static_cast<double>(window) / static_cast<double>(out.size());
    float peak = 0;
    for (std::size_t j = 0; j < out.size(); ++j) {
      const double x = std::clamp(start + (static_cast<double>(j) * step), 0.0, static_cast<double>(last));
      const auto i0 = static_cast<std::size_t>(x);
      const std::size_t i1 = std::min(i0 + 1, last);
      const auto f = static_cast<float>(x - static_cast<double>(i0));
      const float value = (samples[i0] * (1 - f)) + (samples[i1] * f);
      out[j] = value;
      peak = std::max(peak, std::abs(value));
    }
    return peak;
  }

  float nextRingPeak(float tracked, float peak) { return std::max(peak, tracked * kRingPeakDecay); }

  void finishRing(std::span<float> ring, float tracked) {
    const std::size_t size = ring.size();
    const auto taper = static_cast<std::size_t>(static_cast<float>(size) * kRingTaper);
    const float scale = 1 / std::max(tracked, kRingPeakFloor);
    for (std::size_t j = 0; j < size; ++j) {
      const std::size_t fromEnd = std::min(j, size - 1 - j);
      float weight = 1;
      if (fromEnd < taper) {
        weight = 0.5F - (0.5F * std::cos(std::numbers::pi_v<float> * static_cast<float>(fromEnd) / taper));
      }
      ring[j] = std::clamp(ring[j] * scale * weight, -kRingClamp, kRingClamp);
    }
  }

  float ringPulseTarget(float bass) {
    if (std::isnan(bass)) {
      return 0;
    }
    return std::clamp((bass - 1) * 1.2F, 0.0F, kRingPulseMax);
  }

  float stepRingPulse(float pulse, float bass, float seconds) {
    const float target = ringPulseTarget(bass);
    const float rate = target > pulse ? kPulseRise : kPulseFall;
    return pulse + ((target - pulse) * (1 - std::exp(-seconds * rate)));
  }

  float ringSwell(float pulse) { return 1 + (kRingSwell * pulse); }

  RingCamera stepRingCamera(const RingCamera& camera, float seconds) {
    RingCamera next = camera;
    next.yaw = std::fmod(camera.yaw + (kOrbitSpeed * seconds), kTwoPi);
    next.sway = std::fmod(camera.sway + seconds, kSwayWrap);
    next.pitch = kRestPitch + (kPitchSway * std::sin(kSwayRate * next.sway));
    return next;
  }

  RingView ringView(const RingCamera& camera, float aspect) {
    if (!std::isfinite(aspect) || aspect <= 0) {
      aspect = 1;
    }
    const float fit = (1 + kRingAmplitude) * kFitMargin / std::tan(kRingFieldOfView / 2);
    RingView out;
    out.distance = aspect < 1 ? fit / aspect : fit;

    const float focal = 1 / std::tan(kRingFieldOfView / 2);
    const float far = out.distance + kFarPast;
    auto& p = out.projection;
    p[0] = focal / aspect;
    p[5] = focal;
    p[10] = (far + kNear) / (kNear - far);
    p[11] = -1;
    p[14] = 2 * far * kNear / (kNear - far);

    // The eye sits on the orbit sphere looking at the origin; its x axis stays level.
    const float d = out.distance;
    const float cosPitch = std::cos(camera.pitch);
    const std::array<float, 3> back{
        cosPitch * std::sin(camera.yaw), std::sin(camera.pitch), cosPitch * std::cos(camera.yaw)
    };
    const float level = std::max(std::hypot(back[0], back[2]), 1e-6F);
    const std::array<float, 3> right{back[2] / level, 0, -back[0] / level};
    const std::array<float, 3> up{
        (back[1] * right[2]) - (back[2] * right[1]), (back[2] * right[0]) - (back[0] * right[2]),
        (back[0] * right[1]) - (back[1] * right[0])
    };
    auto& v = out.view;
    for (std::size_t c = 0; c < 3; ++c) {
      v[(c * 4) + 0] = right[c];
      v[(c * 4) + 1] = up[c];
      v[(c * 4) + 2] = back[c];
    }
    // The eye is back * d, so its own coordinates are (0, 0, d) and the origin lands at (0, 0, -d).
    v[14] = -d;
    v[15] = 1;
    return out;
  }

  RingField::RingField() : m_rows(kRingCount * kRingPoints, 0.0F) {}

  void RingField::reset() {
    std::ranges::fill(m_rows, 0.0F);
    m_head = 0;
    m_count = 0;
    m_pushes = 0;
    m_fraction = 0;
    m_tracked = 0.1F;
    m_pulse = 0;
    m_camera = RingCamera{};
  }

  void RingField::advance(const AudioFrame& frame) {
    const float seconds = frameStep(frame);
    m_camera = stepRingCamera(m_camera, seconds);
    m_pulse = stepRingPulse(m_pulse, bassRatio(frame), seconds);
    const float clock = m_fraction + (seconds * static_cast<float>(kRingCount) / kRingTravelSeconds);
    const float due = std::floor(clock);
    m_fraction = std::clamp(clock - due, 0.0F, std::nextafter(1.0F, 0.0F));
    const auto rings = std::min(static_cast<std::size_t>(due), kRingCount);
    for (std::size_t k = 0; k < rings; ++k) {
      push(frame);
    }
  }

  void RingField::push(const AudioFrame& frame) {
    // The newest samples, oldest first; a short waveform is padded with silence in front.
    const auto size = static_cast<std::ptrdiff_t>(frame.waveform.size());
    const auto first = size - static_cast<std::ptrdiff_t>(m_window.size());
    for (std::size_t k = 0; k < m_window.size(); ++k) {
      const std::ptrdiff_t index = first + static_cast<std::ptrdiff_t>(k);
      m_window[k] = index < 0 ? 0.0F : waveformSample(frame, static_cast<std::size_t>(index));
    }
    m_head = (m_head + 1) % kRingCount;
    const std::span ring = std::span(m_rows).subspan(m_head * kRingPoints, kRingPoints);
    const double start = ringStart(m_window, kRingWindow);
    m_tracked = nextRingPeak(m_tracked, resampleRing(m_window, start, kRingWindow, ring));
    finishRing(ring, m_tracked);
    m_count = std::min(m_count + 1, kRingCount);
    ++m_pushes;
  }
} // namespace luciole::workbench
