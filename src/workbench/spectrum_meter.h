#pragma once

#include "workbench/audio_frame.h"

#include <array>
#include <span>

// Level meter math the sound visuals share: easing, smoothed levels, bands merged into fewer bars, and peak caps.
namespace luciole::workbench {
  // Seconds a peak cap stays put before it falls, and its fall's acceleration in full heights per second squared.
  inline constexpr float kPeakHoldSeconds = 0.45F;
  inline constexpr float kPeakGravity = 3.2F;

  // `value` moved toward `target` for `seconds`, keeping `keep` of the gap per 60th of a second. A target that is not
  // a number reads as 0, a value that is not a number as the target, and a keep or a step that is not a number as 0;
  // keep is held within [0, 1).
  float easeToward(float value, float target, float keep, float seconds);

  // Fast attack, eased release: a level jumps up to a higher target and otherwise eases toward it. Pairs the two
  // spans up to the shorter one.
  void smoothLevels(std::span<float> levels, std::span<const float> targets, float keep, float seconds);

  // Each of `out`'s values is the loudest of its share of `values`, split into out.size() groups as equal as they
  // can be, so a narrow peak still shows in fewer bars; 0 for an empty input.
  void groupLoudest(std::span<const float> values, std::span<float> out);

  // The frame's kAudioBandCount bands, each within [0, 1], into the first of `out`'s values.
  void readBands(const AudioFrame& frame, std::span<float> out);

  // A cap over each of up to kAudioBandCount levels: it jumps to a new high, holds kPeakHoldSeconds, then falls with
  // kPeakGravity, never below its level.
  class PeakCaps {
  public:
    void reset();
    void advance(std::span<const float> levels, float seconds);
    std::span<const float> peaks() const { return m_peaks; }

  private:
    std::array<float, kAudioBandCount> m_peaks{};
    std::array<float, kAudioBandCount> m_hold{};
    std::array<float, kAudioBandCount> m_speed{};
  };
} // namespace luciole::workbench
