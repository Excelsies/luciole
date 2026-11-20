#include "workbench/spectrum_meter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace luciole::workbench {
  namespace {
    constexpr float kFramesPerSecond = 60;
    constexpr float kMaxKeep = 0.999F;
    // A step longer than this eases no further, so an absurd step stays finite.
    constexpr float kMaxSeconds = 60;
  } // namespace

  float easeToward(float value, float target, float keep, float seconds) {
    if (std::isnan(target)) {
      target = 0;
    }
    if (std::isnan(value)) {
      return target;
    }
    const float k = std::isnan(keep) ? 0.0F : std::clamp(keep, 0.0F, kMaxKeep);
    const float s = std::isnan(seconds) ? 0.0F : std::clamp(seconds, 0.0F, kMaxSeconds);
    const float share = std::pow(k, s * kFramesPerSecond);
    return target + ((value - target) * share);
  }

  void smoothLevels(std::span<float> levels, std::span<const float> targets, float keep, float seconds) {
    const std::size_t count = std::min(levels.size(), targets.size());
    for (std::size_t i = 0; i < count; ++i) {
      const float target = std::isnan(targets[i]) ? 0.0F : targets[i];
      levels[i] = std::max(target, easeToward(levels[i], target, keep, seconds));
    }
  }

  void groupLoudest(std::span<const float> values, std::span<float> out) {
    const std::size_t groups = out.size();
    for (std::size_t group = 0; group < groups; ++group) {
      const std::size_t first = group * values.size() / groups;
      const std::size_t end = (group + 1) * values.size() / groups;
      float loudest = 0;
      for (std::size_t i = first; i < end; ++i) {
        loudest = std::max(loudest, values[i]);
      }
      out[group] = loudest;
    }
  }

  void readBands(const AudioFrame& frame, std::span<float> out) {
    const std::size_t count = std::min(out.size(), kAudioBandCount);
    for (std::size_t i = 0; i < count; ++i) {
      out[i] = spectrumBand(frame, i);
    }
  }

  void PeakCaps::reset() {
    m_peaks.fill(0);
    m_hold.fill(0);
    m_speed.fill(0);
  }

  void PeakCaps::advance(std::span<const float> levels, float seconds) {
    const float s = std::isnan(seconds) ? 0.0F : std::clamp(seconds, 0.0F, kMaxAudioFrameSeconds);
    const std::size_t count = std::min(levels.size(), m_peaks.size());
    for (std::size_t i = 0; i < count; ++i) {
      const float level = std::isnan(levels[i]) ? 0.0F : std::clamp(levels[i], 0.0F, 1.0F);
      if (level >= m_peaks[i]) {
        m_peaks[i] = level;
        m_hold[i] = kPeakHoldSeconds;
        m_speed[i] = 0;
      } else if (m_hold[i] > 0) {
        m_hold[i] = std::max(0.0F, m_hold[i] - s);
      } else {
        m_speed[i] += kPeakGravity * s;
        m_peaks[i] = std::max(level, m_peaks[i] - (m_speed[i] * s));
      }
    }
  }
} // namespace luciole::workbench
