#pragma once

#include "workbench/audio_frame.h"
#include "workbench/hit_flash.h"
#include "workbench/spectrum_meter.h"

#include <array>
#include <cstddef>
#include <span>

// The state behind the Bars sound visual: spectrum bars with falling peak caps, a waveform line across them and a
// glow behind them that brightens on hits.
namespace luciole::workbench {
  // Points of the waveform line, each the mean of kWaveSamplesPerPoint of the newest samples.
  inline constexpr std::size_t kWavePoints = 128;
  inline constexpr std::size_t kWaveSamplesPerPoint = 4;
  // Logical px the slot of one bar and its gap is at least wide, while there are more than 8 bars.
  inline constexpr float kMinBarSlot = 9;

  // The bars for a box `width` logical px wide: 64, 32, 16 or 8, the most whose slots are at least kMinBarSlot wide.
  std::size_t barCount(float width);

  // Where the parts of the picture sit in a box, in buffer px from its top left corner with y growing down.
  struct BarLayout {
    std::size_t count = 0;
    float margin = 0;        // left and right of the bars
    float slot = 0;          // one bar and its gap
    float gap = 0;           // between two bars, at least one px
    float base = 0;          // bottom edge of the bars
    float top = 0;           // where the bar color reaches its lightest
    float maxHeight = 0;     // height of a full bar
    float capHeight = 0;     // height of a cap and of the shortest bar
    float room = 0;          // below the base, where the reflection shows
    float waveCenter = 0;    // the waveform's zero line
    float waveAmplitude = 0; // px of a full-scale sample
    float lineWidth = 0;     // the waveform line
    float glowWidth = 0;     // the soft stroke under it
  };

  // The layout of a box `width` by `height` buffer px at `scale` buffer px per logical px; all zero for a box
  // without area or a scale that is not positive.
  BarLayout barLayout(float width, float height, float scale);

  class BarScene {
  public:
    void reset();
    void advance(const AudioFrame& frame);
    // The levels of levels.size() bars and the caps of caps.size() bars, each the loudest of its group of bands.
    void bars(std::span<float> levels, std::span<float> caps) const;
    // The newest waveform as kWavePoints values from -1 to 1, oldest first; 0 in silence.
    std::span<const float> wave() const { return m_wave; }
    // Brightness of the glow behind the bars: 0.25 in silence, rising with the bass and on hits, 0 before the first
    // frame.
    float backdrop() const { return m_flash.value(); }

  private:
    std::array<float, kAudioBandCount> m_levels{};
    PeakCaps m_caps;
    std::array<float, kWavePoints> m_wave{};
    float m_glow = 0;
    float m_bass = 0;
    FlashLimiter m_flash;
  };
} // namespace luciole::workbench
